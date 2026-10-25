#!/usr/bin/env python3
import json
import os
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.request import Request, urlopen


def argument(name):
    return sys.argv[sys.argv.index(name) + 1]


with open(os.environ["MTRXAI_TEST_LLAMA_STARTS"], "a", encoding="utf-8") as log:
    log.write(json.dumps({"args": sys.argv[1:], "pid": os.getpid()}) + "\n")

sys.stderr.write("mock llama-server started\n")
sys.stderr.flush()

upstream = os.environ["MTRXAI_TEST_UPSTREAM"]


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path != "/health":
            self.send_error(404)
            return
        self.send_response(200)
        self.end_headers()

    def do_POST(self):
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
        # Answer as an OpenAI-style SSE stream.
        content = payload["choices"][0]["message"]["content"]
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.end_headers()
        for index in range(0, len(content), 6):
            chunk = content[index:index + 6]
            event = json.dumps(
                {"choices": [{"delta": {"content": chunk}}]})
            self.wfile.write(f"data: {event}\n\n".encode())
            self.wfile.flush()
        self.wfile.write(b"data: [DONE]\n\n")
        self.wfile.flush()

    def log_message(self, *_args):
        pass


server = ThreadingHTTPServer((argument("--host"), int(argument("--port"))), Handler)
server.serve_forever()
