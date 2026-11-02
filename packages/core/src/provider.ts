import { chmod, mkdir, open, writeFile } from "node:fs/promises"
import { spawn, spawnSync } from "node:child_process"
import { once } from "node:events"
import path from "node:path"
import { apiKeyFor } from "./config"
import { parseStream } from "./parser"
import { scrubbedEnvironment } from "./safety"
import type { Message, RoleConfig, StreamChunk } from "./types"

export function validateEndpoint(value: string) {
  const url = new URL(value)
  if (url.username || url.password) throw new Error("Provider endpoints cannot contain credentials")
  const loopback = url.hostname === "localhost" || url.hostname === "127.0.0.1" || url.hostname === "::1"
  if (url.protocol !== "https:" && !(url.protocol === "http:" && loopback)) throw new Error("Remote provider endpoints must use HTTPS")
  return url
}

function trimUrl(value: string) {
  return value.replace(/\/+$/u, "")
}

function endpointFor(slot: RoleConfig) {
  if (slot.provider === "ollama") return `${trimUrl(slot.endpoint || "http://127.0.0.1:11434")}/api/chat`
  if (slot.provider === "anthropic") return `${trimUrl(slot.endpoint || "https://api.anthropic.com")}/v1/messages`
  const base = trimUrl(slot.endpoint || "https://api.openai.com/v1")
  return `${base.endsWith("/v1") ? base : `${base}/v1`}/chat/completions`
}

function requestBody(slot: RoleConfig, messages: Message[]) {
  const body: Record<string, unknown> = { model: slot.model, messages, stream: true }
  if (slot.provider === "ollama") {
    body.keep_alive = "30s"
    body.options = { num_ctx: slot.contextSize, num_predict: slot.maxTokens }
  } else if (slot.provider === "anthropic") {
    body.max_tokens = slot.maxTokens
    const system = messages.filter((message) => message.role === "system").map((message) => message.content).join("\n\n")
    body.messages = messages.filter((message) => message.role !== "system")
    if (system) body.system = system
  } else {
    body.max_tokens = slot.maxTokens
  }
  return JSON.stringify(body)
}

async function responseError(response: Response) {
  const text = (await response.text()).slice(0, 1000)
  throw new Error(`Provider returned HTTP ${response.status}: ${text}`)
}

type ManagedServer = { endpoint: string; stop: () => Promise<void> }

async function startGguf(slot: RoleConfig, onStatus: (text: string) => void): Promise<ManagedServer> {
  if (!slot.ggufPath) throw new Error(`No GGUF file configured for ${slot.name}`)
  const logDir = path.join(process.env.XDG_STATE_HOME ?? path.join(process.env.HOME ?? ".", ".local", "state"), "mtrxai", "logs")
  await mkdir(logDir, { recursive: true, mode: 0o700 })
  const logPath = path.join(logDir, `${slot.name}.log`)
  const log = await open(logPath, "a", 0o600)
  await chmod(logPath, 0o600)
  const modern = spawnSync("llama", ["server", "--help"], { stdio: "ignore" }).status === 0
  const command = modern ? "llama" : "llama-server"
  const args = modern ? ["server"] : []
  args.push("--model", slot.ggufPath, "--host", "127.0.0.1", "--port", "0", "--ctx-size", String(slot.contextSize), "--device", "none", "--n-gpu-layers", "0")
  onStatus(`Starting ${command} for ${slot.name}...`)
  const child = spawn(command, args, { env: scrubbedEnvironment(), stdio: ["ignore", log.fd, log.fd] })
  const stop = async () => {
    if (child.exitCode !== null) return
    child.kill("SIGTERM")
    await Promise.race([once(child, "exit"), new Promise((resolve) => setTimeout(resolve, 3000))])
    if (child.exitCode === null) child.kill("SIGKILL")
    await log.close()
  }
  // llama accepts port 0 but reports the selected port in its log. Wait for a
  // local health endpoint by reading the log rather than exposing a fixed port.
  let endpoint = ""
  for (let attempt = 0; attempt < Math.max(1, slot.ggufLoadTimeoutSeconds * 4); attempt++) {
    const text = await Bun.file(logPath).text()
    const match = text.match(/(?:https?:\/\/127\.0\.0\.1:|port\s+)(\d+)/iu)
    if (match?.[1]) {
      endpoint = `http://127.0.0.1:${match[1]}/v1/chat/completions`
      try {
        const health = await fetch(`http://127.0.0.1:${match[1]}/health`, { signal: AbortSignal.timeout(1000) })
        if (health.ok) return { endpoint, stop }
      } catch {}
    }
    if (child.exitCode !== null) break
    await Bun.sleep(250)
  }
  await stop()
  throw new Error(`GGUF server did not become ready; see ${logPath}`)
}

