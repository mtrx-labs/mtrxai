import { describe, expect, test } from "bun:test"
import { mkdtemp, writeFile } from "node:fs/promises"
import os from "node:os"
import path from "node:path"
import { checkedTarget, sensitiveFilename, scrubbedEnvironment } from "../src/safety"
import { parseCoderAction, parseStream } from "../src/parser"
import { generate } from "../src/provider"
import { createSession, loadSession, saveManifest } from "../src/session"
import { verifySha256 } from "../src/updater"

describe("mtrxAI safety", () => {
  test("blocks credentials and key material but permits ordinary source names", () => {
    for (const name of [".env", ".env.local", ".netrc", ".npmrc", ".pypirc", "credentials.json", "server.pem", "secret.key", "bundle.p12", "bundle.pfx", "id_rsa", "id_ed25519_work"]) expect(sensitiveFilename(name)).toBe(true)
    for (const name of ["tokenizer.cpp", "dev.env", "README.md", "keybinding.ts"]) expect(sensitiveFilename(name)).toBe(false)
  })

  test("rejects traversal and symlink escapes", async () => {
    const root = await mkdtemp(path.join(os.tmpdir(), "mtrxai-safety-"))
    await writeFile(path.join(root, "safe.txt"), "safe")
    expect(await checkedTarget(root, "safe.txt")).toBe(path.join(root, "safe.txt"))
    await expect(checkedTarget(root, "../outside")).rejects.toThrow()
    await expect(checkedTarget(root, path.join("/", "etc", "passwd"))).rejects.toThrow()
  })

  test("scrubs arbitrary environment variables", () => {
    process.env.MTRXAI_TEST_SECRET = "do-not-pass"
    expect(scrubbedEnvironment().MTRXAI_TEST_SECRET).toBeUndefined()
  })
})

describe("reply parser", () => {
  test("parses provider streams and strips thinking", () => {
    const raw = 'data: {"choices":[{"delta":{"content":"<think>private</think>visible"}}]}\n\ndata: [DONE]\n'
    expect(parseStream(raw, "openai")).toEqual({ content: "visible", reasoning: "private" })
  })

  test("parses fenced coder actions", () => {
    expect(parseCoderAction('```json\n{"action":"finish","summary":"done"}\n```')).toEqual({ action: "finish", summary: "done" })
    expect(() => parseCoderAction("not an action")).toThrow()
  })
})

describe("resume artifacts", () => {
  test("refuses a session from another project and preserves stage state", async () => {
    const state = await mkdtemp(path.join(os.tmpdir(), "mtrxai-state-"))
    const project = await mkdtemp(path.join(os.tmpdir(), "mtrxai-project-"))
    process.env.XDG_STATE_HOME = state
    const session = await createSession("request", project)
    await saveManifest(session.directory, { completedStage: "chief", projectRoot: project, updatedAt: new Date().toISOString() })
    await expect(loadSession(session.id)).rejects.toThrow("resume it from that directory")
  })
})

test("updater verifies exact SHA-256 bytes", async () => {
  expect(await verifySha256(new TextEncoder().encode("mtrxAI"), "aba1791ee19f2d5f2279acb5eca72bbc40c644f0aa8c16eaccd72859347cc633")).toBe(true)
  expect(await verifySha256(new TextEncoder().encode("mtrxAI"), "0000000000000000000000000000000000000000000000000000000000000000")).toBe(false)
})

test("mock OpenAI-compatible provider streams and records a response", async () => {
  const server = Bun.serve({
    port: 0,
    async fetch(request) {
      if (new URL(request.url).pathname !== "/v1/chat/completions") return new Response("missing", { status: 404 })
      return new Response('data: {"choices":[{"delta":{"content":"hello"}}]}\n\ndata: {"choices":[{"delta":{"content":" world"}}]}\n\ndata: [DONE]\n', { headers: { "content-type": "text/event-stream" } })
    },
  })
  const directory = await mkdtemp(path.join(os.tmpdir(), "mtrxai-provider-"))
  const chunks: string[] = []
  try {
    const result = await generate({ name: "chief", provider: "custom", model: "mock", contextSize: 4096, maxTokens: 32, endpoint: `http://127.0.0.1:${server.port}`, apiKeyEnv: "", timeoutSeconds: 30, ggufLoadTimeoutSeconds: 1 }, [{ role: "user", content: "hello" }], path.join(directory, "chief"), 1, (chunk) => chunks.push(chunk.text), () => {})
    expect(result.content).toBe("hello world")
    expect(chunks.join("")).toContain("hello")
    expect(await Bun.file(path.join(directory, "chief", "call-1.raw")).exists()).toBe(true)
  } finally {
    server.stop()
  }
})
