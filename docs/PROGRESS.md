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
- Build and ctest pass locally as uid 1000.

**Next.** PR 2: exact sensitive-file matching, llama-server log redirection, per-role
`max_tokens`, reasoning extraction/persistence, cancellation token, and configurable
turn/timeout limits.

**Known risks / not run.** Real Ollama, real GGUF/llama-server, and non-Ubuntu
distributions were not exercised. CI uses FetchContent, so the first configure
needs network access.
