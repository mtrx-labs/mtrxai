import { readFile } from "node:fs/promises"
import path from "node:path"
import { loadSession, appendTranscript, createSession, loadManifest, loadTranscript, readText, saveManifest, writeAtomic } from "./session"
import { generate, unload } from "./provider"
import { parseCoderAction } from "./parser"
import { listFiles, readProjectFile, runCommand, writeProjectFile } from "./tools"
import type { Action, Approval, Config, Message, PipelineEvent, RoleName, Stage, StreamChunk, TranscriptMessage } from "./types"

export type PipelineOptions = {
  config: Config
  projectRoot?: string
  onEvent?: (event: PipelineEvent) => void
  approve?: Approval
  signal?: AbortSignal
}

const stageRank: Record<string, number> = { request: 1, chief: 2, prompter: 3, finished: 99 }

function instruction(role: RoleName) {
  if (role === "chief") return "You are Chief of Staff. Analyze the request, intent, constraints, acceptance criteria, risks, and a concise implementation brief. Do not write code or claim completion."
  if (role === "prompter") return "You are Prompter. Convert the original request and Chief brief into a precise coding prompt with deliverables, acceptance criteria, constraints, and verification. Do not claim work is complete."
  return `You are Coder. Work iteratively in the current project. Reply with exactly one JSON object and no markdown fences. Supported actions are:
{"action":"list_files","path":"relative/directory"}
{"action":"read_file","path":"relative/file","offset":0}
{"action":"write_file","path":"relative/file","content":"complete contents"}
{"action":"run_command","command":"command","timeoutSeconds":120}
{"action":"finish","summary":"completed work and checks"}
Inspect before editing. Never access secrets. Every write and command requires user approval.`
}

function emit(options: PipelineOptions, event: PipelineEvent) {
  options.onEvent?.(event)
}

function log(options: PipelineOptions, text: string) {
  emit(options, { type: "log", text })
}

function stage(options: PipelineOptions, name: Stage, status: string, detail: string) {
  emit(options, { type: "stage", stage: name, status, detail })
}

function chunkHandler(options: PipelineOptions, name: Stage) {
  return (chunk: StreamChunk) => emit(options, { type: "chunk", stage: name, chunk })
}

async function call(options: PipelineOptions, role: RoleName, messages: Message[], directory: string, callNumber: number) {
  stage(options, role, "running", options.config[role].model || options.config[role].ggufPath || role)
  const result = await generate(options.config[role], messages, path.join(directory, role), callNumber, chunkHandler(options, role), (text) => log(options, text), options.signal)
  await writeAtomic(path.join(directory, role === "chief" ? "chief.md" : role === "prompter" ? "prompt.md" : `coder-turn-${callNumber}.md`), result.content)
  if (role !== "coder") await writeAtomic(path.join(directory, `${role}.reasoning.md`), result.reasoning)
  else await writeAtomic(path.join(directory, `coder_turn_${callNumber}_thinking.md`), result.reasoning)
  await unload(options.config[role], (text) => log(options, text))
  return result
}

function ensureNotAborted(signal?: AbortSignal) {
  if (signal?.aborted) throw new Error("Cancelled by user")
}

async function actionResult(action: Action, root: string, directory: string, turn: number, options: PipelineOptions): Promise<string> {
  if (action.action === "list_files") return `Project files:\n${await listFiles(root, action.path || ".")}`
  if (action.action === "read_file") return `Contents of ${action.path}:\n${await readProjectFile(root, action.path, action.offset ?? 0)}`
  if (action.action === "finish") return action.summary
  if (action.action === "write_file") {
    const approved = (await options.approve?.("Approve file write", `Path: ${action.path}\n\n${action.content}`)) ?? false
    if (!approved) return "User declined this file write. The file was not changed."
    return await writeProjectFile(root, directory, action.path, action.content, turn)
  }
  const timeout = Math.min(300, Math.max(1, action.timeoutSeconds ?? 300))
  const approved = (await options.approve?.("Approve shell command", `Command: ${action.command}\nTimeout: ${timeout} seconds\nWorking directory: ${root}`)) ?? false
  if (!approved) return "User declined this command. It was not executed."
  return await runCommand(root, action.command, timeout, options.signal)
}

function codingPrompt(request: string, brief: string, prompt: string, root: string) {
  return `Original request:\n${request}\n\nChief brief:\n${brief}\n\nDetailed prompt:\n${prompt}\n\nProject root: ${root}\nUse list_files before selecting files.`
}

async function saveRejected(directory: string, turn: number, text: string, reason: string) {
  const target = path.join(directory, "coder", "rejected", `turn-${turn}-${Date.now()}.md`)
  await writeAtomic(target, `# Rejected Coder reply\n\nReason: ${reason}\n\n## Reply\n\n${text}\n`)
}

