# mtrxAI

mtrxAI is a Linux-only terminal coding product. Every request passes through
three sequential roles: **Chief of Staff**, **Prompter**, and **Coder**. Each
stage is saved before the next model starts. The Coder can inspect files, but
every file write and shell command is shown in a paged approval screen and
requires `y` before it happens.

## Build

Requires Bun 1.4+, Linux, and a terminal. The application uses only configured
model providers and makes no telemetry calls.

```sh
bun install
bun run typecheck
bun test
bun run dev -- --init
bun run dev
```

The executable entry point is `mtrxai`. Configuration is stored under
`$XDG_CONFIG_HOME/mtrxai` (or `~/.config/mtrxai`), credentials are in a separate
0600 file, and sessions are under `$XDG_STATE_HOME/mtrxai/sessions` (or
`~/.local/state/mtrxai/sessions`).

## Providers

Each role can independently use Ollama, OpenAI, Anthropic, a custom
OpenAI-compatible HTTPS endpoint, or a local GGUF served by a short-lived
`llama` process. Chief and Prompter default to 4096 context tokens; Coder
defaults to 2048. Providers are unloaded or stopped before the next role.

Use `mtrxai --init` for setup. During a session, `/roles`, `/model`, `/gguf`,
`/endpoint`, `/keyenv`, `/apikey`, `/context`, `/maxturns`, and `/gguftimeout`
change configuration without restarting.

## Sessions

`mtrxai --resume [id]` reuses saved Chief and Prompter stages and rebuilds the
Coder from `messages.jsonl`. An action saved without a result is shown again for
approval; writes and commands are never replayed automatically.

See [docs/MERGE_PLAN.md](docs/MERGE_PLAN.md) for the retained architecture and
the porting map from the source projects.
