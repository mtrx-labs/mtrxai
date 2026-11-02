import type { ProviderKind, StreamChunk } from "./types"

export type ParsedReply = { content: string; reasoning: string }

function field(value: unknown, key: string) {
  if (!value || typeof value !== "object") return ""
  const result = (value as Record<string, unknown>)[key]
  return typeof result === "string" ? result : ""
}

class ThinkStripper {
  private pending = ""
  private inThink = false
  readonly result: ParsedReply = { content: "", reasoning: "" }

  push(text: string, kind: StreamChunk["kind"], emit: (chunk: StreamChunk) => void) {
    if (kind === "reasoning") {
      this.result.reasoning += text
      if (text) emit({ kind, text })
      return
    }
    this.pending += text
    this.flush(false, emit)
  }

  finish(emit: (chunk: StreamChunk) => void) {
    this.flush(true, emit)
  }

  private flush(final: boolean, emit: (chunk: StreamChunk) => void) {
    const open = "<think>"
    const close = "</think>"
    while (this.pending) {
      if (this.inThink) {
        const end = this.pending.toLowerCase().indexOf(close)
        if (end >= 0) {
          this.emit("reasoning", this.pending.slice(0, end), emit)
          this.pending = this.pending.slice(end + close.length)
          this.inThink = false
          continue
        }
        if (final) {
          this.emit("reasoning", this.pending, emit)
          this.pending = ""
          continue
        }
        const keep = Math.max(0, close.length - 1)
        if (this.pending.length > keep) {
          this.emit("reasoning", this.pending.slice(0, -keep), emit)
          this.pending = this.pending.slice(-keep)
        }
        return
      }
      const start = this.pending.toLowerCase().indexOf(open)
      if (start >= 0) {
        this.emit("content", this.pending.slice(0, start), emit)
        this.pending = this.pending.slice(start + open.length)
        this.inThink = true
        continue
      }
      if (final) {
        this.emit("content", this.pending, emit)
        this.pending = ""
        return
      }
      const keep = open.length - 1
      if (this.pending.length > keep) {
        this.emit("content", this.pending.slice(0, -keep), emit)
        this.pending = this.pending.slice(-keep)
      }
      return
    }
  }

  private emit(kind: StreamChunk["kind"], text: string, emit: (chunk: StreamChunk) => void) {
    if (!text) return
    this.result[kind] += text
    emit({ kind, text })
  }
}

export function parseStream(raw: string, provider: ProviderKind, onChunk: (chunk: StreamChunk) => void = () => {}) {
  const reply = new ThinkStripper()
  const lines = raw.split(/\r?\n/u)
  for (const line of lines) {
    const payload = provider === "ollama" ? line.trim() : line.startsWith("data:") ? line.slice(5).trim() : ""
    if (!payload || payload === "[DONE]") continue
    let value: unknown
    try {
      value = JSON.parse(payload)
    } catch {
      continue
    }
    if (value && typeof value === "object" && "error" in value) throw new Error(String(field(value, "error") || "Provider stream error"))
    if (provider === "ollama") {
      const message = value && typeof value === "object" ? (value as Record<string, unknown>).message : undefined
      reply.push(field(message, "thinking") || field(message, "reasoning"), "reasoning", onChunk)
      reply.push(field(message, "content"), "content", onChunk)
      continue
    }
    if (provider === "anthropic") {
      if (field(value, "type") !== "content_block_delta") continue
      const delta = value && typeof value === "object" ? (value as Record<string, unknown>).delta : undefined
      const kind = field(delta, "type") === "thinking_delta" ? "reasoning" : "content"
      reply.push(field(delta, kind === "reasoning" ? "thinking" : "text"), kind, onChunk)
      continue
    }
    const choices = value && typeof value === "object" ? (value as Record<string, unknown>).choices : undefined
    const choice = Array.isArray(choices) ? choices[0] : undefined
    const delta = choice && typeof choice === "object" ? (choice as Record<string, unknown>).delta : undefined
    reply.push(field(delta, "reasoning_content") || field(delta, "reasoning"), "reasoning", onChunk)
    reply.push(field(delta, "content"), "content", onChunk)
  }
  reply.finish(onChunk)
  return { content: reply.result.content.trim(), reasoning: reply.result.reasoning.trim() }
}

export function parseCoderAction(text: string) {
  const fenced = text.match(/```(?:json)?\s*([\s\S]*?)```/iu)?.[1] ?? text
  const start = fenced.indexOf("{")
  const end = fenced.lastIndexOf("}")
  const candidate = start >= 0 && end > start ? fenced.slice(start, end + 1) : fenced.trim()
  const action = JSON.parse(candidate) as Record<string, unknown>
  if (typeof action.action !== "string" || !["list_files", "read_file", "write_file", "run_command", "finish"].includes(action.action)) throw new Error("Coder response must contain one supported action")
  return action
}
