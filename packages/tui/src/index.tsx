import { render } from "@opentui/solid"
import { For, Show, createSignal, onCleanup, onMount } from "solid-js"
import { loadConfig, saveConfig, saveCredential, roleConfig, runPipeline, type Config, type PipelineEvent, type RoleName } from "@mtrxai/core"

type Props = { config: Config; resumeId?: string }

function App(props: Props) {
  const [input, setInput] = createSignal("")
  const [lines, setLines] = createSignal<string[]>(["Enter a coding request. Every write and command will require approval."])
  const [approval, setApproval] = createSignal<{ title: string; body: string; resolve: (value: boolean) => void }>()
  const [busy, setBusy] = createSignal(false)
  const [stage, setStage] = createSignal("idle")
  const [abort] = createSignal(new AbortController())
  const add = (line: string) => setLines((current) => [...current.slice(-80), line])
  const event = (value: PipelineEvent) => {
    if (value.type === "stage") {
      setStage(`${value.stage}: ${value.status}`)
      add(`[${value.stage}] ${value.detail}`)
    }
    if (value.type === "log") add(value.text)
    if (value.type === "chunk" && value.chunk.text) add(`${value.chunk.kind === "reasoning" ? "…" : ""}${value.chunk.text}`)
    if (value.type === "approval") setApproval(value)
  }
  const approve = (value: string) => {
    const pending = approval()
    if (!pending) return false
    if (value.toLowerCase() !== "y" && value.toLowerCase() !== "n") return true
    pending.resolve(value.toLowerCase() === "y")
    setApproval(undefined)
    return true
  }
  const command = async (value: string) => {
    const parts = value.trim().split(/\s+/u)
    const name = parts.shift()?.toLowerCase()
    if (name === "/roles") {
      for (const role of ["chief", "prompter", "coder"] as RoleName[]) add(`${role}: ${roleConfig(props.config, role).provider}/${roleConfig(props.config, role).model || roleConfig(props.config, role).ggufPath}`)
      return
    }
    if (name === "/model" && parts.length >= 3) {
      const role = parts.shift() as RoleName
      const slot = roleConfig(props.config, role)
      slot.provider = parts.shift() as typeof slot.provider
      slot.model = parts.join(" ")
      await saveConfig(props.config)
      add(`${role} model changed to ${slot.provider}/${slot.model}`)
      return
    }
    if (name === "/endpoint" && parts.length >= 2) {
      roleConfig(props.config, parts.shift() as RoleName).endpoint = parts.join(" ")
      await saveConfig(props.config)
      add("Endpoint saved")
      return
    }
    if (name === "/keyenv" && parts.length === 2) {
      roleConfig(props.config, parts[0] as RoleName).apiKeyEnv = parts[1]
      await saveConfig(props.config)
      add("API-key environment name saved")
      return
    }
    if (name === "/apikey" && parts.length >= 1) {
      const role = parts.shift() as RoleName
      await saveCredential(role, parts.join(" "))
      add(`Credential ${parts.length ? "saved" : "cleared"} for ${role}`)
      return
    }
    if (name === "/gguf" && parts.length >= 2) {
      const role = parts.shift() as RoleName
      const slot = roleConfig(props.config, role)
      slot.provider = "gguf"
      slot.ggufPath = parts.join(" ")
      await saveConfig(props.config)
      add(`GGUF saved for ${role}`)
      return
    }
    if (name === "/context" && parts.length === 2) {
      roleConfig(props.config, parts[0] as RoleName).contextSize = Math.min(32768, Math.max(512, Number(parts[1])))
      await saveConfig(props.config)
      return
    }
    if (name === "/maxturns" && parts.length === 1) {
      props.config.maxTurns = Math.min(500, Math.max(1, Number(parts[0])))
      await saveConfig(props.config)
      return
    }
    if (name === "/gguftimeout" && parts.length === 1) {
      props.config.ggufLoadTimeoutSeconds = Math.min(3600, Math.max(1, Number(parts[0])))
      for (const role of ["chief", "prompter", "coder"] as RoleName[]) roleConfig(props.config, role).ggufLoadTimeoutSeconds = props.config.ggufLoadTimeoutSeconds
      await saveConfig(props.config)
      return
    }
    if (name === "/sessions") {
      add("Sessions are listed under $XDG_STATE_HOME/mtrxai/sessions; use mtrxai --resume [id].")
      return
    }
    add("Commands: /roles /sessions /model /gguf /endpoint /keyenv /apikey /context /maxturns /gguftimeout")
  }
  const submit = async (value: string) => {
    setInput("")
    if (approve(value)) return
    if (busy()) return
    if (value.startsWith("/")) {
      await command(value)
      return
    }
    if (!value.trim()) return
    setBusy(true)
    add(`> ${value}`)
    try {
      const session = await runPipeline(value, undefined, { config: props.config, onEvent: event, approve: async (title, body) => new Promise((resolve) => setApproval({ title, body, resolve })), signal: abort().signal })
      add(`Session saved: ${session.id}`)
    } catch (error) {
      add(`Error: ${error instanceof Error ? error.message : String(error)}`)
    } finally {
      setBusy(false)
    }
  }
  const cancel = () => {
    abort().abort()
    add("Cancellation requested; completed stage files are preserved.")
  }
  process.once("SIGINT", cancel)
  onCleanup(() => process.off("SIGINT", cancel))
  onMount(() => {
    if (!props.resumeId) return
    setBusy(true)
    add(`Resuming session ${props.resumeId}...`)
    void runPipeline(undefined, props.resumeId, { config: props.config, onEvent: event, approve: async (title, body) => new Promise((resolve) => setApproval({ title, body, resolve })), signal: abort().signal })
      .then((session) => add(`Session saved: ${session.id}`))
      .catch((error) => add(`Error: ${error instanceof Error ? error.message : String(error)}`))
      .finally(() => setBusy(false))
  })
  return (
    <box flexDirection="column" width="100%" height="100%" padding={1} backgroundColor="#071006">
      <box flexDirection="row" justifyContent="space-between" height={1}>
        <text fg="#26ab07">mtrxAI</text>
        <text fg="#76d65d">{busy() ? stage() : "ready"}</text>
      </box>
      <box flexDirection="row" flexGrow={1} gap={2}>
        <box width={24} flexDirection="column" borderStyle="single" borderColor="#197a06" padding={1}>
          <text fg="#26ab07">ROLES</text>
          <text>Chief</text><text>{props.config.chief.provider}/{props.config.chief.model}</text>
          <text>Prompter</text><text>{props.config.prompter.provider}/{props.config.prompter.model}</text>
          <text>Coder</text><text>{props.config.coder.provider}/{props.config.coder.model || props.config.coder.ggufPath}</text>
        </box>
        <box flexDirection="column" flexGrow={1}>
          <scrollbox flexGrow={1} stickyScroll={true}>
            <For each={lines()}>{(line) => <text wrapMode="word">{line}</text>}</For>
          </scrollbox>
          <Show when={approval()}>
            {(pending) => <box borderStyle="double" borderColor="#26ab07" padding={1} height={8}><text fg="#26ab07">{pending().title} — page through this text, then type y or n</text><text wrapMode="word">{pending().body}</text></box>}
          </Show>
          <input width="100%" placeholder={busy() ? "y/n for approval, or Esc/Ctrl-C to cancel" : "request or /command"} value={input()} onInput={setInput} onSubmit={() => void submit(input())} focused={true} />
        </box>
      </box>
      <text fg="#76d65d">Chief → Prompter → Coder · Esc cancels · writes and commands always ask</text>
    </box>
  )
}

export async function runTui(props: Props) {
  const renderer = await render(() => <App {...props} />)
  return renderer
}

export async function loadTuiConfig() {
  return loadConfig()
}
