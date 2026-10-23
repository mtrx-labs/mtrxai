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


def check_portable_default_config(binary, root):
    config_home = root / "portable-config"
    user_home = root / "different-user-home"
    user_home.mkdir()
    env = os.environ.copy()
    env.update({"HOME": str(user_home), "XDG_CONFIG_HOME": str(config_home)})
    result = subprocess.run(
        [binary, "--init"], env=env, text=True, capture_output=True, check=True
    )
    assert "coder.gguf_path" in result.stdout
    config_path = config_home / "mtrxai" / "config.json"
    config = json.loads(config_path.read_text())
    assert config["coder"]["provider"] == "gguf"
    assert config["coder"]["gguf_path"] == ""
    assert str(user_home) not in config_path.read_text()

    master, slave = pty.openpty()
    process = subprocess.Popen(
        [binary], cwd=root, env={**env, "TERM": "xterm"},
        stdin=slave, stdout=slave, stderr=slave,
    )
    os.close(slave)
    output = bytearray()
    try:
        deadline = time.time() + 10
        while b"Coder GGUF path is unset" not in output and time.time() < deadline:
            readable, _, _ = select.select([master], [], [], 0.2)
            if readable:
                output.extend(os.read(master, 8192))
        assert b"Coder GGUF path is unset" in output, output.decode(errors="replace")
        os.write(master, b"/quit\n")
        assert process.wait(timeout=5) == 0
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=3)
        os.close(master)


def run(binary):
    temp = Path(tempfile.mkdtemp(prefix="mtrxai-e2e-"))
    check_portable_default_config(binary, temp)
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
        "Chief brief: create and verify a generated file.",
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

            requests.append({"path": self.path, "body": body})
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
            response = json.dumps({"message": {"content": answer}}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(response)))
            self.end_headers()
            self.wfile.write(response)

        def log_message(self, *_args):
            pass

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    config = {}
    for role in ("chief", "prompter"):
        config[role] = {
            "provider": "ollama",
            "model": f"{role}-test",
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
        [binary], cwd=workspace, env=env, stdin=slave, stdout=slave, stderr=slave
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
        wait_for("Enter a task")
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
        wait_for("Enter a task")
        os.write(master, b"/quit\n")
        assert process.wait(timeout=10) == 0
        assert (workspace / "generated.txt").read_text() == "generated-ok\n"
        assert (workspace / "existing.txt").read_text() == "updated file\n"
        assert not (workspace / "declined.txt").exists()
        assert not (workspace / "declined-command.txt").exists()
        assert not (outside / "outside.txt").exists()
        assert len(generated_requests) == 16, len(generated_requests)
        assert [entry["path"] for entry in requests[:4]] == [
            "/api/chat",
            "/api/generate",
            "/api/chat",
            "/api/generate",
        ]
        assert requests[0]["body"]["keep_alive"] == "30s"
        assert requests[1]["body"]["keep_alive"] == 0
        assert requests[2]["body"]["keep_alive"] == "30s"
        assert requests[3]["body"]["keep_alive"] == 0
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
        session_dirs = list((state_home / "mtrxai" / "sessions").iterdir())
        assert len(session_dirs) == 1
        session = session_dirs[0]
        assert (session / "request.md").exists()
        assert (session / "chief.md").exists()
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
    print("Sequential stages, iterative coder tools, approvals, and persistence passed.")
