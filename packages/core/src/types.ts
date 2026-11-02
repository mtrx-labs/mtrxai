export type RoleName = "chief" | "prompter" | "coder"
export type ProviderKind = "ollama" | "openai" | "anthropic" | "custom" | "gguf"

export type RoleConfig = {
  name: RoleName
  provider: ProviderKind
  model: string
  contextSize: number
  maxTokens: number
  endpoint: string
  apiKeyEnv: string
  apiKey?: string
  ggufPath?: string
  timeoutSeconds: number
  ggufLoadTimeoutSeconds: number
}

export type Config = {
  chief: RoleConfig
  prompter: RoleConfig
  coder: RoleConfig
  maxTurns: number
  ggufLoadTimeoutSeconds: number
}

export type Message = { role: "system" | "user" | "assistant"; content: string }

export type StreamChunk = { kind: "content" | "reasoning"; text: string }

export type Stage = "chief" | "prompter" | "coder"

export type PipelineEvent =
  | { type: "stage"; stage: Stage; status: string; detail: string }
  | { type: "chunk"; stage: Stage; chunk: StreamChunk }
  | { type: "log"; text: string }
  | { type: "approval"; title: string; body: string; resolve: (approved: boolean) => void }

export type Approval = (title: string, body: string) => Promise<boolean>

export type Action =
  | { action: "list_files"; path?: string }
  | { action: "read_file"; path: string; offset?: number }
  | { action: "write_file"; path: string; content: string }
  | { action: "run_command"; command: string; timeoutSeconds?: number }
  | { action: "finish"; summary: string }

export type TranscriptMessage = Message & { thinking?: string; turn?: number }
