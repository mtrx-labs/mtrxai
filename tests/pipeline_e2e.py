#!/usr/bin/env python3
import json
import os
import pty
import select
import shutil
import stat
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


def check_setup_wizard(binary, root):
    config_home = root / "wizard-config"
    user_home = root / "wizard-home"
    user_home.mkdir()
    model_dir = user_home / "models"
    model_dir.mkdir()
    gguf = model_dir / "chief.gguf"
    gguf.write_bytes(b"mock gguf")
    env = os.environ.copy()
    env.update({"HOME": str(user_home), "XDG_CONFIG_HOME": str(config_home), "TERM": "xterm"})
    master, slave = pty.openpty()
    process = subprocess.Popen(
        [binary, "--init"], cwd=root, env=env,
        stdin=slave, stdout=slave, stderr=slave,
    )
    os.close(slave)
    output = bytearray()
    transcript = bytearray()

    def wait_for(needle, timeout=8):
        target = needle.encode()
        deadline = time.time() + timeout
        while target not in output and time.time() < deadline:
            readable, _, _ = select.select([master], [], [], 0.2)
            if readable:
                try:
                    received = os.read(master, 8192)
                    output.extend(received)
                    transcript.extend(received)
                except OSError:
                    break
        assert target in output, output.decode(errors="replace")

    def answer(needle, value):
        wait_for(needle)
        os.write(master, value)
        output.clear()

    try:
        answer("Choose a provider (1-4 or name)", b"1\n")
        answer("Path to GGUF model", b"~/models/chief.gguf\n")
        wait_for("2 / 3")
        answer("Choose a provider (1-4 or name)", b"3\n")
        answer("OpenAI-compatible API base URL", b"\n")
        answer("OpenAI model name", b"wizard-openai-model\n")
        answer("OpenAI API key (input is hidden)", b"wizard-openai-secret\n")
        wait_for("3 / 3")
        answer("Choose a provider (1-4 or name)", b"4\n")
        answer("Anthropic API base URL", b"\n")
        answer("Anthropic model name", b"wizard-anthropic-model\n")
        answer("Anthropic API key (input is hidden)", b"wizard-anthropic-secret\n")
        wait_for("Setup complete.")
        assert process.wait(timeout=5) == 0
        assert b"wizard-openai-secret" not in transcript
        assert b"wizard-anthropic-secret" not in transcript
        config_path = config_home / "mtrxai" / "config.json"
        config_text = config_path.read_text()
        config = json.loads(config_text)
        assert config["chief"]["provider"] == "gguf"
        assert config["chief"]["gguf_path"] == str(gguf)
        assert config["prompter"]["provider"] == "openai"
        assert config["prompter"]["model"] == "wizard-openai-model"
        assert config["prompter"]["base_url"] == "https://api.openai.com/v1"
        assert config["coder"]["provider"] == "anthropic"
        assert config["coder"]["model"] == "wizard-anthropic-model"
        assert "wizard-openai-secret" not in config_text
        assert "wizard-anthropic-secret" not in config_text
        credentials_path = config_path.parent / "credentials.json"
        assert json.loads(credentials_path.read_text()) == {
            "prompter": "wizard-openai-secret",
            "coder": "wizard-anthropic-secret",
        }
        assert stat.S_IMODE(credentials_path.stat().st_mode) == 0o600

        ui_master, ui_slave = pty.openpty()
        ui = subprocess.Popen(
            [binary, "--no-update-check"], cwd=root, env=env,
            stdin=ui_slave, stdout=ui_slave, stderr=ui_slave
        )
        os.close(ui_slave)
        ui_output = bytearray()
        ui_transcript = bytearray()

        def wait_ui(needle, timeout=8):
            target = needle.encode()
            deadline = time.time() + timeout
            while target not in ui_output and time.time() < deadline:
                readable, _, _ = select.select([ui_master], [], [], 0.2)
                if readable:
                    try:
                        received = os.read(ui_master, 8192)
                        ui_output.extend(received)
                        ui_transcript.extend(received)
                    except OSError:
                        break
            assert target in ui_output, ui_output.decode(errors="replace")
            ui_output.clear()

        try:
            wait_ui("Describe what you want to build")
            os.write(ui_master, b"/apikey prompter")
            wait_ui("/apikey prompter")
            os.write(ui_master, b"\n")
            wait_ui("API key for prompter")
            os.write(ui_master, b"new-tui-secret\n")
            wait_ui("Saved API key for prompter.")
            assert b"new-tui-secret" not in ui_transcript
            os.write(ui_master, b"x")
            wait_ui("Describe what you want to build")
            os.write(ui_master, b"/quit\n")
            assert ui.wait(timeout=5) == 0
            assert json.loads(credentials_path.read_text())["prompter"] == (
                "new-tui-secret"
            )
            assert stat.S_IMODE(credentials_path.stat().st_mode) == 0o600
        finally:
            if ui.poll() is None:
                ui.terminate()
                ui.wait(timeout=3)
            os.close(ui_master)
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=3)
        os.close(master)


