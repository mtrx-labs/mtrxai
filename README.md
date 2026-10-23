# mtrxAI

mtrxAI is a Linux-first, C++ terminal application that hands one coding request
through three configurable AI roles: Chief of Staff, Prompter, and Coder. It
runs the roles sequentially, saves each completed handoff to disk before
starting the next model, and asks before applying the Coder's proposed files.

## Why sequential

Only one local inference is active at a time. Ollama receives `keep_alive: 0`
for every stage, asking it to unload the model when generation finishes, and
each local role has a bounded context (`4096` tokens by default). A
`.gguf` role is served by a short-lived `llama-server` process and stopped after
its stage. The request, Chief brief, detailed prompt, Coder response, and
manifest are saved under
`~/.local/state/mtrxai/sessions/<timestamp>/` (or `$XDG_STATE_HOME`). A failure
in a later stage does not discard earlier work. Existing files overwritten
after approval are backed up in that session directory.

The default Prompter is `qwen2.5:3b`: it is small enough for a 16 GB, CPU-only
machine while being capable of turning a task brief into a structured coding
prompt. Model choices are defaults, not dependencies; each role can use a
different Ollama model, an OpenAI-compatible API, or a local GGUF file.

## Build (Linux)

Dependencies: C++17 compiler, CMake 3.20+, ncurses, libcurl, json-c, and
pkg-config.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
./build/mtrxai --init
./build/mtrxai
```

The default assignments are:

| Role | Default |
|---|---|
| Chief of Staff | Ollama `deepseek-r1:8b` |
| Prompter | Ollama `qwen2.5:3b` |
| Coder | `~/cooking/Mellum2.1-12B-A2.5B-Thinking-Q6_K.gguf` via llama.cpp |

Pull the Ollama models before using them, and install `llama-server` from
[llama.cpp](https://github.com/ggml-org/llama.cpp) for GGUF inference. The
coder GGUF path can be changed in `~/.config/mtrxai/config.json` or from the
TUI. The GGUF server uses a 4096-token context by default and explicitly disables
GPU layers. Lower a role's `context_size` in the config if memory pressure
requires it; increase it only when there is RAM headroom.

## Providers and hot swapping

Edit `~/.config/mtrxai/config.json` or use TUI commands:

```text
/roles
/model chief ollama deepseek-r1:8b
/model prompter ollama qwen2.5:3b
/gguf coder ~/cooking/Mellum2.1-12B-A2.5B-Thinking-Q6_K.gguf
/endpoint coder https://api.example.com/v1
/keyenv coder MY_API_KEY
```

For an OpenAI-compatible provider, set `provider` to `openai`, configure
`base_url` (the service root or its `/v1` URL), `model`, and optionally
`api_key_env`. The API key is read from that environment variable and is never
written to the config. A locally running llama.cpp server can also be used as
an OpenAI-compatible endpoint. For direct GGUF use, install `llama-server`;
mtrxAI starts it for the Coder stage and terminates it afterward.

## Applying code

The Coder returns a JSON proposal containing complete file contents. mtrxAI
validates paths, refuses path traversal and symlinks, shows the proposed files,
and waits for explicit approval before writing. It never executes generated
shell commands. It passes a bounded snapshot of project paths and selected
source/documentation files to the Coder; avoid using remote endpoints for
workspaces whose contents must remain local. Declining leaves the response
available in `coder.md`.

## License

MIT. See [LICENSE](LICENSE).
