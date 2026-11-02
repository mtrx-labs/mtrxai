import { chmod, mkdir, readFile, rename, writeFile } from "node:fs/promises"
import path from "node:path"
import type { Config, RoleConfig, RoleName } from "./types"

const roles: RoleName[] = ["chief", "prompter", "coder"]

function homePath(...parts: string[]) {
  const home = process.env.HOME
  if (!home) throw new Error("HOME is not set")
  return path.join(home, ...parts)
}

export function configDirectory() {
  return process.env.XDG_CONFIG_HOME ? path.join(process.env.XDG_CONFIG_HOME, "mtrxai") : homePath(".config", "mtrxai")
}

export function configPath() {
  return path.join(configDirectory(), "config.json")
}

export function credentialsPath() {
  return path.join(configDirectory(), "credentials.json")
}

export function stateDirectory() {
  return process.env.XDG_STATE_HOME ? path.join(process.env.XDG_STATE_HOME, "mtrxai") : homePath(".local", "state", "mtrxai")
}

function role(name: RoleName, defaults: Partial<RoleConfig>): RoleConfig {
  return {
    name,
    provider: defaults.provider ?? "ollama",
    model: defaults.model ?? "",
    contextSize: defaults.contextSize ?? (name === "coder" ? 2048 : 4096),
    maxTokens: defaults.maxTokens ?? 4096,
    endpoint: defaults.endpoint ?? (defaults.provider === "ollama" || !defaults.provider ? "http://127.0.0.1:11434" : ""),
    apiKeyEnv: defaults.apiKeyEnv ?? "",
    apiKey: defaults.apiKey,
    ggufPath: defaults.ggufPath,
    timeoutSeconds: defaults.timeoutSeconds ?? 3600,
    ggufLoadTimeoutSeconds: defaults.ggufLoadTimeoutSeconds ?? 150,
  }
}

export function defaultConfig(): Config {
  return {
    chief: role("chief", { model: "deepseek-r1:8b" }),
    prompter: role("prompter", { model: "qwen3:4b-instruct" }),
    coder: role("coder", { model: "qwen3:4b-instruct" }),
    maxTurns: 40,
    ggufLoadTimeoutSeconds: 150,
  }
}

export async function loadCredentials() {
  try {
    return JSON.parse(await readFile(credentialsPath(), "utf8")) as Record<string, string>
  } catch {
    return {}
  }
}

export async function apiKeyFor(slot: RoleConfig, credentials?: Record<string, string>) {
  credentials ??= await loadCredentials()
  if (slot.apiKey) return slot.apiKey
  if (slot.apiKeyEnv && process.env[slot.apiKeyEnv]) return process.env[slot.apiKeyEnv]
  return credentials[slot.name] ?? ""
}

export async function loadConfig(file = configPath()): Promise<Config> {
  const defaults = defaultConfig()
  let raw: Record<string, unknown> = {}
  try {
    raw = JSON.parse(await readFile(file, "utf8")) as Record<string, unknown>
  } catch {
    return defaults
  }
  const loaded = (name: RoleName) => {
    const value = raw[name]
    return role(name, typeof value === "object" && value !== null ? (value as Partial<RoleConfig>) : {})
  }
  return {
    chief: { ...defaults.chief, ...loaded("chief"), name: "chief" },
    prompter: { ...defaults.prompter, ...loaded("prompter"), name: "prompter" },
    coder: { ...defaults.coder, ...loaded("coder"), name: "coder" },
    maxTurns: typeof raw.maxTurns === "number" ? raw.maxTurns : defaults.maxTurns,
    ggufLoadTimeoutSeconds: typeof raw.ggufLoadTimeoutSeconds === "number" ? raw.ggufLoadTimeoutSeconds : defaults.ggufLoadTimeoutSeconds,
  }
}

export async function saveConfig(config: Config, file = configPath()) {
  await mkdir(path.dirname(file), { recursive: true, mode: 0o700 })
  const clean = structuredClone(config)
  for (const name of roles) delete clean[name].apiKey
  const temporary = `${file}.tmp-${process.pid}`
  await writeFile(temporary, JSON.stringify(clean, null, 2) + "\n", { mode: 0o600 })
  await chmod(temporary, 0o600)
  await rename(temporary, file)
}

export async function saveCredential(roleName: RoleName, value: string, file = credentialsPath()) {
  const credentials = await loadCredentials()
  if (value) credentials[roleName] = value
  else delete credentials[roleName]
  await mkdir(path.dirname(file), { recursive: true, mode: 0o700 })
  const temporary = `${file}.tmp-${process.pid}`
  await writeFile(temporary, JSON.stringify(credentials, null, 2) + "\n", { mode: 0o600 })
  await chmod(temporary, 0o600)
  await rename(temporary, file)
}

export function roleConfig(config: Config, name: RoleName) {
  return config[name]
}
