"""The mtrxAI web server: stdlib asyncio HTTP + SSE, no new dependencies.

Serves the GUI and exposes the same core operations the REPL exposes:

    GET  /                    GUI (index.html)
    GET  /theme.css           generated palette tokens (both themes)
    GET  /static/app.css|js   front-end assets
    GET  /api/state           agents, settings, version (one-shot snapshot)
    GET  /api/events          SSE stream: events + cards
    POST /api/task            {text}  → orchestrator.dispatch (worker task)
    POST /api/card/<id>       resolve a pending approval card
    POST /api/swap            {agent, ref} → registry.swap (hot-swap, persists)
    POST /api/reload          registry.load()
    POST /api/create          {name?} → orchestrator.create_flow
    POST /api/stop            cooperative cancel of the running dispatch
    POST /api/shutdown        exit the app (ctrl+q equivalent)

Design decisions:
  - Bind 127.0.0.1 by default; the tool loop and approvals are powerful, this
    service must not be reachable from the LAN unless asked for (--host).
  - Dispatches are serialized: the REPL is line-serial too, and concurrent runs
    would interleave approval cards confusingly. A task while busy → 409.
  - Content-Length is capped (MAX_BODY); requests are small JSON.
"""

from __future__ import annotations

import asyncio
import json
import logging
from pathlib import Path

from .. import __version__
from .theme import theme_css
from .ui import WebUI

log = logging.getLogger("mtrxai.gui")

MAX_BODY = 64 * 1024
STATIC_DIR = Path(__file__).parent / "static"
STATIC_FILES = {
    "/": ("index.html", "text/html; charset=utf-8"),
    "/static/app.css": ("app.css", "text/css; charset=utf-8"),
    "/static/app.js": ("app.js", "text/javascript; charset=utf-8"),
}


class ServerState:
    """Everything the request handlers need, shared with the WebUI."""

    def __init__(self, runtime, ui: WebUI) -> None:
        self.rt = runtime
        self.ui = ui
        self.current_task: asyncio.Task | None = None

    def busy(self) -> bool:
        return self.current_task is not None and not self.current_task.done()

    def start_dispatch(self, text: str) -> bool:
        """Run orchestrator.dispatch as a managed task; False if already busy."""
        if self.busy():
            return False
        self.current_task = asyncio.get_running_loop().create_task(self._dispatch(text))
        return True

    async def _dispatch(self, text: str) -> None:
        try:
            answer = await self.rt.orchestrator.dispatch(text, source="web")
        except asyncio.CancelledError:
            self.ui.event({"type": "status", "text": "task stopped by user"})
            raise
        except Exception as e:  # noqa: BLE001 — surface, never crash the server
            self.ui.event({"type": "error", "text": f"dispatch failed: {e}"})
            return
        finally:
            self.current_task = None
        # NOTE: orchestrator.dispatch already emits the 'answer' event — no re-emit here.

    def stop_dispatch(self) -> bool:
        if not self.busy():
            return False
        self.current_task.cancel()
        return True

    # ------------------------------------------------------------- snapshots

    def state(self) -> dict:
        orch, reg = self.rt.orchestrator, self.rt.registry
        reg.reload_if_changed()
        agents = []
        for la in reg.all():
            agents.append({
                "name": la.spec.name,
                "model": la.spec.model,
                "tools": la.spec.tools,
                "description": la.spec.description,
                "router": la.spec.name == orch.router_name,
            })
        v = self.rt.settings["voice"]
        return {
            "version": __version__,
            "project_root": str(self.rt.settings["project_root"]),
            "workspace": str(self.rt.settings["workspace"]),
            "default_permission": self.rt.settings.get("default_permission", "ask"),
            "agents": agents,
            "router": orch.router_name,
            "default_model": orch.default_model,
            "voice": {"enabled": bool(v.get("enabled")), "stt": v["stt"], "tts": v["tts"]},
            "busy": self.busy(),
            "warnings": list(reg.warnings),
        }

    def trace_text(self) -> str:
        return self.rt.trace.format_last()

    def slash(self, text: str) -> dict:
        """Slash commands share the registry/orchestrator with the REPL."""
        cmd, _, rest = text.partition(" ")
        cmd = cmd.lower()
        reg, orch = self.rt.registry, self.rt.orchestrator
        if cmd == "/agents":
            return {"ok": True, "agents": [
                {"name": la.spec.name, "model": la.spec.model, "tools": la.spec.tools,
                 "router": la.spec.name == orch.router_name}
                for la in reg.all()
            ]}
        if cmd == "/swap":
            parts = rest.split()
            if len(parts) != 2:
                return {"ok": False, "error": "usage: /swap <agent> <provider:model>"}
            try:
                la = reg.swap(parts[0], parts[1])
                return {"ok": True, "message": f"{la.spec.name} → {la.spec.model}"}
            except Exception as e:  # noqa: BLE001 — user-facing swap errors
                return {"ok": False, "error": f"swap failed: {e}"}
        if cmd == "/reload":
            reg.load()
            return {"ok": True, "message": f"{len(reg.names())} agents loaded",
                    "warnings": list(reg.warnings)}
        if cmd == "/trace":
            return {"ok": True, "trace": self.trace_text()}
        if cmd == "/help":
            return {"ok": True, "help": [
                "(just type)", "route + run a task",
                "/agents", "list agents with their current model refs",
                "/swap <agent> <provider:model>", "hot-swap one agent's model now",
                "/reload", "re-read all agent YAMLs from disk",
                "/create [name]", "manual agent-creation wizard",
                "/trace", "print the last run's routing/tool trace",
                "/quit", "exit (or POST /api/shutdown)",
            ]}
        return None  # not a server-side command (e.g. /create → card flow)


