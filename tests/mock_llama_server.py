#!/usr/bin/env python3
import json
import os
import sys
from pathlib import Path
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.request import Request, urlopen


def argument(name):
    return sys.argv[sys.argv.index(name) + 1]


with open(os.environ["MTRXAI_TEST_LLAMA_STARTS"], "a", encoding="utf-8") as log:
    log.write(json.dumps({"args": sys.argv[1:], "pid": os.getpid()}) + "\n")

sys.stderr.write("mock llama-server started\n")
sys.stderr.flush()

upstream = os.environ["MTRXAI_TEST_UPSTREAM"]
stream_dir = Path(os.environ["MTRXAI_TEST_LLAMA_STREAM_DIR"])
stream_dir.mkdir(parents=True, exist_ok=True)
served_calls = 0


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path != "/health":
            self.send_error(404)
            return
        self.send_response(200)
        self.end_headers()

    def do_POST(self):
        global served_calls
        raw_body = self.rfile.read(int(self.headers["Content-Length"]))
        body = json.loads(raw_body)
        request = Request(
            upstream + "/coder",
            data=raw_body,
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        with urlopen(request, timeout=60) as response:
            payload = json.loads(response.read())
        if not body.get("stream"):
            raise AssertionError("expected a streaming /chat/completions request")
        # Answer as an OpenAI-style SSE stream. A leading think block travels
        # as reasoning_content, exactly like the real providers, so the engine's
        # content/reasoning split is exercised end to end.
        text = payload["choices"][0]["message"]["content"]
        reasoning = ""
        if text.startswith("<think>") and "</think>" in text:
            end = text.index("</think>")
            reasoning = text[len("<think>"):end]
            text = text[end + len("</think>"):].lstrip()
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.end_headers()
        raw_response = bytearray()
        for index in range(0, len(reasoning), 6):
            event = json.dumps(
                {"choices": [{"delta": {
                    "reasoning_content": reasoning[index:index + 6]}}]})
            wire = f"data: {event}\n\n".encode()
            raw_response.extend(wire)
            self.wfile.write(wire)
            self.wfile.flush()
        for index in range(0, len(text), 6):
            chunk = text[index:index + 6]
            event = json.dumps(
                {"choices": [{"delta": {"content": chunk}}]})
            wire = f"data: {event}\n\n".encode()
            raw_response.extend(wire)
            self.wfile.write(wire)
            self.wfile.flush()
        raw_response.extend(b"data: [DONE]\n\n")
        self.wfile.write(b"data: [DONE]\n\n")
        self.wfile.flush()
        (stream_dir / f"call-{served_calls + 1}.raw").write_bytes(raw_response)
        served_calls += 1

    def log_message(self, *_args):
        pass


server = ThreadingHTTPServer((argument("--host"), int(argument("--port"))), Handler)
server.serve_forever()
