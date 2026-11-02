# mtrxAI merge plan

## Scope and observed architecture

The source tree is mtrxAI 2.2.0. `packages/cli/src/index.ts` owns command
dispatch and process lifetime. Its run path creates an HTTP client and normally
starts a standalone `serve` child. `packages/server` exposes the HTTP and event
stream API; `packages/core` owns configuration, locations, sessions, model
execution, tools, permissions, filesystem and persistence. `packages/client`
contains generated Promise/Effect/Solid clients. `packages/tui` renders the
terminal application and reads location-scoped data through the client. The
remaining UI packages provide shared Solid/OpenTUI components and themes.

The mtrxAI source is a complete C++ reference implementation rather than a
subprocess dependency. Its `Pipeline`, `InferenceSession`, `Session`, safety
helpers, parser, transcript, signal, setup, TUI and updater modules are the
behavioral source of truth. The TypeScript implementation will host those
behaviors in the retained CLI/Core/TUI path.

## Keep/remove map

| Tree | Decision | Reason |
| --- | --- | --- |
| `packages/cli` | Keep, simplify | Linux `mtrxai` binary, `--init`, `--resume`, slash-command routing, lifecycle and updater. |
| `packages/tui` | Keep, simplify | Terminal layout, sidebar, transcript, input, dialogs and approval pager. |
| `packages/core` | Keep, rewrite around pipeline | Sequential roles, providers, sessions, safe filesystem and approved command execution. |
| `packages/ai` | Keep only provider-stream primitives if useful | Provider request/stream decoding can host the five mtrxAI provider kinds; remove unrelated media and hosted integrations. |
| `packages/client` | Keep only if the TUI/server boundary remains | Generated local API client for the retained CLI/TUI connection. |
| `packages/protocol` | Keep only pipeline/session events | Typed local API contract; remove unrelated handlers and generated operations. |
| `packages/schema` | Keep only config/session schemas | Runtime validation for role configuration and persisted artifacts. |
| `packages/theme` | Keep and replace defaults | Shared TUI token resolution with mtrxAI green `#26ab07` range. |
| `packages/ui` | Keep only components imported by retained TUI | Shared OpenTUI/Solid controls; delete browser-only and product-specific UI. |
| `packages/util` | Keep used filesystem, process, path, text and error helpers | Small shared runtime utilities needed by CLI/Core/TUI. |
| `packages/server` | Keep as local-only server or fold into CLI | Hosts the TUI event stream and pipeline API without cloud/network service behavior. |
| `packages/sdk` | Remove | No external SDK is part of the Linux terminal product. |
| `packages/app` | Remove | Web client is outside scope. |
| `packages/desktop` | Remove | Desktop client and packaging are outside scope. |
| `packages/web` | Remove | Web application and hosted UI are outside scope. |
| `packages/console` | Remove | Hosted console is outside scope. |
| `packages/stats` | Remove | Telemetry/usage reporting violates the no-telemetry decision. |
| `packages/posts` | Remove | Marketing/content product surface is outside scope. |
| `packages/stories` | Remove | Browser story application is outside scope. |
| `packages/session-ui` | Remove | Browser session UI is outside scope; terminal session UI stays in TUI. |
| `packages/gui-plugins` | Remove | GUI plugin surface is outside scope. |
| `packages/plugin` | Remove | Plugin system is explicitly removed. |
| `packages/plugin-browser` | Remove | Browser plugin runtime is explicitly removed. |
| `packages/plugins` | Remove | Built-in plugin packages are explicitly removed. |
| `packages/codemode` | Remove | Codemode/plugin execution is not an mtrxAI tool. |
| `packages/function` | Remove | Hosted function integration is outside scope. |
| `packages/identity` | Remove | Accounts and identity are outside scope. |
| `packages/effect-drizzle-sqlite` | Remove | The product uses session files, not hosted/database identity storage. |
| `packages/http-recorder` | Remove | Recording infrastructure is not a product feature. |
| `packages/httpapi-codegen` | Remove | No public generated API is shipped. |
| `packages/latex` | Remove | Document rendering is not needed by the terminal pipeline. |
| `packages/merman` | Remove | Browser diagram rendering is outside scope. |
| `packages/patchmark` | Remove | mtrxAI patch presentation is replaced by explicit mtrxAI approvals. |
| `packages/process-daemon` | Remove | No shared hosted/background daemon is needed. |
| `packages/simulation` | Remove | Simulation harness is not shipped. |
| `packages/effect-drizzle-sqlite` | Remove | Database adapter is not needed for file-backed sessions. |
| `packages/theme` | Keep | TUI color/token foundation. |
| `packages/script` | Keep temporarily, then minimize | Build/typecheck and release scripts only. |
| `services/update` | Remove after updater port | Hosted update service is not needed; retain only client-side verified updater logic. |
| `services/www` | Remove | Docs/marketing site is outside scope. |
| `services/changelog` | Remove | Hosted release tooling is outside scope. |
| `services/files` | Remove | Cloud file storage is outside scope. |
| `infra` | Remove | Cloud infrastructure is outside scope. |
| `salesforce` | Remove | Enterprise integration is outside scope. |
| `sdks` | Remove | No editor/host SDK is shipped. |
| `nix` | Remove | Linux packaging can be added only if it serves the final binary; the requested product has no packaging requirement beyond the binary. |
| `specs` | Remove | mtrxAI public API specifications are not shipped. |
| `perf` | Remove | mtrxAI performance harness is not product code. |
| `patches` | Remove unused entries | Dependencies for deleted clients/integrations must not remain. |
| `.mtrxai` | Replace/minimize | Retain only contributor/build metadata; remove mtrxAI commands, plugins, themes and product docs. |
| `.github` | Replace/minimize | Linux-only CI and issue metadata with mtrxAI branding. |