export async function generate(slot: RoleConfig, messages: Message[], sessionStage: string, call: number, onChunk: (chunk: StreamChunk) => void, onStatus: (text: string) => void, signal?: AbortSignal) {
  let managed: ManagedServer | undefined
  let requestSlot = slot
  if (slot.provider === "gguf") {
    managed = await startGguf(slot, onStatus)
    requestSlot = { ...slot, provider: "custom", endpoint: managed.endpoint.replace(/\/chat\/completions$/u, "") }
  }
  const stageDir = path.join(sessionStage, "..")
  await mkdir(sessionStage, { recursive: true, mode: 0o700 })
  const rawPath = path.join(sessionStage, `call-${call}.raw`)
  const metaPath = path.join(sessionStage, `call-${call}.meta.json`)
  const raw = await open(rawPath, "w", 0o600)
  await writeFile(metaPath, JSON.stringify({ provider: slot.provider, provider_kind: slot.provider }) + "\n", { mode: 0o600 })
  try {
    const key = await apiKeyFor(slot)
    if (requestSlot.provider !== "gguf") validateEndpoint(requestSlot.endpoint || (requestSlot.provider === "ollama" ? "http://127.0.0.1:11434" : "https://api.openai.com/v1"))
    const headers: Record<string, string> = { "content-type": "application/json", accept: "text/event-stream" }
    if (requestSlot.provider === "anthropic") {
      headers["x-api-key"] = key ?? ""
      headers["anthropic-version"] = "2023-06-01"
    } else if (key) headers.authorization = `Bearer ${key}`
    const response = await fetch(endpointFor(requestSlot), {
      method: "POST",
      headers,
      body: requestBody(requestSlot, messages),
      redirect: "manual",
      signal: AbortSignal.any([signal ?? new AbortController().signal, AbortSignal.timeout(Math.max(1, slot.timeoutSeconds) * 1000)]),
    })
    if (response.status >= 300 && response.status < 400) throw new Error("Provider redirects are not followed")
    if (!response.ok) await responseError(response)
    if (!response.body) throw new Error("Provider returned no response body")
    const decoder = new TextDecoder()
    let pending = ""
    for await (const chunk of response.body as AsyncIterable<Uint8Array>) {
      await raw.write(chunk)
      await raw.sync()
      pending += decoder.decode(chunk, { stream: true })
      // Parsing the accumulated bytes preserves split JSON/SSE records while
      // the raw file remains the exact provider response.
      const lines = pending.split(/\r?\n/u)
      pending = lines.pop() ?? ""
      parseStream(lines.join("\n") + (lines.length ? "\n" : ""), requestSlot.provider, onChunk)
    }
    pending += decoder.decode()
    if (pending) parseStream(pending, requestSlot.provider, onChunk)
    const result = parseStream(await Bun.file(rawPath).text(), requestSlot.provider)
    if (!result.content) throw new Error("Provider stream produced no content")
    return result
  } finally {
    await raw.close()
    if (managed) await managed.stop()
    void stageDir
  }
}

export async function unload(slot: RoleConfig, onStatus: (text: string) => void) {
  if (slot.provider !== "ollama") return
  const endpoint = `${trimUrl(slot.endpoint || "http://127.0.0.1:11434")}/api/generate`
  validateEndpoint(slot.endpoint || "http://127.0.0.1:11434")
  onStatus(`Unloading Ollama model ${slot.model} before the next role...`)
  const response = await fetch(endpoint, { method: "POST", redirect: "manual", headers: { "content-type": "application/json" }, body: JSON.stringify({ model: slot.model, keep_alive: 0 }), signal: AbortSignal.timeout(10_000) })
  if (!response.ok) await responseError(response)
}
