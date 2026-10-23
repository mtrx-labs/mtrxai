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

Dependencies: C++17 compiler, CMake 3.20+, ncurses, libcurl, OpenSSL, json-c,
and pkg-config.

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
/model chief ollama deepseek-r1:8b
/model prompter ollama qwen2.5:3b
/gguf coder /path/to/your/Mellum2.1-12B-A2.5B-Thinking-Q6_K.gguf
/endpoint coder https://api.example.com/v1
/keyenv coder MY_API_KEY
/apikey coder
```

Set a role's `provider` to `openai` for an OpenAI-compatible `/chat/completions`
endpoint or `anthropic` for Anthropic's native Messages API. Both support
`api_key_env`; wizard-provided keys are loaded from the private
`credentials.json`. `/apikey ROLE` lets you securely enter or clear a key from
inside the TUI after switching providers. A locally running llama.cpp server
can also be used as an OpenAI-compatible endpoint. For direct GGUF use, install
`llama-server`; mtrxAI starts it for the Coder stage and terminates it afterward.

## Applying code

The Coder iteratively lists and reads project files, requests individual file
writes, and runs relevant checks. Each proposed write and each shell command is
shown in a paged review screen; nothing is written or executed until you
explicitly approve it. Reads skip private/secret-like paths, exclude build and
VCS directories, and return 4 KiB chunks (with offsets) from files up to 2 MiB.
Commands run from the
project root with a minimal environment (API keys are not passed to commands);
they are limited to 300 seconds and 16 KiB of output. File paths
are checked against traversal and symlink escapes. Replaced files are backed
up in the private session folder. Generated commands are not run automatically.

mtrxAI saves the Chief and Prompter handoffs and each Coder turn before moving
on. The Coder gets a bounded project-file listing and inspects relevant source
on demand. Avoid using remote endpoints for workspaces whose contents must
remain local. Run the mock-provider integration test with
`ctest --test-dir build --output-on-failure`.

## License

MIT. See [LICENSE](LICENSE).