### Major retained-module decisions

| Module | Decision | Host mapping |
| --- | --- | --- |
| CLI command registry and handlers | Keep only default/run/init/resume/version/update | Maps mtrxAI command-line entry points and `/sessions` support. |
| CLI standalone/server process | Simplify or remove the network listener | Prefer one local process; if the current TUI needs an event boundary, keep loopback-only local transport with no remote service. |
| Core session runner | Rewrite | Port `src/pipeline.cpp`, `session.cpp`, and `coder_transcript.cpp`; one pipeline lock and one active provider at a time. |
| Core provider/model code | Rewrite | Port `provider.cpp`, `provider_stream.cpp`, `endpoint.cpp`; Ollama, OpenAI, Anthropic, custom OpenAI-compatible, and short-lived llama.cpp GGUF. |
| Core tools | Rewrite | Port `files.cpp` and command execution with approval, 4 KiB reads, 2 MiB cap, backups, path/symlink and sensitive-file checks. |
| Core config/credentials | Rewrite | Port `config.cpp`; role slots, endpoint validation, 0600 separate credentials and `MTRXAI_*` environment names. |
| Core event/persistence | Keep minimal | Stage events and incremental raw/meta/transcript writes feed the TUI and resume. |
| TUI app/context/data | Simplify | Replace generic session/provider data with three-role pipeline state while preserving transcript follow and keyboard input. |
| TUI approval/dialog components | Keep/adapt | Implement paged y/n review for every write and command. |
| TUI model/config dialogs | Adapt | Implement `/roles`, `/model`, `/gguf`, `/endpoint`, `/keyenv`, `/apikey`, `/context`, `/maxturns`, `/gguftimeout`. |
| Theme files | Rewrite defaults | Plain `mtrxAI` branding and green accent family based on `#26ab07`. |
| Updater | Port last | Preserve HTTPS, SHA-256 verification, atomic install and ask-before-install from `src/update.cpp`. |

## mtrxAI feature mapping

| mtrxAI behavior | mtrxAI host |
| --- | --- |
| Chief of Staff → Prompter → Coder | Core pipeline/session runner, called by the retained CLI and surfaced through TUI events. |
| Per-role provider/model/context/max tokens/endpoint/key env/timeouts | Core config schema and credentials store; TUI command handlers mutate the in-memory config and persist it. |
| Ollama/OpenAI/Anthropic/custom/GGUF | Core provider adapter boundary; provider streaming emits normalized content/reasoning chunks. |
| Strict sequential unload/stop | Pipeline stage guard and provider session finalizer; no next role starts until the previous provider is released. |
| Think stripping and separate reasoning files | Core reply parser and session artifact writer; TUI receives content and reasoning as separate events. |
| Coder tools | Core tool registry limited to list/read/write/run/finish. |
| Explicit approval | TUI approval dialog/pager; Core blocks the worker on a one-shot approval callback and only executes an approved action. |
| Safety restrictions | Core filesystem/command boundary, before reads, writes, symlink resolution and process spawn. |
| Scrubbed command environment | Core command runner constructs an allowlisted environment and omits configured API-key names/values. |
| Session files and resume | Core file-backed session module under `$XDG_STATE_HOME/mtrxai/sessions`; CLI `--resume`; TUI `/sessions`. |
| Raw response persistence | Core provider stream wrapper writes `call-N.raw` incrementally and metadata before parsing. |
| Rejected Coder replies | Core transcript/rejection logic with one correction and one retry. |
| Cancellation and signals | CLI process lifecycle plus Core cancel token; provider and llama child receive cancellation, terminal is restored. |
| Setup wizard | CLI `--init` entry and TUI configuration forms, backed by the Core config/credentials module. |
| Slash commands | TUI command registry and CLI-independent command handlers. |
| Self-updater | Retained CLI service, ported only after functional pipeline and TUI are stable. |

## Phase 1 rename inventory

1. Rename the repository, root package and binary to `mtrxai`; all workspace
   package names become `@mtrxai/*`.
2. Rename package directories, files, generated artifacts, lockfile keys,
   workspace references, bin entries, scripts and CI job names.
3. Replace case-insensitively: `mtrxai`, `mtrxAI`, `MTRXAI`, `mtrxai`,
   `mtrxai.dev`, `@mtrxai/*`, `mtrxai`, `mtrx-labs`, `mtrxai`, and
   legacy product abbreviations in source, docs, comments, fixtures, URLs,
   schema IDs, user agents and ASCII art.
4. Rename environment variables to `MTRXAI_*`, including config directory,
   service lease, client/channel and updater variables; delete telemetry
   environment handling rather than renaming it.
5. Set runtime paths to `~/.config/mtrxai`, `~/.local/share/mtrxai`,
   `~/.local/state/mtrxai` with XDG overrides, and session paths to the latter.
6. Replace logos and user-facing names with plain `mtrxAI`; set the retained
   TUI theme accent and semantic green tokens to `#26ab07` and compatible green
   shades.
7. Move the mtrxAI MIT copyright and permission text to
   `THIRD_PARTY_NOTICES.md`; keep the root `LICENSE` as MIT for mtrxAI. The
   old name may remain only in that notice.
8. Verify after each rename/deletion batch using case-insensitive searches over
   file contents, path names and lockfiles; the only allowed match is
   `THIRD_PARTY_NOTICES.md`.

## Verification order

After the initial rename, prune package manifests and lockfiles, then typecheck
the retained packages. Port the provider/session/safety tests from the C++
source, add parser/safety/resume unit tests, and run them as a non-root user.
Exercise the CLI with mock providers and a temporary project. Finish with the
updater, a clean Linux build with warnings enabled, and the final case-
insensitive grep proof.
