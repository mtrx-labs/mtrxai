# mtrxAI

mtrxAI is a Linux-first, C++ terminal application that hands one coding request
through three configurable AI roles: Chief of Staff, Prompter, and Coder. It
runs the roles sequentially, saves each completed handoff to disk before
starting the next model, and asks before applying the Coder's proposed files.

## Why sequential

Only one local inference is active at a time. Ollama keeps a role loaded across
its turns, then mtrxAI explicitly unloads it before starting the next role.
Chief and Prompter default to a 4096-token context; Coder defaults to 2048. A
`.gguf` role is served by one short-lived `llama-server` process for its whole
stage, then stopped before the next role. The request, Chief brief, detailed
prompt, Coder responses, and manifest are saved under
`~/.local/state/mtrxai/sessions/<timestamp>/` (or `$XDG_STATE_HOME`). A failure
in a later stage does not discard earlier work. Existing files overwritten
after approval are backed up in that session directory.

Each role can use a different Ollama model, OpenAI, Anthropic, or a local GGUF
file. Provider and model choices are made during setup rather than tied to
machine-specific defaults.

**A single-model-at-a-time strategy reduces peak RAM, but cannot make an
oversized model fit.** The supplied Mellum Q6 GGUF is about 11 GB; on a 16 GB
machine it may leave too little memory for the OS and inference runtime. The
Coder defaults to a 2048-token context to reduce KV-cache usage. If the model
fails to load or the machine swaps heavily, use a smaller quantization (for
example Q4_K_M) or select a remote OpenAI-compatible provider. The GGUF is
never copied or modified by mtrxAI.

## Build (Linux)