export async function runPipeline(request: string | undefined, resumeId: string | undefined, options: PipelineOptions) {
  const root = path.resolve(options.projectRoot ?? process.cwd())
  const session = resumeId !== undefined || request === undefined ? await loadSession(resumeId) : await createSession(request, root)
  const savedRequest = request ?? (await readFile(path.join(session.directory, "request.md"), "utf8"))
  if (session.projectRoot !== root) throw new Error(`This session belongs to ${session.projectRoot}; resume it from that directory.`)
  const manifest = await loadManifest(session.directory)
  const completed = manifest?.completedStage ?? "request"
  let chief = await readText(path.join(session.directory, "chief.md"))
  if (stageRank[completed] < stageRank.chief || !chief) {
    stage(options, "chief", "queued", "Chief of Staff")
    const result = await call(options, "chief", [{ role: "system", content: instruction("chief") }, { role: "user", content: savedRequest }], session.directory, 1)
    chief = result.content
    await saveManifest(session.directory, { completedStage: "chief", projectRoot: root, updatedAt: new Date().toISOString() })
    stage(options, "chief", "saved", "chief.md saved")
  } else {
    stage(options, "chief", "saved", "Using saved Chief brief")
  }
  let prompt = await readText(path.join(session.directory, "prompt.md"))
  if (stageRank[completed] < stageRank.prompter || !prompt) {
    const result = await call(options, "prompter", [{ role: "system", content: instruction("prompter") }, { role: "user", content: `Original request:\n${savedRequest}\n\nChief brief:\n${chief}` }], session.directory, 1)
    prompt = result.content
    await saveManifest(session.directory, { completedStage: "prompter", projectRoot: root, updatedAt: new Date().toISOString() })
    stage(options, "prompter", "saved", "prompt.md saved")
  } else {
    stage(options, "prompter", "saved", "Using saved implementation prompt")
  }

  const transcriptPath = path.join(session.directory, "coder", "messages.jsonl")
  let transcript = await loadTranscript(transcriptPath)
  if (!transcript.length) {
    transcript = [{ role: "system", content: instruction("coder") }, { role: "user", content: codingPrompt(savedRequest, chief, prompt, root) }]
    for (const message of transcript) await appendTranscript(transcriptPath, message)
  }
  const messages: Message[] = transcript.map(({ role, content }) => ({ role, content }))
  let pending: TranscriptMessage | undefined = transcript.at(-1)?.role === "assistant" ? transcript.at(-1) : undefined
  let turn = transcript.filter((message) => message.role === "assistant").length + (pending ? 0 : 1)
  for (;;) {
    ensureNotAborted(options.signal)
    if (turn > Math.max(1, Math.min(500, options.config.maxTurns))) throw new Error(`Coder reached the ${options.config.maxTurns}-turn limit`)
    let result: { content: string; reasoning: string }
    if (pending) {
      result = { content: pending.content, reasoning: pending.thinking ?? "" }
      pending = undefined
    } else {
      result = await call(options, "coder", messages.slice(-8), session.directory, turn)
      const assistant: TranscriptMessage = { role: "assistant", content: result.content, thinking: result.reasoning, turn }
      transcript.push(assistant)
      messages.push({ role: "assistant", content: result.content })
      await appendTranscript(transcriptPath, assistant)
    }
    let action: Action
    try {
      action = parseCoderAction(result.content) as Action
    } catch (error) {
      const reason = error instanceof Error ? error.message : "invalid JSON action"
      await saveRejected(session.directory, turn, result.content, reason)
      const correction: TranscriptMessage = { role: "user", content: `Your previous reply was rejected: ${reason}. Reply with exactly one supported JSON action and no extra text.`, turn }
      await appendTranscript(transcriptPath, correction)
      messages.push({ role: "user", content: correction.content })
      if (pending) throw new Error(`Coder produced two unusable replies for turn ${turn}`)
      pending = undefined
      const retry = await call(options, "coder", messages.slice(-8), session.directory, turn)
      const assistant: TranscriptMessage = { role: "assistant", content: retry.content, thinking: retry.reasoning, turn }
      transcript.push(assistant)
      messages.push({ role: "assistant", content: retry.content })
      await appendTranscript(transcriptPath, assistant)
      try {
        action = parseCoderAction(retry.content) as Action
      } catch (retryError) {
        await saveRejected(session.directory, turn, retry.content, retryError instanceof Error ? retryError.message : "invalid JSON action")
        throw new Error(`Coder produced two unusable replies for turn ${turn}`)
      }
    }
    const resultText = await actionResult(action, root, session.directory, turn, options)
    const toolMessage: TranscriptMessage = { role: "user", content: `Tool result for ${action.action}:\n${resultText}`, turn }
    await appendTranscript(transcriptPath, toolMessage)
    messages.push({ role: "user", content: toolMessage.content })
    if (action.action === "finish") {
      await writeAtomic(path.join(session.directory, "coder.md"), `# Coder session\n\n${resultText}\n`)
      await saveManifest(session.directory, { completedStage: "finished", projectRoot: root, updatedAt: new Date().toISOString() })
      stage(options, "coder", "saved", "Coder finished")
      return session
    }
    await saveManifest(session.directory, { completedStage: `coder-turn-${turn}`, projectRoot: root, updatedAt: new Date().toISOString() })
    turn++
  }
}
