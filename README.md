# mtrxAI

One router model receives your task, decides which specialist agent (Coder /
Researcher / Writer / …) handles it best, and dispatches. If no suitable agent
exists, it drafts a new one **and asks for your approval before creating it**.
Every model — including the router's own — is **hot-swappable** between API-key
cloud models and local models (Ollama / raw `.gguf`) at runtime, without a
restart. Ships with a Textual TUI (voice-capable: say **"hey matrix"**), a browser
web GUI (dark/light themes built on a #26ab07 accent palette), and a plain
REPL.

```
user ──▶ TUI / REPL ──▶ Orchestrator (router agent)
                         │ route JSON          │ proposal: new agent
                         ▼                     ▼
                   Agent Registry ◀── approval gate ── agent factory
                   (agents/*.yaml, live-reloaded)
                         ▼
              Coder · Researcher · Writer · <dynamic> …
                         ▼
              Tool layer (jailed, permission-gated) ── approval gate
                         ▼
              Model abstraction layer  ← hot-swap happens here
              ollama:* · gguf:<path> · openai:* · anthropic:* · llamacpp:* (any
              OpenAI-compatible endpoint) · fake:* (offline)
```

## Quick start (Fedora)

```bash
sudo dnf install -y python3-pip gcc gcc-c++ make cmake python3-devel git
cd ~/mtrxAI
python3 -m venv .venv && source .venv/bin/activate
pip install -e ".[tui,web]"
mtrxai                       # TUI      (or: mtrxai --mode repl | --mode web)
```

Zero-model demo (works offline, exercises the full loop):

```bash
mtrxai --mode repl
/swap router fake:demo
/swap writer fake:demo
hello                        # → routed → writer → canned answer
/swap coder fake:tools       # replies with a write_file tool call, then an answer
write me a demo file
```

With Ollama (the intended local backend):

```bash
curl -fsSL https://ollama.com/install.sh | sh    # once
sudo systemctl enable --now ollama
ollama pull qwen2.5:3b          # router
ollama pull qwen2.5-coder:7b    # coder
ollama pull llama3.1:8b         # researcher / writer
```

API models: `cp .env.example .env && chmod 600 .env`, fill `OPENAI_API_KEY`
and/or `ANTHROPIC_API_KEY`, then e.g. `/swap coder openai:gpt-4o`.

## Everyday use

| You do | What happens |
|---|---|
| type a task | router picks an agent (JSON + repair retry + keyword fallback), agent runs a tool loop, answer lands in the log |
| agent hits a dangerous tool | approval card: **y** / **a**lways-this-session / **n**o |
| task fits no agent | router proposes a new one → approval card (edit name/model/tools) → `agents/<name>.yaml` written → dispatched |
| `ctrl+s` or `/swap coder openai:gpt-4o` | hot-swap that agent's model mid-session, persisted to YAML |
| `ctrl+r` or `/reload` | re-read every agent YAML (also picked up lazily by mtime) |
| `ctrl+t` or `/trace` | last run's routing/tool trace |
| `/agents` | live roster with current model refs |
| `/create [name]` | manual agent-creation wizard |
| `ctrl+q` | quit |

Swapping away from a `.gguf:` agent unloads it from RAM/VRAM immediately
(`free -h` / `nvidia-smi` to watch it happen). YAML edits + `/reload` are the
same operation as `/swap` — both persist.

## Model reference grammar

| Reference | Resolves to |
|---|---|
| `ollama:qwen2.5-coder:7b` | Ollama on `127.0.0.1:11434` |
| `gguf:/abs/path/model-q4_k_m.gguf` | in-process llama-cpp-python (needs `.[gguf]`) |
| `openai:gpt-4o` | OpenAI API (`$OPENAI_API_KEY`) |
| `anthropic:claude-sonnet-4-20250514` | Anthropic Messages API |
| `llamacpp:my-model` | any OpenAI-compatible endpoint in `settings.toml` (llama-server, LM Studio, vLLM, OpenRouter, Groq…) |
| `fake:demo` / `fake:tools` / `fake:error` | deterministic offline backend for demos & tests |

Add endpoints by editing `[providers.*]` in `settings.toml`; no restart needed
(`/reload` or the lazy mtime check re-reads it on the next launch).

## Web GUI

A browser frontend that drives the **same runtime** as the TUI/REPL — same
router dispatch, same approval gates, same hot-swap — over stdlib asyncio HTTP
+ SSE (no extra dependencies).

```bash
mtrxai --mode web                 # → http://127.0.0.1:8417
mtrxai --mode web --port 9000     # custom port
mtrxai --mode web --host 0.0.0.0  # only if you really mean it
```

| In the TUI you… | In the GUI you… |
|---|---|
| type a task | type in the composer — routing, tool calls, results and the answer stream live into the log |
| approval card y / a / n | **allow once / always this session / deny** buttons (or `y` / `a` / `n` keys) |
| agent-proposal card | same card, editable inline, Create/Cancel |
| `ctrl+s` / `/swap` | **⇄ swap** button, or type `/swap coder openai:gpt-4o` |
| `ctrl+r` / `/reload` | **⟳ reload** button or `/reload` |
| `ctrl+t` / `/trace` | **⌗ trace** button or `/trace` |
| `/agents` | sidebar roster (always live) or `/agents` |
| `/create [name]` | **+ agent** button or `/create [name]` |
| `ctrl+q` | **⏻** button or `/quit` |
| — | ■ stop button cancels a running dispatch |
| — | ◐ button toggles **dark / light** (persisted per browser) |

The palette is code: `mtrxai/gui/theme.py` derives a full token set from the
#26ab07 accent and serves it as `/theme.css`; both themes are contrast-checked
by `tests/test_gui.py`. Closing the browser while an approval card is open
denies the action — nothing runs unattended. Voice is TUI-only.

## RPM package

Build a Fedora RPM (no sudo needed for the *build* itself):

```bash
# one-time build deps (on the build machine)
sudo dnf install -y rpmbuild python3-setuptools python3-pytest python3-pytest-asyncio \
    python3-httpx python3-pyyaml python3-rich python3-textual python3-numpy

./packaging/build-rpm.sh
# → ~/rpmbuild/RPMS/noarch/mtrxai-<version>-1.fc*.noarch.rpm
```

Install anywhere Fedora-ish:

```bash
sudo dnf install ./mtrxai-0.2.0-1.fc44.noarch.rpm
mtrxai --mode web          # or tui / repl
```

The RPM requires Fedora's `python3-{httpx,pyyaml,rich,textual}` — nothing is
bundled, no pip at install time. RPM installs ship the default agent roster to
`/usr/share/mtrxai/` — bootstrap a project with:

```bash
cp -r /usr/share/mtrxai ~/mtrxai && cd ~/mtrxai && mtrxai --mode web
```

The `web` extra (`ddgs`) and the `voice` extra are not packaged: install them
in a venv (`pip install -e '.[voice]'`) if you need them. Version bumps come
from `pyproject.toml`; the spec's `Source0` pulls the matching GitHub tag.

## Voice — "hey matrix"

Fully optional, fully local by default (openWakeWord + webrtcvad + faster-whisper
+ Piper). Your custom wake model is at `assets/wake/mtrxAI.onnx`.

```bash
sudo dnf install -y portaudio          # sounddevice backend
pip install -e ".[voice]"
python -m mtrxai.voice.setup           # checks + downloads piper voice & wake feature models
python -m mtrxai.voice.calibrate       # live scores → pick wake_threshold (§settings)
mtrxai --voice                         # or ctrl+v inside the TUI
```

Say **"hey matrix"** → recording → transcribing → the utterance enters the same
dispatch path as typed text. Voice-initiated answers are spoken back (Piper;
`speak_responses = false` in `settings.toml` to mute). `Esc` barges in.
Swap STT/TTS like any model: `stt = "openai:whisper-1"`,
`tts = "openai:tts-1"` in `[voice]`.

Useful: `python -m mtrxai.voice.calibrate --list-devices` (pick `input_device`),
`--selftest` (no mic needed).

## Security model

- File tools jailed to `workspace/` (`resolve()` + `is_relative_to`).
- `run_shell` defaults to **ask**, cwd = workspace, 120 s timeout, output truncated.
- Agent creation always requires explicit approval.
- API keys only via `.env` (`chmod 600`); never in YAML, never logged.
- All HTTP has timeouts; the tool loop is capped by `max_steps`.

## Development

```bash
pip install -e ".[tui,web,dev]"
pytest                          # whole suite: no GPU, no keys, no mic, no network
```

Layout: `mtrxai/models` (backends + factory) · `mtrxai/core` (registry,
orchestrator, approval, trace) · `mtrxai/agents` (tool loop, factory) ·
`mtrxai/tools` · `mtrxai/tui` · `mtrxai/gui` (web frontend + palette) ·
`mtrxai/voice`. Agents live in `agents/*.yaml` — they're data, not code.

## Troubleshooting (Fedora)

| Symptom | Fix |
|---|---|
| `externally-managed-environment` | use the venv (PEP 668) |
| `Ollama is not reachable` | `systemctl start ollama` / `ollama serve` |
| `model 'x' is not in Ollama` | `ollama pull x` |
| mic won't open at 16 kHz | PipeWire handles it; if not, we auto-resample — else set `input_device` |
| whisper first run stalls | one-time model download to `~/.cache/huggingface` |