Dependencies: C++17 compiler, CMake 3.20+, wide ncurses (ncursesw),
libcurl, OpenSSL, json-c, and pkg-config.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
./build/mtrxai --init
./build/mtrxai
```

Run `mtrxai --init` in an interactive terminal to open the setup wizard. For
each role it offers GGUF, Ollama, OpenAI, and Anthropic and asks for the
provider-specific model, endpoint, file path, or API key. GGUF paths support
`~` expansion and must point to a readable file. API-key entry is hidden.
Keys are saved separately in `credentials.json` beside the config (by default
`~/.config/mtrxai/credentials.json`) with owner-only permissions; they are not
written to `config.json`. This local credentials file is plaintext protected
by filesystem permissions, so protect your account and backups accordingly.
Existing environment-variable keys (`api_key_env`) remain supported.

## Updates

When started from an interactive terminal, mtrxAI checks GitHub for a newer
stable release and asks before installing it. Downloads are HTTPS-only and must
match the SHA-256 digest published in GitHub release metadata. On confirmation,
the new binary is installed atomically and mtrxAI restarts itself with the
current command-line arguments. Configuration, credentials, and saved
conversation/session history live outside the executable and are left intact.
Use `mtrxai --no-update-check` to skip the check for one launch.

Automatic installation replaces the running executable when its directory is
writable. Otherwise, it installs to `~/.local/bin/mtrxai` and restarts from
there; add `~/.local/bin` to your `PATH` to use that copy on subsequent
launches. Linux x86_64 and AArch64 release assets are published when a `v*`
version tag matching the CMake project version is created. Each release also
attaches explicit source archives named `mtrxai-<tag>-source.zip` and
`mtrxai-<tag>-source.tar.gz` in addition to GitHub's automatic source links.

## Linux packages

Prebuilt releases include `.deb` (Debian/Ubuntu), `.rpm` (Fedora/RHEL/openSUSE),
`.pkg.tar.zst` (Arch Linux), and x86_64 `.AppImage` downloads. Install the
distribution package with its normal package manager, for example:

```sh
sudo apt install ./mtrxai-*.deb
sudo dnf install ./mtrxai-*.rpm
sudo pacman -U ./mtrxai-*.pkg.tar.zst
```

DEB, RPM, and Arch packages install the executable as `/usr/bin/mtrxai`, so
after installation launch it by running `mtrxai` from a terminal. The AppImage
is portable and does not install into `PATH`; make it executable and launch its
downloaded file directly. The packages install only the application and its
runtime dependencies; per-user configuration, API credentials, and session
history are stored in the user's home directories and are not removed during
package upgrades or uninstallation.

Install `llama-server` from [llama.cpp](https://github.com/ggml-org/llama.cpp)
for GGUF inference and pull any configured Ollama models before using them.
The GGUF server uses the role's `context_size` (2048 tokens for the Coder) and
explicitly disables GPU layers. Lower a role's `context_size` in the config if
memory pressure requires it; increase it only when there is RAM headroom.

## Providers and hot swapping

Edit `~/.config/mtrxai/config.json` or use TUI commands:

```text
/roles
/sessions
/model chief ollama deepseek-r1:8b
/model prompter ollama qwen2.5:3b
/gguf coder /path/to/your/Mellum2.1-12B-A2.5B-Thinking-Q6_K.gguf
/endpoint coder https://api.example.com/v1
/keyenv coder MY_API_KEY
/apikey coder
/context coder 4096
/maxturns 60
/gguftimeout 300
```

`/context` sets a role's context window in tokens (512-32768). `/maxturns` sets
the Coder turn limit (1-500). `/gguftimeout` sets how long llama-server may take
to load a GGUF model before mtrxAI gives up (1-3600 seconds). The same values
are stored in `config.json` as top-level `max_turns` and
`gguf_load_timeout_seconds`, per-slot `context_size`, and per-slot
`gguf_load_timeout_seconds`.

Set a role's `provider` to `openai` for an OpenAI-compatible `/chat/completions`
endpoint or `anthropic` for Anthropic's native Messages API. Both support
`api_key_env`; wizard-provided keys are loaded from the private
`credentials.json`. `/apikey ROLE` lets you securely enter or clear a key from
inside the TUI after switching providers. A locally running llama.cpp server
can also be used as an OpenAI-compatible endpoint. For direct GGUF use, install
`llama-server`; mtrxAI starts it for the Coder stage and terminates it afterward.

Verified Ollama tags (checked against the Ollama library in October 2026):

```sh
ollama pull deepseek-r1:8b               # Chief of Staff, 5.2 GB
ollama pull qwen3:4b-instruct            # Prompter, 2.5 GB
```

There is no plain `qwen3:4b-instruct-2507` tag; `qwen3:4b-instruct` serves the
same weights (digest 0edcdef34593, the Qwen3-4B-Instruct-2507 build, 256K
context). Pin `qwen3:4b-instruct-2507-q4_K_M` if you prefer an exact quant tag.
The Mellum Coder model is not in the Ollama library; pull the Hugging Face GGUF
with `ollama pull hf.co/JetBrains/Mellum2.1-12B-A2.5B-Thinking-GGUF:Q4_K_M`
(about 7.5 GB) or download a GGUF file and set it with `/gguf coder`. The
Q6_K variant is about 11 GB and will not fit a 16 GB machine alongside the OS;
use Q4_K_M.

## Applying code

The Coder iteratively lists and reads project files, requests individual file
writes, and runs relevant checks. Each proposed write and each shell command is
shown in a paged review screen; nothing is written or executed until you
explicitly approve it. Reads and writes protect sensitive credentials and keys
matching exact names (`.env`, `.netrc`, `.npmrc`, `.pypirc`, `credentials.json`)
and patterns (`.env.*`, `*.pem`, `*.key`, `*.p12`, `*.pfx`, `id_rsa*`, `id_ed25519*`),
exclude build and VCS directories, and return 4 KiB chunks (with offsets) from files up to 2 MiB.
Common source files like `tokenizer.cpp` or `dev.env` remain accessible.
Commands run from the
project root with a minimal environment (API keys are not passed to commands);
they are limited to 300 seconds and 16 KiB of output. File paths
are checked against traversal and symlink escapes. Replaced files are backed
up in the private session folder. Generated commands are not run automatically.

mtrxAI saves the Chief and Prompter handoffs and each Coder turn before moving
on. Every stage additionally records its streamed tokens verbatim in
`<session>/<stage>/raw.jsonl` (owner-only, flushed as chunks arrive, fsynced
about once a second); the derived stage files such as `chief.md` are written
afterwards from that recording, so a killed run still leaves enough state to
rebuild the stage. The Coder gets a bounded project-file listing and inspects
relevant source on demand. Each model role configures `max_tokens` (`num_predict` in Ollama).
Generation requests stream: Ollama uses its native NDJSON stream while
OpenAI-compatible endpoints and Anthropic use SSE, so replies arrive as
incremental chunks and a request can be aborted mid-stream instead of waiting
for one blocking response. The pipeline runs on a worker thread while the TUI
keeps rendering live output and status; press Esc to cancel the current job,
which aborts the active request and keeps completed stage files on disk. The
transcript pane follows the newest output as it grows. Model `<think>...</think>` blocks are stripped before passing outputs to downstream
roles to prevent context pollution, and reasoning is saved in separate session
artifacts (`chief_thinking.md`, `prompt_thinking.md`, `coder_turn_<N>_thinking.md`).
When running local GGUF models, `llama-server` stdout and stderr are
redirected to `$XDG_STATE_HOME/mtrxai/logs/<role>.log` (mode 0600) to keep the TUI
display clean. Signal handlers for SIGINT and SIGTERM cleanly terminate child processes,
unload active Ollama models, restore the terminal (`endwin()`), and exit non-zero.
Avoid using remote endpoints for workspaces whose contents must
remain local. Run the mock-provider integration test with
`ctest --test-dir build --output-on-failure`.

## Sessions and resume

Every task creates a session directory under
`$XDG_STATE_HOME/mtrxai/sessions/<timestamp>/` and records its progress in
`manifest.json`. `mtrxai --resume` continues the most recent unfinished session;
`mtrxai --resume <id>` continues a specific one. Inside the TUI, `/sessions`
lists recent sessions with their id, saved stage, and request. A resumed task
starts immediately.

Resume reuses the saved Chief and Prompter output instead of loading those
models again, so a failure later in the pipeline does not repeat earlier work.
The session records the project directory it belongs to, and resume refuses to
run from a different directory. If the Coder had already started, its unfinished
attempt is kept as `coder.before-resume-<timestamp>` and the Coder begins again
from the saved plan. A session whose manifest says `finished` is reported as
complete without starting any model.

## License

The TUI runs under the terminal's locale and accepts UTF-8 input (multibyte
characters are kept intact while typing, moving, and deleting). Set a UTF-8
locale such as `LANG=C.UTF-8` in your environment for non-ASCII input.

MIT. See [LICENSE](LICENSE).