def check_esc_cancellation(binary, root):
    """Esc during a running pipeline must abort the request and leave the
    session usable, so a follow-up task still runs to completion."""
    workspace = root / "esc-workspace"
    workspace.mkdir()
    chief_answers = [
        "<think>Chief thinking process</think>Chief brief: create hello.txt with the content hello-ok.",
        "<think>Chief thinking process</think>Chief brief: create hello.txt with hello-ok.",
    ]
    prompter_answers = [
        "<think>Prompter thinking process</think>Prompter brief: write hello.txt containing hello-ok.",
    ]
    coder_answers = [
        {"action": "write_file", "path": "hello.txt", "content": "hello-ok\n"},
        {"action": "finish", "summary": "Created hello.txt."},
    ]
    # A stage only consumes its scripted answer when the response was fully
    # delivered, so an Esc-aborted chief request does not shift later answers.
    served = {"chief": 0, "prompter": 0, "coder": 0}

    class Handler(BaseHTTPRequestHandler):
        def do_POST(self):
            body = json.loads(
                self.rfile.read(int(self.headers["Content-Length"])))
            model = str(body.get("model", ""))
            role = ("chief" if model.startswith("chief-")
                    else "prompter" if model.startswith("prompter-")
                    else "coder")
            index = served[role]
            if index >= len(chief_answers if role == "chief" else
                            prompter_answers if role == "prompter" else
                            coder_answers):
                raise AssertionError(f"unexpected request for {role}")
            pool = {"chief": chief_answers, "prompter": prompter_answers,
                    "coder": coder_answers}[role]
            answer = pool[index]
            content = (answer if isinstance(answer, str) else json.dumps(answer))
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.end_headers()
            # The chief streams slowly so the test can press Esc mid-request.
            interval = 0.25 if role == "chief" else 0.0
            try:
                for start in range(0, len(content), 9):
                    if interval:
                        time.sleep(interval)
                    event = json.dumps(
                        {"type": "content_block_delta",
                         "delta": {"type": "text_delta",
                                   "text": content[start:start + 9]}})
                    self.wfile.write(f"data: {event}\n\n".encode())
                    self.wfile.flush()
                self.wfile.write(b"data: {\"type\":\"message_stop\"}\n\n")
                self.wfile.flush()
                served[role] = index + 1
            except (BrokenPipeError, ConnectionResetError):
                pass  # the client aborted; leave `served` untouched

        def log_message(self, *_args):
            pass

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    config = {}
    for role in ("chief", "prompter", "coder"):
        config[role] = {
            "provider": "anthropic",
            "model": f"{role}-esc",
            "base_url": f"http://127.0.0.1:{server.server_port}",
            "api_key_env": "",
            "gguf_path": "",
            "context_size": 2048,
        }
    config_home = root / "esc-config"
    config_path = config_home / "mtrxai" / "config.json"
    config_path.parent.mkdir(parents=True)
    config_path.write_text(json.dumps(config))
    credentials_path = config_path.parent / "credentials.json"
    credentials_path.write_text(
        json.dumps({"chief": "esc-key", "prompter": "esc-key", "coder": "esc-key"}))
    credentials_path.chmod(0o600)
    env = os.environ.copy()
    env.update({
        "XDG_CONFIG_HOME": str(config_home),
        "XDG_STATE_HOME": str(root / "esc-state"),
        "TERM": "xterm",
    })
    master, slave = pty.openpty()
    process = subprocess.Popen(
        [binary, "--no-update-check"], cwd=workspace, env=env,
        stdin=slave, stdout=slave, stderr=slave
    )
    os.close(slave)
    output = bytearray()

    def wait_for(needle, timeout=20):
        target = needle.encode()
        deadline = time.time() + timeout
        while target not in output and time.time() < deadline:
            readable, _, _ = select.select([master], [], [], 0.2)
            if readable:
                try:
                    output.extend(os.read(master, 8192))
                except OSError:
                    break
        if target not in output:
            raise AssertionError(
                f"Timed out waiting for {needle!r}: "
                f"{output.decode(errors='replace')[-1500:]}")

    try:
        wait_for("Describe what you want to build")
        os.write(master, b"make hello.txt\n")
        output.clear()
        # Early chief tokens prove streaming started; Esc now interrupts the
        # open request instead of merely declining a review. The needle is a
        # single streamed fragment because ncurses only writes changed cells.
        wait_for("think")
        os.write(master, b"\x1b")
        wait_for("Cancelled. Completed stage outputs remain saved on disk.")
        time.sleep(0.3)
        os.write(master, b"\n")
        wait_for("Describe what you want to build")
        os.write(master, b"make hello.txt\n")
        output.clear()
        wait_for("Approve this exact file content?")
        os.write(master, b"y")
        wait_for("Created hello.txt.")
        os.write(master, b"\n")
        wait_for("Describe what you want to build")
        os.write(master, b"/quit\n")
        assert process.wait(timeout=10) == 0
        assert (workspace / "hello.txt").read_text() == "hello-ok\n"
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        server.shutdown()
        os.close(master)