# --------------------------------------------------------------------- HTTP

class Response:
    def __init__(self, status: int = 200, body: bytes = b"", ctype: str = "application/json",
                 extra: dict[str, str] | None = None) -> None:
        self.status, self.body, self.ctype, self.extra = status, body, ctype, extra or {}


def json_response(data: dict, status: int = 200) -> Response:
    return Response(status, json.dumps(data).encode(), "application/json")


class PayloadTooLarge(Exception):
    pass


async def _recv_body(reader: asyncio.StreamReader, head: dict) -> bytes:
    """Read exactly Content-Length bytes (headers were already parsed)."""
    try:
        n = int(head.get("content-length", "0") or "0")
    except ValueError:
        n = 0
    if n > MAX_BODY:
        raise PayloadTooLarge()
    if n <= 0:
        return b""
    return await reader.readexactly(n)


class GuiServer:
    def __init__(self, state: ServerState) -> None:
        self.st = state
        self.ui = state.ui
        self._server: asyncio.Server | None = None
        self._sse_clients: set[asyncio.StreamWriter] = set()

    # ------------------------------------------------------------- lifecycle

    async def start(self, host: str = "127.0.0.1", port: int = 8417) -> None:
        self._server = await asyncio.start_server(self._handle, host, port)
        addr = ", ".join(str(s.getsockname()) for s in self._server.sockets)
        log.info("gui listening on http://%s", addr)

    async def stop(self) -> None:
        self.ui.fail_pending()
        if self._server is not None:
            self._server.close()
            await self._server.wait_closed()
        for w in list(self._sse_clients):
            w.close()

    # ----------------------------------------------------------- connection

    async def _handle(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        try:
            head = await self._read_head(reader)
            method = head["method"]
            path = head["path"]
            if path == "/api/events" and method == "GET":
                await self._sse(reader, writer, head)
                return
            body = await _recv_body(reader, head) if method in ("POST", "PUT") else b""
            resp = await self._route(method, path, head, body)
        except PayloadTooLarge:
            resp = json_response({"error": "payload too large"}, 413)
        except (asyncio.IncompleteReadError, ConnectionError):
            return  # client hung up — nothing to answer
        except Exception as e:  # noqa: BLE001 — last-ditch guard, log + 500
            log.exception("gui request failed")
            resp = json_response({"error": f"internal error: {e}"}, 500)
        try:
            await self._send(writer, resp, head.get("keepalive", True))
        except ConnectionError:
            pass
        finally:
            try:
                await writer.drain()
                writer.close()
            except Exception:  # noqa: BLE001
                pass

    async def _read_head(self, reader: asyncio.StreamReader) -> dict:
        head: dict[str, str] = {}
        request_line = (await reader.readline()).decode("latin-1").strip()
        parts = request_line.split(" ")
        head["method"] = parts[0].upper() if parts else ""
        head["path"] = parts[1] if len(parts) > 1 else "/"
        while True:
            line = (await reader.readline()).decode("latin-1").strip()
            if not line:
                break
            k, _, v = line.partition(":")
            head.setdefault(k.strip().lower(), v.strip())
        head["keepalive"] = head.get("connection", "").lower() != "close"
        return head

    async def _send(self, writer: asyncio.StreamWriter, resp: Response, keepalive: bool) -> None:
        head = (
            f"HTTP/1.1 {resp.status} {_STATUS.get(resp.status, 'OK')}\r\n"
            f"Content-Type: {resp.ctype}\r\n"
            f"Content-Length: {len(resp.body)}\r\n"
            "Cache-Control: no-store\r\n"
            f"Connection: {'keep-alive' if keepalive else 'close'}\r\n"
        )
        for k, v in resp.extra.items():
            head += f"{k}: {v}\r\n"
        writer.write(head.encode("latin-1") + b"\r\n" + resp.body)

    # ------------------------------------------------------------------ SSE

    async def _sse(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter,
                   head: dict) -> None:
        q = self.ui.subscribe()
        self._sse_clients.add(writer)
        try:
            writer.write(
                b"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                b"Cache-Control: no-store\r\nConnection: keep-alive\r\n\r\n"
            )
            # initial state so a fresh page paints instantly
            q.put_nowait({"kind": "state", "state": self.st.state()})
            while True:
                try:
                    payload = await asyncio.wait_for(q.get(), timeout=15.0)
                    data = json.dumps(payload, ensure_ascii=False)
                    writer.write(f"data: {data}\n\n".encode("utf-8"))
                except asyncio.TimeoutError:
                    writer.write(b": ping\n\n")  # keep intermediaries away
                try:
                    await writer.drain()
                except (ConnectionError, RuntimeError):
                    break
                if head.get("connection", "").lower() == "close":
                    break
        finally:
            self.ui.unsubscribe(q)
            self._sse_clients.discard(writer)
            if not self.ui.has_subscribers() and self.ui.pending_count():
                self.ui.fail_pending()

    # ---------------------------------------------------------------- routes

    async def _route(self, method: str, path: str, head: dict, body: bytes) -> Response:
        if method == "GET":
            if path in STATIC_FILES:
                return self._static(path)
            if path == "/theme.css":
                return Response(200, theme_css().encode(), "text/css; charset=utf-8")
            if path == "/api/state":
                return json_response(self.st.state())
            if path == "/api/trace":
                return json_response({"ok": True, "trace": self.st.trace_text()})
            if path == "/favicon.ico":
                return Response(204, b"", "image/x-icon")
            return json_response({"error": "not found"}, 404)

        if method == "POST":
            data = self._json(body)
            if path.startswith("/api/card/"):
                card_id = path.rsplit("/", 1)[1]
                ok = self.ui.resolve(card_id, data if isinstance(data, dict) else {})
                return json_response({"ok": ok})
            if path == "/api/task":
                return await self._post_task(data)
            if path == "/api/swap":
                return self._post_swap(data)
            if path == "/api/reload":
                self.st.rt.registry.load()
                return json_response({"ok": True, "message":
                                      f"{len(self.st.rt.registry.names())} agents loaded",
                                      "state": self.st.state()})
            if path == "/api/create":
                return await self._post_create(data)
            if path == "/api/stop":
                return json_response({"ok": self.st.stop_dispatch()})
            if path == "/api/shutdown":
                asyncio.get_running_loop().call_later(0.1, self._shutdown)
                return json_response({"ok": True, "message": "shutting down"})
            return json_response({"error": "not found"}, 404)

        return json_response({"error": "method not allowed"}, 405)

    @staticmethod
    def _json(body: bytes) -> dict:
        if not body:
            return {}
        try:
            d = json.loads(body.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            return {}
        return d if isinstance(d, dict) else {}

    def _shutdown(self) -> None:
        for w in list(self._sse_clients):
            w.close()
        if self._server is not None:
            self._server.close()

    # --------------------------------------------------------------- handlers

    async def _post_task(self, data: dict) -> Response:
        text = str(data.get("text", "")).strip()
        if not text:
            return json_response({"error": "text is required"}, 400)
        if self.st.busy():
            return json_response({"error": "a task is already running"}, 409)
        if text.startswith("/"):
            return self._post_slash(text)
        if not self.st.start_dispatch(text):
            return json_response({"error": "a task is already running"}, 409)
        return json_response({"ok": True, "accepted": text})

    def _post_slash(self, text: str) -> Response:
        cmd = text.partition(" ")[0].lower()
        if cmd in ("/create",):
            name = text.partition(" ")[2].strip()
            proposal = {"name": name} if name else {}
            self.st.current_task = asyncio.get_running_loop().create_task(
                self._create_flow(proposal)
            )
            return json_response({"ok": True, "accepted": text})
        if cmd in ("/quit", "/exit", "/q"):
            asyncio.get_running_loop().call_later(0.1, self._shutdown)
            return json_response({"ok": True, "message": "shutting down"})
        result = self.st.slash(text)
        if result is None:
            return json_response({"error": f"unknown command {cmd}"}, 400)
        if result.pop("ok", True) is False:
            return json_response(result, 400)
        return json_response(result)

    async def _create_flow(self, proposal: dict) -> None:
        try:
            la = await self.st.rt.orchestrator.create_flow(proposal)
        except Exception as e:  # noqa: BLE001
            self.ui.event({"type": "error", "text": f"create failed: {e}"})
            return
        finally:
            self.st.current_task = None
        if la is not None:
            self.ui.event({
                "type": "status",
                "text": f"dispatch a task to it, or /swap {la.spec.name} <model>",
            })

    def _post_swap(self, data: dict) -> Response:
        agent, ref = str(data.get("agent", "")).strip(), str(data.get("ref", "")).strip()
        if not agent or not ref:
            return json_response({"error": "agent and ref are required"}, 400)
        try:
            la = self.st.rt.registry.swap(agent, ref)
        except Exception as e:  # noqa: BLE001 — user-facing swap errors
            return json_response({"error": f"swap failed: {e}"}, 400)
        return json_response({"ok": True, "message": f"{la.spec.name} → {la.spec.model}",
                              "state": self.st.state()})

    async def _post_create(self, data: dict) -> Response:
        if self.st.busy():
            return json_response({"error": "a task is already running"}, 409)
        proposal = {k: data[k] for k in ("name", "description", "model", "tools")
                    if k in data}
        self.st.current_task = asyncio.get_running_loop().create_task(
            self._create_flow(proposal)
        )
        return json_response({"ok": True})

    def _static(self, path: str) -> Response:
        fname, ctype = STATIC_FILES[path]
        try:
            content = (STATIC_DIR / fname).read_bytes()
        except OSError:
            return json_response({"error": f"missing asset {fname}"}, 500)
        return Response(200, content, ctype)


_STATUS = {200: "OK", 204: "No Content", 400: "Bad Request", 404: "Not Found",
           405: "Method Not Allowed", 409: "Conflict", 413: "Payload Too Large",
           500: "Internal Server Error"}


async def serve(runtime, ui: WebUI, host: str = "127.0.0.1", port: int = 8417) -> None:
    """Serve an already-assembled runtime (built with this ui) until closed."""
    ui.attach(asyncio.get_running_loop())
    state = ServerState(runtime, ui)
    srv = GuiServer(state)
    await srv.start(host, port)
    try:
        await srv._server.serve_forever()  # noqa: SLF001 — module-local use
    finally:
        await srv.stop()
