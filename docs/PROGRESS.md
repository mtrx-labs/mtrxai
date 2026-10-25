# mtrxAI engine progress

Prompt 1 of 2 (engine work) is the source of truth for this file. The TUI was
already rewritten with FTXUI before this work and is kept; engine changes adapt
to it rather than replacing it.

## PR 1 — tests and helper extraction

**Plan (5 lines).**
1. Move path/name safety checks to `include/mtrxai/safety.hpp` and `src/safety.cpp`.
2. Move model-reply JSON extraction to `include/mtrxai/textutil.hpp` and `src/textutil.cpp`.
3. Add characterization unit tests for both helpers.
4. Keep the doctest CMake target behind `BUILD_TESTING` and verify CI runs as non-root.
5. Skip the root-bypassed `/proc` permission assertion in the pty test and print why.

**Done.**
- `sensitive_filename()` and `checked_target()` now live in `safety`.
- `extract_json()` now lives in `textutil`; behavior is unchanged.
- Unit tests cover sensitive names/patterns/false positives, path behavior, and JSON extraction.
- `.github/workflows/ci.yml` builds with `BUILD_TESTING=ON`, asserts `id -u` is not 0, and runs `ctest`.
- The setup wizard constructs its no-echo guard before printing the secret prompt.
- The e2e test skips the `/proc/$PPID/environ` permission assertion as root and prints the reason.
- `AGENTS.md` documents the build, non-root test command, safety constraints, and style rules.
- Build and ctest pass locally as uid 1000.

**Known risks / not run.** Real Ollama, real GGUF/llama-server, and non-Ubuntu
distributions were not exercised. CI uses FetchContent, so the first configure
needs network access.

## PR 2 — correctness and safety

**Plan (5 lines).**
1. Keep exact sensitive-file names/patterns and prove the false positives stay readable.
2. Redirect llama-server stdout/stderr to a private role log and stdin to `/dev/null`.
3. Wire per-role `max_tokens` into all three providers, including Anthropic.
4. Strip reasoning tags, persist `chief.reasoning.md`/`prompt.reasoning.md`, and add `CancelToken`.
5. Check cancellation in the pipeline, llama-server wait, command runner, and Coder loop; return 130 on SIGINT.

**Done.**
- `sensitive_filename()` matches exact names, `.env.*`, key/certificate suffixes, and SSH key prefixes only.
- `llama-server` writes to `$XDG_STATE_HOME/mtrxai/logs/<role>.log` (fallback `~/.local/state`), mode 0600; stdin is `/dev/null`; log directory is owner-only.
- `ModelSlot::max_tokens` defaults to 4096 for old configs and maps to Ollama `num_predict`, OpenAI `max_tokens`, and Anthropic `max_tokens`.
- `extract_thinking()` handles canonical and legacy tags, missing opening tags, and unclosed blocks. Chief/Prompter reasoning is saved as `chief.reasoning.md` and `prompt.reasoning.md`.
- `include/mtrxai/cancel.hpp` provides `CancelToken` (`cancel()`, `cancelled()`) and `process_cancel_token()`.
- SIGINT/SIGTERM handlers only set the process token; the pipeline, llama-server startup, command runner, and Coder loop observe it. The TUI stops its worker, unloads Ollama, restores the terminal, and returns `128 + signal` (130 for SIGINT).
- `Config::max_turns` (default 40) and GGUF load timeout (default 150 s) load from old and new configs.

**Next.** Phase 2: raw response body bytes, provider-specific replay, and live
events driven by that same incremental replay parser.

**Known risks / not run.** Real Ollama/GGUF and real Anthropic streaming were not run;
their protocols are covered by unit tests and mock servers.

**Merge status.** Phase 0 and Phase 1 implementation work is present in local
commits, but neither is contained in `origin/main` as of the verified remote
head `5c1b08b`; they are not merged upstream yet.