def check_resume(binary, root):
    """A run that failed after the Prompter must resume without calling the
    Chief or Prompter again, and --resume without an id must skip a newer
    finished session."""
    workspace = root / "resume-workspace"
    workspace.mkdir()
    state_home = root / "resume-state"
    sessions_root = state_home / "mtrxai" / "sessions"
    sessions_root.mkdir(parents=True)

    # Newer than the real run but already complete: --resume must ignore it.
    finished = sessions_root / "29991231-235959-999"
    finished.mkdir()
    (finished / "request.md").write_text("must not be resumed\n")
    (finished / "manifest.json").write_text(
        json.dumps({"completed_stage": "finished",
                    "updated_at": "2999-12-31 23:59:59"}))

    chief_answer = ("<think>Chief thinking process</think>"
                    "Chief brief: create hello-resume.txt.")
    prompter_answer = ("<think>Prompter thinking process</think>"
                       "Prompter brief: write hello-resume.txt.")
    coder_answers = [
        {"action": "write_file", "path": "hello-resume.txt",
         "content": "resumed-ok\n"},
        {"action": "finish", "summary": "Created hello-resume.txt."},
    ]
    mid_turn_answers = [
        {"action": "write_file", "path": "hello-midturn.txt",
         "content": "midturn-ok\n"},
        {"action": "finish", "summary": "Created hello-midturn.txt."},
    ]
    bad_answers = [
        "Sorry, I cannot answer that.",
        {"action": "delete_everything"},
    ]
    bad_resume_answers = [
        {"action": "write_file", "path": "hello-bad.txt",
         "content": "bad-ok\n"},
        {"action": "finish", "summary": "Created hello-bad.txt."},
    ]
    bad_saved_retry_answers = ["still not a JSON action"]
    bad_saved_valid_answers = [
        {"action": "write_file", "path": "hello-saved.txt",
         "content": "saved-ok\n"},
        {"action": "finish", "summary": "Created hello-saved.txt."},
    ]
    phase_answers = {
        "first": coder_answers,
        "mid_turn": mid_turn_answers,
        "bad": bad_answers,
        "bad_resume": bad_resume_answers,
        "bad_saved_retry": bad_saved_retry_answers,
        "bad_saved_valid": bad_saved_valid_answers,
    }
    counts = {"chief": 0, "prompter": 0, "coder": 0}
    state = {"fail_first_coder": True, "phase": "first", "coder_request": 0}
    coder_bodies = []

    class Handler(BaseHTTPRequestHandler):
        def do_POST(self):
            body = json.loads(
                self.rfile.read(int(self.headers["Content-Length"])))
            model = str(body.get("model", ""))
            role = ("chief" if model.startswith("chief-")
                    else "prompter" if model.startswith("prompter-")
                    else "coder")
            counts[role] += 1
            if role == "coder":
                coder_bodies.append(body)
            if role == "coder" and state["fail_first_coder"]:
                state["fail_first_coder"] = False
                message = b"mock coder unavailable"
                self.send_response(500)
                self.send_header("Content-Type", "text/plain")
                self.send_header("Content-Length", str(len(message)))
                self.end_headers()
                self.wfile.write(message)
                return

            if role == "chief":
                content = chief_answer
            elif role == "prompter":
                content = prompter_answer
            else:
                answers = phase_answers[state["phase"]]
                index = state["coder_request"]
                state["coder_request"] += 1
                assert 0 <= index < len(answers), "unexpected coder request"
                answer = answers[index]
                content = answer if isinstance(answer, str) else json.dumps(answer)
                if index == 0:
                    # The first reply also exercises thinking persistence: the
                    # <think> block is stripped before the action is parsed and
                    # stored separately in the transcript (or rejected archive).
                    content = ("<think>Coder thinking process</think>" + content)

            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.end_headers()
            try:
                for start in range(0, len(content), 9):
                    event = json.dumps(
                        {"type": "content_block_delta",
                         "delta": {"type": "text_delta",
                                   "text": content[start:start + 9]}})
                    self.wfile.write(f"data: {event}\n\n".encode())
                    self.wfile.flush()
                self.wfile.write(b"data: {\"type\":\"message_stop\"}\n\n")
                self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError):
                pass

        def log_message(self, *_args):
            pass

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()

    config = {}
    for role in ("chief", "prompter", "coder"):
        config[role] = {
            "provider": "anthropic",
            "model": f"{role}-resume",
            "base_url": f"http://127.0.0.1:{server.server_port}",
            "api_key_env": "",
            "gguf_path": "",
            "context_size": 2048,
        }
    config_home = root / "resume-config"
    config_path = config_home / "mtrxai" / "config.json"
    config_path.parent.mkdir(parents=True)
    config_path.write_text(json.dumps(config))
    credentials_path = config_path.parent / "credentials.json"
    credentials_path.write_text(
        json.dumps({role: "resume-key" for role in ("chief", "prompter",
                                                    "coder")}))
    credentials_path.chmod(0o600)
    env = os.environ.copy()
    env.update({
        "XDG_CONFIG_HOME": str(config_home),
        "XDG_STATE_HOME": str(state_home),
        "TERM": "xterm",
    })

    def start(extra_args, cwd=workspace):
        master, slave = pty.openpty()
        process = subprocess.Popen(
            [binary, "--no-update-check"] + extra_args, cwd=cwd,
            env=env, stdin=slave, stdout=slave, stderr=slave
        )
        os.close(slave)
        return process, master, bytearray()

    def wait_for(output, master, needle, timeout=20):
        target = needle.encode()
        deadline = time.time() + timeout
        while target not in output and time.time() < deadline:
            readable, _, _ = select.select([master], [], [], 0.2)
            if readable:
                try:
                    output.extend(os.read(master, 8192))
                except OSError:
                    break
        if target not in output:
            raise AssertionError(
                f"Timed out waiting for {needle!r}: "
                f"{output.decode(errors='replace')[-1500:]}")

    def stop(process, master):
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        os.close(master)

    # First run: the Chief and Prompter succeed, then the Coder request fails.
    process, master, output = start([])
    try:
        wait_for(output, master, "Describe what you want to build")
        os.write(master, b"create hello-resume.txt\n")
        output.clear()
        wait_for(output, master, "Pipeline stopped:")
        wait_for(output, master, "Completed stage outputs remain saved")
        os.write(master, b"\n")
        wait_for(output, master, "Describe what you want to build")
        os.write(master, b"/quit\n")
        assert process.wait(timeout=10) == 0
    finally:
        stop(process, master)

    real_sessions = [entry for entry in sessions_root.iterdir()
                     if entry.name != finished.name]
    assert len(real_sessions) == 1, real_sessions
    session = real_sessions[0]
    manifest = json.loads((session / "manifest.json").read_text())
    assert manifest["completed_stage"] == "prompter", manifest
    assert (session / "chief.md").exists()
    assert (session / "prompt.md").exists()
    # The Coder failed while generating, but its initial context is already
    # durable. Resume must continue from it instead of adding a second copy.
    transcript = session / "coder" / "messages.jsonl"
    records = [json.loads(line)
               for line in transcript.read_text().splitlines()]
    assert [record["role"] for record in records] == ["system", "user"], records
    assert records[0]["content"].startswith("You are the coding agent")
    assert "Original user request:" in records[1]["content"]
    assert "create hello-resume.txt" in records[1]["content"]

    # Resume without an id: only the Coder may be called again.
    process, master, output = start(["--resume"])
    try:
        wait_for(output, master, "Resuming session")
        wait_for(output, master, "Approve this exact file content?")
        os.write(master, b"y")
        wait_for(output, master, "Created hello-resume.txt.")
        wait_for(output, master, "Finished.")
        os.write(master, b"\n")
        wait_for(output, master, "Describe what you want to build")
        # Type the command one key at a time. Before blocking input was
        # restored after a run, the 100 ms pump timeout silently dropped
        # characters typed at a human pace.
        for character in "/sessions\n":
            os.write(master, character.encode())
            time.sleep(0.15)
        wait_for(output, master, "Saved sessions")
        assert session.name.encode() in output
        assert finished.name.encode() in output
        os.write(master, b" ")
        wait_for(output, master, "Describe what you want to build")
        os.write(master, b"/quit\n")
        assert process.wait(timeout=10) == 0
    finally:
        stop(process, master)

    assert counts == {"chief": 1, "prompter": 1, "coder": 3}, counts
    assert (workspace / "hello-resume.txt").read_text() == "resumed-ok\n"
    manifest = json.loads((session / "manifest.json").read_text())
    assert manifest["completed_stage"] == "finished", manifest

    # The transcript has the durable conversation, not the failed attempt:
    # one system and initial user message, then the write turn and its result,
    # then the finish turn. Thinking is stored beside the content.
    records = [json.loads(line)
               for line in transcript.read_text().splitlines()]
    assert [record["role"] for record in records] == [
        "system", "user", "assistant", "user", "assistant"], records
    assert [record.get("turn", 0) for record in records] == [0, 0, 1, 1, 2]
    assert records[2]["thinking"] == "Coder thinking process"
    assert json.loads(records[2]["content"])["action"] == "write_file"
    assert records[3]["content"].startswith(
        "Tool result:\nSaved hello-resume.txt")
    assert json.loads(records[4]["content"])["action"] == "finish"
    assert (session / "coder_turn_1_thinking.md").read_text().strip() == \
        "Coder thinking process"
    # The finish request after resume carried the persisted tool result, so it
    # was a continuation rather than a fresh Coder conversation.
    finish_messages = coder_bodies[2]["messages"]
    assert [message["role"] for message in finish_messages] == [
        "user", "assistant", "user"], finish_messages
    assert "write_file" in finish_messages[1]["content"]
    assert "Saved hello-resume.txt" in finish_messages[2]["content"]

    # An explicit id of a finished session is reported without any model work.
    result = subprocess.run(
        [binary, "--resume", finished.name], cwd=workspace, env=env,
        stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=20)
    assert result.returncode == 0, result
    assert "already complete" in result.stdout
    # With every session finished, no id has nothing to do.
    result = subprocess.run(
        [binary, "--resume"], cwd=workspace, env=env,
        stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=20)
    assert result.returncode == 0, result
    assert "nothing to resume" in result.stdout

    result = subprocess.run(
        [binary, "--resume", "20000101-000000-000"], cwd=workspace, env=env,
        stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=20)
    assert result.returncode == 1, result
    assert "No saved session matches" in result.stderr

    # A session remembers its project directory; resuming from elsewhere must
    # stop before any model is called.
    foreign = sessions_root / "29991231-235959-998"
    foreign.mkdir()
    (foreign / "request.md").write_text("foreign task\n")
    (foreign / "manifest.json").write_text(
        json.dumps({"completed_stage": "request"}))
    (foreign / "project-root.txt").write_text(str(workspace.resolve()) + "\n")
    process, master, output = start(["--resume", foreign.name], cwd=root)
    try:
        wait_for(output, master, "This session belongs to")
        os.write(master, b"\n")
        wait_for(output, master, "Describe what you want to build")
        os.write(master, b"/quit\n")
        assert process.wait(timeout=10) == 0
    finally:
        stop(process, master)
    assert counts == {"chief": 1, "prompter": 1, "coder": 3}, counts
    shutil.rmtree(foreign)

    # Mid-turn resume: the Coder proposes a write, the process dies while the
    # approval screen is waiting, and resume must ask again rather than apply
    # the saved action automatically.
    mid_workspace = root / "resume-midturn"
    mid_workspace.mkdir()
    state["phase"] = "mid_turn"
    state["coder_request"] = 0
    process, master, output = start([], cwd=mid_workspace)
    try:
        wait_for(output, master, "Describe what you want to build")
        os.write(master, b"create hello-midturn.txt\n")
        output.clear()
        wait_for(output, master, "Approve this exact file content?")
        assert not (mid_workspace / "hello-midturn.txt").exists()
        process.kill()
        process.wait(timeout=5)
    finally:
        stop(process, master)

    mid_sessions = [entry for entry in sessions_root.iterdir()
                    if entry.name not in {finished.name, session.name}]
    assert len(mid_sessions) == 1, mid_sessions
    mid_session = mid_sessions[0]
    mid_transcript = mid_session / "coder" / "messages.jsonl"
    records = [json.loads(line)
               for line in mid_transcript.read_text().splitlines()]
    assert [record["role"] for record in records] == [
        "system", "user", "assistant"], records
    assert records[-1]["turn"] == 1
    assert json.loads(records[-1]["content"])["action"] == "write_file"
    manifest = json.loads((mid_session / "manifest.json").read_text())
    assert manifest["completed_stage"] == "coder-turn-1", manifest

    # Resume without an id sees the one unfinished session. The pending write
    # is not applied until it is approved again.
    process, master, output = start(["--resume"], cwd=mid_workspace)
    try:
        wait_for(output, master, "Resuming session")
        wait_for(output, master, "Approve this exact file content?")
        assert not (mid_workspace / "hello-midturn.txt").exists()
        os.write(master, b"y")
        wait_for(output, master, "Finished: Created hello-midturn.txt.")
        wait_for(output, master, "Finished.")
        os.write(master, b"\n")
        wait_for(output, master, "Describe what you want to build")
        os.write(master, b"/quit\n")
        assert process.wait(timeout=10) == 0
    finally:
        stop(process, master)

    assert counts == {"chief": 2, "prompter": 2, "coder": 5}, counts
    assert (mid_workspace / "hello-midturn.txt").read_text() == "midturn-ok\n"
    manifest = json.loads((mid_session / "manifest.json").read_text())
    assert manifest["completed_stage"] == "finished", manifest
    records = [json.loads(line)
               for line in mid_transcript.read_text().splitlines()]
    assert [record["role"] for record in records] == [
        "system", "user", "assistant", "user", "assistant"], records
    assert [record.get("turn", 0) for record in records] == [0, 0, 1, 1, 2]
    assert "Saved hello-midturn.txt" in records[3]["content"]
    # The resumed run asked the model only for the turn after the re-approved
    # write; the pending write itself made no request.
    assert len(coder_bodies) == 5, len(coder_bodies)
    finish_messages = coder_bodies[4]["messages"]
    assert [message["role"] for message in finish_messages] == [
        "user", "assistant", "user"], finish_messages
    assert "hello-midturn" in finish_messages[2]["content"]

    # A crash after the finish reply was saved but before the manifest was
    # updated is the other resume boundary: the action is completed without
    # loading a model.
    pending_finish = sessions_root / "29991231-235959-997"
    shutil.copytree(mid_session, pending_finish)
    manifest = json.loads((pending_finish / "manifest.json").read_text())
    manifest["completed_stage"] = "coder-turn-2"
    (pending_finish / "manifest.json").write_text(json.dumps(manifest))
    coder_calls_before_finish = counts["coder"]
    process, master, output = start(["--resume", pending_finish.name],
                                    cwd=mid_workspace)
    try:
        wait_for(output, master, "Finished: Created hello-midturn.txt.")
        assert counts["coder"] == coder_calls_before_finish
        os.write(master, b"\n")
        wait_for(output, master, "Describe what you want to build")
        os.write(master, b"/quit\n")
        assert process.wait(timeout=10) == 0
    finally:
        stop(process, master)
    manifest = json.loads((pending_finish / "manifest.json").read_text())
    assert manifest["completed_stage"] == "finished", manifest

    # Two unusable Coder replies for the same turn: the first is not JSON, the
    # second is an unsupported action. Both are archived, the first gets a
    # persisted correction, and the second aborts. The transcript never holds
    # either reply, and resume continues from the correction.
    bad_workspace = root / "resume-bad"
    bad_workspace.mkdir()
    state["phase"] = "bad"
    state["coder_request"] = 0
    first_bad_body = len(coder_bodies)
    process, master, output = start([], cwd=bad_workspace)
    try:
        wait_for(output, master, "Describe what you want to build")
        os.write(master, b"create hello-bad.txt\n")
        wait_for(output, master, "Pipeline stopped:")
        wait_for(output, master, "not valid actions")
        wait_for(output, master, "Completed stage outputs remain saved")
        os.write(master, b"\n")
        wait_for(output, master, "Describe what you want to build")
        os.write(master, b"/quit\n")
        assert process.wait(timeout=10) == 0
    finally:
        stop(process, master)

    bad_sessions = [entry for entry in sessions_root.iterdir()
                    if (entry / "request.md").exists()
                    and "hello-bad.txt" in (entry / "request.md").read_text()]
    assert len(bad_sessions) == 1, bad_sessions
    bad_session = bad_sessions[0]
    bad_transcript = bad_session / "coder" / "messages.jsonl"
    records = [json.loads(line)
               for line in bad_transcript.read_text().splitlines()]
    assert [record["role"] for record in records] == [
        "system", "user", "user"], records
    assert "Your previous reply was rejected" in records[2]["content"]
    assert records[2]["turn"] == 1
    rejected = list((bad_session / "coder" / "rejected").iterdir())
    assert len(rejected) == 2, rejected
    rejected_names = [entry.name for entry in rejected]
    assert any(name.startswith("turn-1-malformed-") for name in rejected_names)
    assert any(name.startswith("turn-1-unsupported-")
               for name in rejected_names)
    malformed = next(entry for entry in rejected
                     if entry.name.startswith("turn-1-malformed-"))
    unsupported = next(entry for entry in rejected
                       if entry.name.startswith("turn-1-unsupported-"))
    assert "Sorry, I cannot answer that." in malformed.read_text()
    assert "Coder thinking process" in malformed.read_text()
    assert "delete_everything" in unsupported.read_text()
    # The live retry request carried the correction, so the model was told why
    # its first reply could not be used.
    retry_messages = coder_bodies[first_bad_body + 1]["messages"]
    assert [message["role"] for message in retry_messages] == [
        "user", "user"], retry_messages
    assert "Your previous reply was rejected" in retry_messages[1]["content"]
    assert "must be a JSON action" in retry_messages[1]["content"]
    # Neither reply entered the conversation, and both raw streams were set
    # aside so the retry could not read the old text back.
    transcript_text = bad_transcript.read_text()
    assert "Sorry, I cannot answer that." not in transcript_text
    assert "delete_everything" not in transcript_text
    raw_asides = list((bad_session / "coder").glob("raw.jsonl.before-resume-*"))
    assert len(raw_asides) == 2, raw_asides
    assert not (bad_session / "coder" / "raw.jsonl").exists()
    manifest = json.loads((bad_session / "manifest.json").read_text())
    assert manifest["completed_stage"] == "prompter", manifest

    first_resume_body = len(coder_bodies)
    state["phase"] = "bad_resume"
    state["coder_request"] = 0
    process, master, output = start(["--resume", bad_session.name],
                                    cwd=bad_workspace)
    try:
        wait_for(output, master, "Resuming session")
        wait_for(output, master, "Approve this exact file content?")
        os.write(master, b"y")
        wait_for(output, master, "Finished: Created hello-bad.txt.")
        os.write(master, b"\n")
        wait_for(output, master, "Describe what you want to build")
        os.write(master, b"/quit\n")
        assert process.wait(timeout=10) == 0
    finally:
        stop(process, master)

    assert (bad_workspace / "hello-bad.txt").read_text() == "bad-ok\n"
    manifest = json.loads((bad_session / "manifest.json").read_text())
    assert manifest["completed_stage"] == "finished", manifest
    records = [json.loads(line)
               for line in bad_transcript.read_text().splitlines()]
    assert [record["role"] for record in records] == [
        "system", "user", "user", "assistant", "user", "assistant"], records
    assert [record.get("turn", 0) for record in records] == [
        0, 0, 1, 1, 1, 2], records
    resume_messages = coder_bodies[first_resume_body]["messages"]
    assert [message["role"] for message in resume_messages] == [
        "user", "user"], resume_messages
    assert "Your previous reply was rejected" in resume_messages[1]["content"]
    assert "delete_everything" not in json.dumps(resume_messages)

    # A transcript from before parse-before-persist can end with a saved reply
    # that was never usable. Resume must discard it, save a correction, and
    # allow only one fresh attempt for that turn.
    saved_root = sessions_root / "29991231-235959-996"
    shutil.copytree(mid_session, saved_root)
    manifest = json.loads((saved_root / "manifest.json").read_text())
    manifest["completed_stage"] = "coder-turn-1"
    (saved_root / "manifest.json").write_text(json.dumps(manifest))
    saved_transcript = saved_root / "coder" / "messages.jsonl"
    records = [json.loads(line)
               for line in saved_transcript.read_text().splitlines()]
    # Keep the write and its tool result, drop the original finish, and append
    # an unsupported saved reply for turn 2.
    records = records[:4]
    records.append({"role": "assistant",
                    "content": json.dumps({"action": "explode"}),
                    "turn": 2})
    saved_transcript.write_text(
        "".join(json.dumps(record) + "\n" for record in records))
    state["phase"] = "bad_saved_retry"
    state["coder_request"] = 0
    process, master, output = start(["--resume", saved_root.name],
                                    cwd=mid_workspace)
    try:
        wait_for(output, master, "discarded it")
        wait_for(output, master, "Pipeline stopped:")
        wait_for(output, master, "not valid actions")
        wait_for(output, master, "Completed stage outputs remain saved")
        os.write(master, b"\n")
        wait_for(output, master, "Describe what you want to build")
        os.write(master, b"/quit\n")
        assert process.wait(timeout=10) == 0
    finally:
        stop(process, master)

    recovered = list((saved_root / "coder" / "rejected").iterdir())
    assert len(recovered) == 2, recovered
    recovered_names = [entry.name for entry in recovered]
    assert any(name.startswith("turn-2-unsupported-")
               for name in recovered_names)
    assert any(name.startswith("turn-2-malformed-")
               for name in recovered_names)
    recovered_unsupported = next(
        entry for entry in recovered
        if entry.name.startswith("turn-2-unsupported-"))
    recovered_malformed = next(
        entry for entry in recovered
        if entry.name.startswith("turn-2-malformed-"))
    assert "explode" in recovered_unsupported.read_text()
    assert "still not a JSON action" in recovered_malformed.read_text()
    records = [json.loads(line)
               for line in saved_transcript.read_text().splitlines()]
    assert [record["role"] for record in records] == [
        "system", "user", "assistant", "user", "user"], records
    assert records[-1]["turn"] == 2
    assert "Your previous reply was rejected" in records[-1]["content"]
    assert not any(record["role"] == "assistant" and
                   "explode" in record["content"] for record in records)
    assert list((saved_root / "coder").glob("raw.jsonl.before-resume-*"))
    assert not (saved_root / "coder" / "raw.jsonl").exists()

    # The persisted correction survives the abort. This resume gets a valid
    # write and finish, and the model sees the correction in its context.
    first_valid_body = len(coder_bodies)
    state["phase"] = "bad_saved_valid"
    state["coder_request"] = 0
    process, master, output = start(["--resume", saved_root.name],
                                    cwd=mid_workspace)
    try:
        wait_for(output, master, "Resuming session")
        wait_for(output, master, "Approve this exact file content?")
        os.write(master, b"y")
        wait_for(output, master, "Finished: Created hello-saved.txt.")
        os.write(master, b"\n")
        wait_for(output, master, "Describe what you want to build")
        os.write(master, b"/quit\n")
        assert process.wait(timeout=10) == 0
    finally:
        stop(process, master)

    assert (mid_workspace / "hello-saved.txt").read_text() == "saved-ok\n"
    manifest = json.loads((saved_root / "manifest.json").read_text())
    assert manifest["completed_stage"] == "finished", manifest
    records = [json.loads(line)
               for line in saved_transcript.read_text().splitlines()]
    assert [record["role"] for record in records] == [
        "system", "user", "assistant", "user", "user", "assistant", "user",
        "assistant"], records
    assert [record.get("turn", 0) for record in records] == [
        0, 0, 1, 1, 2, 2, 2, 3], records
    retry_messages = coder_bodies[first_valid_body]["messages"]
    assert [message["role"] for message in retry_messages] == [
        "user", "assistant", "user", "user"], retry_messages
    assert "Your previous reply was rejected" in \
        retry_messages[-1]["content"]

    assert counts == {"chief": 3, "prompter": 3, "coder": 12}, counts

    server.shutdown()


