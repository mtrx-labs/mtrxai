#!/usr/bin/env bun
import { createInterface } from "node:readline/promises"
import { stdin as input, stdout as output } from "node:process"
import { loadConfig, saveConfig, saveCredential, configPath, type ProviderKind, type RoleName } from "@mtrxai/core"
import { runTui } from "@mtrxai/tui"

const roles: RoleName[] = ["chief", "prompter", "coder"]
const providers: ProviderKind[] = ["ollama", "openai", "anthropic", "custom", "gguf"]

async function secret(prompt: string) {
  output.write(prompt)
  if (!input.isTTY || !input.setRawMode) return (await createInterface({ input, output }).question(""))
  return await new Promise<string>((resolve) => {
    let value = ""
    const onData = (chunk: Buffer) => {
      for (const byte of chunk) {
        if (byte === 13 || byte === 10) {
          input.setRawMode?.(false)
          input.pause()
          input.off("data", onData)
          output.write("\n")
          resolve(value)
        } else if (byte === 127) {
          value = value.slice(0, -1)
        } else if (byte >= 32) value += String.fromCharCode(byte)
      }
    }
    input.setRawMode?.(true)
    input.resume()
    input.on("data", onData)
  })
}

async function init() {
  const config = await loadConfig()
  const prompt = createInterface({ input, output })
  try {
    for (const name of roles) {
      const slot = config[name]
      const provider = await prompt.question(`${name} provider (${providers.join(", ")}) [${slot.provider}]: `)
      if (providers.includes(provider as ProviderKind)) slot.provider = provider as ProviderKind
      if (slot.provider === "gguf") {
        slot.ggufPath = await prompt.question(`GGUF path [${slot.ggufPath ?? ""}]: `) || slot.ggufPath
      } else {
        slot.endpoint = await prompt.question(`Endpoint [${slot.endpoint}]: `) || slot.endpoint
        slot.model = await prompt.question(`Model [${slot.model}]: `) || slot.model
        slot.apiKeyEnv = await prompt.question(`API key environment variable [${slot.apiKeyEnv}]: `) || slot.apiKeyEnv
        const key = await secret(`API key (leave empty to keep credentials file unchanged): `)
        if (key) await saveCredential(name, key)
      }
      slot.contextSize = Number(await prompt.question(`Context tokens [${slot.contextSize}]: `) || slot.contextSize)
      slot.maxTokens = Number(await prompt.question(`Max output tokens [${slot.maxTokens}]: `) || slot.maxTokens)
    }
  } finally {
    prompt.close()
  }
  await saveConfig(config)
  output.write(`Saved ${configPath()}\n`)
}

async function main() {
  const args = process.argv.slice(2)
  if (args.includes("--help") || args.includes("-h")) {
    output.write("mtrxai [request...]\n\n  --init          configure the three roles\n  --resume [id]  resume a saved session\n  --help          show this help\n")
    return
  }
  if (args.includes("--init")) {
    await init()
    return
  }
  const resumeIndex = args.indexOf("--resume")
  const resumeId = resumeIndex >= 0 && args[resumeIndex + 1] && !args[resumeIndex + 1].startsWith("-") ? args[resumeIndex + 1] : resumeIndex >= 0 ? undefined : undefined
  const request = args.filter((arg, index) => !arg.startsWith("-") && index !== resumeIndex && index !== resumeIndex + 1).join(" ")
  if (!input.isTTY || !output.isTTY) {
    if (!request && resumeIndex < 0) throw new Error("A terminal is required for mtrxAI")
    const { runPipeline } = await import("@mtrxai/core")
    const config = await loadConfig()
    await runPipeline(request || undefined, resumeId, { config, onEvent: (event) => event.type === "log" && output.write(event.text + "\n") })
    return
  }
  await runTui({ config: await loadConfig(), resumeId })
}

main().catch((error) => {
  output.write(`mtrxAI: ${error instanceof Error ? error.message : String(error)}\n`)
  process.exitCode = 1
})
