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

upstream = os.environ["MTRXAI_TEST_UPSTREAM"]


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path != "/health":
            self.send_error(404)
            return
        self.send_response(200)
        self.end_headers()

    def do_POST(self):
        request = Request(
            upstream + "/coder",
            data=self.rfile.read(int(self.headers["Content-Length"])),
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        with urlopen(request, timeout=60) as response:
            payload = response.read()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, *_args):
        pass


server = ThreadingHTTPServer((argument("--host"), int(argument("--port"))), Handler)
server.serve_forever()