def check_signal_handling(binary, root):
    user_home = root / "signal-home"
    user_home.mkdir()
    config_home = root / "signal-config"
    config_path = config_home / "mtrxai" / "config.json"
    config_path.parent.mkdir(parents=True)

    ollama_unloaded = threading.Event()

    class SignalHandler(BaseHTTPRequestHandler):
        def do_POST(self):
            if self.path == "/api/generate":
                body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                if body.get("keep_alive") == 0:
                    ollama_unloaded.set()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                self.wfile.write(b"{}")
                return
            # Hang briefly on /api/chat so SIGINT is received while Ollama is active
            time.sleep(2)
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(b'{"message":{"content":"ok"}}')

        def log_message(self, *_args):
            pass

    server = ThreadingHTTPServer(("127.0.0.1", 0), SignalHandler)
    threading.Thread(target=server.serve_forever, daemon=True).start()

    config = {
        "chief": {"provider": "ollama", "model": "chief-sig-model", "base_url": f"http://127.0.0.1:{server.server_port}"},
        "prompter": {"provider": "ollama", "model": "prompter-sig-model", "base_url": f"http://127.0.0.1:{server.server_port}"},
        "coder": {"provider": "ollama", "model": "coder-sig-model", "base_url": f"http://127.0.0.1:{server.server_port}"},
    }
    config_path.write_text(json.dumps(config))

    env = os.environ.copy()
    env.update({"HOME": str(user_home), "XDG_CONFIG_HOME": str(config_home), "TERM": "xterm"})
    master, slave = pty.openpty()
    process = subprocess.Popen(
        [binary, "--no-update-check"], cwd=root, env=env,
        stdin=slave, stdout=slave, stderr=slave
    )
    os.close(slave)
    output = bytearray()
    try:
        deadline = time.time() + 8
        while b"Describe what you want to build" not in output and time.time() < deadline:
            r, _, _ = select.select([master], [], [], 0.1)
            if r:
                try:
                    output.extend(os.read(master, 1024))
                except OSError:
                    break
        assert b"Describe what you want to build" in output

        os.write(master, b"Test task\n")
        # Give it a moment to initiate the Ollama call
        time.sleep(0.3)

        os.kill(process.pid, 2)  # SIGINT
        exit_code = process.wait(timeout=5)
        assert exit_code != 0, f"Expected non-zero exit code on SIGINT, got {exit_code}"
        assert ollama_unloaded.wait(timeout=4), "Ollama keep_alive: 0 unload request was not received on SIGINT"
    finally:
        server.shutdown()
        if process.poll() is None:
            process.kill()
            process.wait()
        os.close(master)


def run(binary):
    temp = Path(tempfile.mkdtemp(prefix="mtrxai-e2e-"))
    check_setup_wizard(binary, temp)
    check_signal_handling(binary, temp)
    check_esc_cancellation(binary, temp)
    check_resume(binary, temp)
    workspace = temp / "workspace"
    outside = temp / "outside"
    config_home = temp / "config"
    state_home = temp / "state"
    workspace.mkdir()
    outside.mkdir()
    (workspace / "escape").symlink_to(outside, target_is_directory=True)
    (workspace / "existing.txt").write_text("original file\n")
    (workspace / ".env.local").write_text("local-secret-value\n")
    (workspace / "large.txt").write_text("A" * 4000 + "B" * 4000 + "C" * 500)
    gguf_path = temp / "mock.gguf"
    gguf_path.write_bytes(b"test model placeholder")

    actions = [
        "<think>Chief thinking process</think>Chief brief: create and verify a generated file.",
        "Prompter brief: inspect, edit, test, and summarize.",
        {"action": "list_files", "path": "."},
        {"action": "read_file", "path": "existing.txt"},
        {"action": "read_file", "path": ".env.local"},
        {"action": "read_file", "path": "large.txt", "offset": 0},
        {"action": "read_file", "path": "large.txt", "offset": 4000},
        {"action": "write_file", "path": "../outside.txt", "content": "blocked\n"},
        {"action": "write_file", "path": "escape/outside.txt", "content": "blocked\n"},
        {"action": "write_file", "path": "existing.txt", "content": "updated file\n"},
        {"action": "write_file", "path": "generated.txt", "content": "generated-ok\n"},
        {"action": "run_command", "command": "sleep 5", "timeout_seconds": 1},
        {"action": "run_command", "command": "exit 3", "timeout_seconds": 10},
        {"action": "run_command", "command": "cat /proc/$PPID/environ", "timeout_seconds": 10},
        {
            "action": "run_command",
            "command": "printf 'test-ok:%s' \"$MTRXAI_TEST_TOKEN\"",
            "timeout_seconds": 10,
        },
        {"action": "write_file", "path": "declined.txt", "content": "must-not-exist\n"},
        {"action": "run_command", "command": "touch declined-command.txt", "timeout_seconds": 10},
        {"action": "finish", "summary": "Created generated.txt and verified it with printf."},
    ]
    requests = []
    generated_requests = []
    chat_requests = []

    class Handler(BaseHTTPRequestHandler):
        def do_POST(self):
            body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            if self.path == "/coder":
                generated_requests.append({"path": self.path, "body": body})
                answer = actions[len(generated_requests) + 1]
                content = answer if isinstance(answer, str) else json.dumps(answer)
                response = json.dumps(
                    {"choices": [{"message": {"content": content}}]}
                ).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(response)))
                self.end_headers()
                self.wfile.write(response)
                return

            requests.append(
                {
                    "path": self.path,
                    "body": body,
                    "headers": dict(self.headers.items()),
                }
            )
            if self.path == "/api/generate":
                response = json.dumps({"response": "", "done": True}).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(response)))
                self.end_headers()
                self.wfile.write(response)
                return
            answer = actions[len(chat_requests)]
            chat_requests.append(body)
            if self.path == "/api/generate" or not body.get("stream"):
                raise AssertionError(f"expected a streaming request to {self.path}")
            if self.path == "/v1/messages":
                # Anthropic streaming: emit the answer as text_delta events.
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.end_headers()
                for index in range(0, len(answer), 7):
                    chunk = answer[index:index + 7]
                    event = json.dumps(
                        {"type": "content_block_delta",
                         "delta": {"type": "text_delta", "text": chunk}})
                    self.wfile.write(f"data: {event}\n\n".encode())
                    self.wfile.flush()
                self.wfile.write(b"data: {\"type\":\"message_stop\"}\n\n")
                self.wfile.flush()
                return
            # Ollama native chat streaming: one JSON object per line.
            self.send_response(200)
            self.send_header("Content-Type", "application/x-ndjson")
            self.end_headers()
            for index in range(0, len(answer), 9):
                chunk = answer[index:index + 9]
                event = json.dumps(
                    {"model": "prompter-test",
                     "message": {"role": "assistant", "content": chunk},
                     "done": False})
                self.wfile.write(event.encode() + b"\n")
                self.wfile.flush()
            event = json.dumps({"model": "prompter-test", "done": True})
            self.wfile.write(event.encode() + b"\n")
            self.wfile.flush()

        def log_message(self, *_args):
            pass

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    config = {}
    config["chief"] = {
        "provider": "anthropic",
        "model": "chief-test",
        "base_url": f"http://127.0.0.1:{server.server_port}",
        "api_key_env": "",
        "gguf_path": "",
        "context_size": 2048,
    }
    config["prompter"] = {
        "provider": "ollama",
        "model": "prompter-test",
        "base_url": f"http://127.0.0.1:{server.server_port}",
        "api_key_env": "",
        "gguf_path": "",
        "context_size": 2048,
    }
    config["coder"] = {
        "provider": "gguf",
        "model": "",
        "base_url": "http://127.0.0.1",
        "api_key_env": "",
        "gguf_path": str(gguf_path),
        "context_size": 2048,
    }
    llama_bin = temp / "bin"
    llama_bin.mkdir()
    llama_executable = llama_bin / "llama-server"
    shutil.copyfile(Path(__file__).with_name("mock_llama_server.py"), llama_executable)
    llama_executable.chmod(llama_executable.stat().st_mode | stat.S_IXUSR)
    config_path = config_home / "mtrxai" / "config.json"
    config_path.parent.mkdir(parents=True)
    config_path.write_text(json.dumps(config))
    credentials_path = config_path.parent / "credentials.json"
    credentials_path.write_text(json.dumps({"chief": "test-anthropic-key"}))
    credentials_path.chmod(0o600)
    env = os.environ.copy()
    env.update(
        {
            "XDG_CONFIG_HOME": str(config_home),
            "XDG_STATE_HOME": str(state_home),
            "TERM": "xterm",
            "MTRXAI_TEST_TOKEN": "must-not-leak",
            "MTRXAI_TEST_UPSTREAM": f"http://127.0.0.1:{server.server_port}",
            "MTRXAI_TEST_LLAMA_STARTS": str(temp / "llama-starts.jsonl"),
            "PATH": str(llama_bin) + os.pathsep + os.environ.get("PATH", ""),
        }
    )
    master, slave = pty.openpty()
    process = subprocess.Popen(
        [binary, "--no-update-check"], cwd=workspace, env=env,
        stdin=slave, stdout=slave, stderr=slave
    )
    os.close(slave)
    output = bytearray()

    def wait_for(needle, timeout=20):
        target = needle.encode()
        deadline = time.time() + timeout
        while target not in output and time.time() < deadline:
            readable, _, _ = select.select([master], [], [], 0.5)
            if readable:
                try:
                    output.extend(os.read(master, 8192))
                except OSError:
                    break
        if target not in output:
            raise AssertionError(
                f"Timed out waiting for {needle!r}: "
                f"{output.decode(errors='replace')[-1500:]}"
            )

    try:
        wait_for("Describe what you want to build")
        os.write(master, b"Create and verify a generated file\n")
        output.clear()
        wait_for("Approve this exact file content?")
        assert b"existing.txt" in output
        os.write(master, b"y")
        output.clear()
        wait_for("Approve this exact file content?")
        assert b"generated-ok" in output
        os.write(master, b"y")
        output.clear()
        wait_for("Approve this exact command?")
        assert b"sleep 5" in output
        os.write(master, b"y")
        output.clear()
        wait_for("Approve this exact command?")
        assert b"exit 3" in output
        os.write(master, b"y")
        output.clear()
        wait_for("Approve this exact command?")
        assert b"/proc/$PPID/environ" in output
        os.write(master, b"y")
        output.clear()
        wait_for("Approve this exact command?")
        assert b"test-ok" in output
        os.write(master, b"y")
        output.clear()
        wait_for("Approve this exact file content?")
        assert b"declined.txt" in output
        os.write(master, b"n")
        output.clear()
        wait_for("Approve this exact command?")
        assert b"touch declined-command.txt" in output
        os.write(master, b"n")
        output.clear()
        wait_for("Created generated.txt and verified it with printf.")
        os.write(master, b"\n")
        output.clear()
        wait_for("Describe what you want to build")
        os.write(master, b"/quit\n")
        assert process.wait(timeout=10) == 0
        assert (workspace / "generated.txt").read_text() == "generated-ok\n"
        assert (workspace / "existing.txt").read_text() == "updated file\n"
        assert not (workspace / "declined.txt").exists()
        assert not (workspace / "declined-command.txt").exists()
        assert not (outside / "outside.txt").exists()
        assert len(generated_requests) == 16, len(generated_requests)
        assert [entry["path"] for entry in requests[:3]] == [
            "/v1/messages",
            "/api/chat",
            "/api/generate",
        ]
        headers = {key.lower(): value for key, value in requests[0]["headers"].items()}
        assert headers["x-api-key"] == "test-anthropic-key"
        assert headers["anthropic-version"] == "2023-06-01"
        assert requests[0]["body"]["model"] == "chief-test"
        assert requests[0]["body"]["system"]
        assert requests[0]["body"]["max_tokens"] == 4096
        assert requests[1]["body"]["keep_alive"] == "30s"
        assert requests[1]["body"]["options"]["num_predict"] == 4096
        assert requests[2]["body"]["keep_alive"] == 0
        # Every generation request asks for streaming; the unload does not.
        assert requests[0]["body"]["stream"] is True
        assert requests[1]["body"]["stream"] is True
        assert all(entry["body"]["stream"] is True for entry in generated_requests)
        assert "Chief thinking process" not in json.dumps(requests[1]["body"]["messages"])
        assert "<think>" not in json.dumps(requests[1]["body"]["messages"])
        assert generated_requests[0]["body"]["max_tokens"] == 4096
        assert "existing.txt" in json.dumps(generated_requests[1]["body"]["messages"])
        assert "original file" in json.dumps(generated_requests[2]["body"]["messages"])
        assert "sensitive file" in json.dumps(generated_requests[3]["body"]["messages"])
        assert "bytes 0-4000" in json.dumps(generated_requests[4]["body"]["messages"])
        assert "A" * 100 in json.dumps(generated_requests[4]["body"]["messages"])
        assert "next_offset=8000" in json.dumps(generated_requests[5]["body"]["messages"])
        assert "traverses outside" in json.dumps(generated_requests[6]["body"]["messages"])
        assert "outside the project" in json.dumps(generated_requests[7]["body"]["messages"])
        assert "command timed out" in json.dumps(generated_requests[10]["body"]["messages"])
        assert "exit_code=3" in json.dumps(generated_requests[11]["body"]["messages"])
        assert "Permission denied" in json.dumps(generated_requests[12]["body"]["messages"])
        assert "test-ok" in json.dumps(generated_requests[13]["body"]["messages"])
        assert "must-not-leak" not in json.dumps(requests + generated_requests)
        assert "local-secret-value" not in json.dumps(requests + generated_requests)
        assert "declined" in json.dumps(generated_requests[15]["body"]["messages"])
        llama_starts = (temp / "llama-starts.jsonl").read_text().splitlines()
        assert len(llama_starts) == 1, llama_starts
        llama_start = json.loads(llama_starts[0])
        llama_args = llama_start["args"]
        assert "--n-gpu-layers" in llama_args
        assert llama_args[llama_args.index("--n-gpu-layers") + 1] == "0"
        assert llama_args[llama_args.index("--ctx-size") + 1] == "2048"
        try:
            os.kill(llama_start["pid"], 0)
        except ProcessLookupError:
            pass
        else:
            raise AssertionError("llama-server remained running after the Coder stage")
        coder_log_file = state_home / "mtrxai" / "logs" / "coder.log"
        assert coder_log_file.exists(), f"Missing llama log at {coder_log_file}"
        assert stat.S_IMODE(coder_log_file.stat().st_mode) == 0o600
        assert "mock llama-server started" in coder_log_file.read_text()
        session_dirs = list((state_home / "mtrxai" / "sessions").iterdir())
        assert len(session_dirs) == 1
        session = session_dirs[0]
        assert (session / "request.md").exists()
        assert (session / "chief.md").exists()
        assert (session / "chief_thinking.md").exists()
        assert (session / "chief_thinking.md").read_text().strip() == "Chief thinking process"
        # Raw streams: tokens persisted verbatim before the derived artifacts.
        for stage in ("chief", "prompt", "coder"):
            raw_file = session / stage / "raw.jsonl"
            assert raw_file.exists(), f"Missing raw stream {raw_file}"
            assert stat.S_IMODE(raw_file.stat().st_mode) == 0o600
        chief_raw_lines = [
            json.loads(line)
            for line in
            (session / "chief" / "raw.jsonl").read_text().splitlines()
        ]
        assert all(entry["type"] == "token" for entry in chief_raw_lines)
        chief_raw_text = "".join(entry["text"] for entry in chief_raw_lines)
        assert "Chief thinking process" in chief_raw_text
        assert chief_raw_text.endswith(
            "Chief brief: create and verify a generated file.")
        # Last Coder turn raw content matches the finish action in coder.md.
        coder_raw_lines = [
            json.loads(line)
            for line in
            (session / "coder" / "raw.jsonl").read_text().splitlines()
        ]
        assert coder_raw_lines[0]["type"] == "start"
        assert coder_raw_lines[0]["turn"] == 1
        final_turn = coder_raw_lines[-1]["turn"]
        final_text = "".join(
            entry["text"] for entry in coder_raw_lines
            if entry["type"] == "token" and entry["turn"] == final_turn)
        assert json.loads(final_text)["action"] == "finish"
        assert "<think>" not in (session / "chief.md").read_text()
        assert (session / "prompt.md").exists()
        coder_log = (session / "coder.md").read_text()
        assert "Created generated.txt and verified it with printf." in coder_log, coder_log
        backups = list((session / "backups").glob("*.bak"))
        assert len(backups) == 1, backups
        assert backups[0].read_text() == "original file\n"
        assert json.loads((session / "manifest.json").read_text())[
            "completed_stage"
        ] == "finished"
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        server.shutdown()
        os.close(master)
        shutil.rmtree(temp)


if __name__ == "__main__":
    run(sys.argv[1])
    print("Sequential stages, iterative coder tools, approvals, persistence, "
          "Esc cancellation, and resume passed.")
