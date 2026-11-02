import { chmod, mkdir, readFile, rename, writeFile } from "node:fs/promises"
import path from "node:path"
import { stateDirectory } from "./config"
import type { TranscriptMessage } from "./types"

export type Session = { id: string; directory: string; projectRoot: string }
export type Manifest = { completedStage: string; projectRoot: string; updatedAt: string }

export async function writeAtomic(file: string, content: string, mode = 0o600) {
  await mkdir(path.dirname(file), { recursive: true, mode: 0o700 })
  const temporary = `${file}.tmp-${process.pid}`
  await writeFile(temporary, content, { mode })
  await chmod(temporary, mode)
  await rename(temporary, file)
}

export async function sessionRoot() {
  const root = path.join(stateDirectory(), "sessions")
  await mkdir(root, { recursive: true, mode: 0o700 })
  return root
}

export async function createSession(request: string, projectRoot: string): Promise<Session> {
  const root = await sessionRoot()
  const id = new Date().toISOString().replace(/[-:TZ.]/gu, "").slice(0, 15) + "-" + process.pid
  const directory = path.join(root, id)
  await mkdir(directory, { recursive: true, mode: 0o700 })
  await writeAtomic(path.join(directory, "request.md"), request)
  await writeAtomic(path.join(directory, "project-root.txt"), projectRoot + "\n")
  await saveManifest(directory, { completedStage: "request", projectRoot, updatedAt: new Date().toISOString() })
  return { id, directory, projectRoot }
}

export async function saveManifest(directory: string, manifest: Manifest) {
  await writeAtomic(path.join(directory, "manifest.json"), JSON.stringify(manifest, null, 2) + "\n")
}

export async function loadManifest(directory: string) {
  try {
    return JSON.parse(await readFile(path.join(directory, "manifest.json"), "utf8")) as Manifest
  } catch {
    return undefined
  }
}

export async function loadSession(idOrPath?: string) {
  const root = await sessionRoot()
  const directory = idOrPath ? (path.isAbsolute(idOrPath) ? idOrPath : path.join(root, idOrPath)) : await latestUnfinished(root)
  if (!directory) throw new Error("No unfinished mtrxAI session was found")
  const projectRoot = (await readFile(path.join(directory, "project-root.txt"), "utf8")).trim()
  const current = path.resolve(process.cwd())
  if (projectRoot !== current) throw new Error(`This session belongs to ${projectRoot}; resume it from that directory.`)
  return { id: path.basename(directory), directory, projectRoot }
}

async function latestUnfinished(root: string) {
  const entries = await Array.fromAsync(new Bun.Glob("*/request.md").scan({ cwd: root, absolute: true }))
  const candidates: Array<{ directory: string; mtime: number }> = []
  for (const request of entries) {
    const directory = path.dirname(request)
    const manifest = await loadManifest(directory)
    if (manifest?.completedStage === "finished") continue
    candidates.push({ directory, mtime: (await Bun.file(request).stat()).mtimeMs })
  }
  return candidates.sort((a, b) => b.mtime - a.mtime)[0]?.directory
}

export async function listSessions() {
  const root = await sessionRoot()
  const entries = await Array.fromAsync(new Bun.Glob("*/manifest.json").scan({ cwd: root, absolute: true }))
  return Promise.all(entries.map(async (manifest) => ({ id: path.basename(path.dirname(manifest)), directory: path.dirname(manifest), value: await loadManifest(path.dirname(manifest)) })))
}

export async function readText(file: string) {
  try {
    return await readFile(file, "utf8")
  } catch {
    return ""
  }
}

export async function appendTranscript(file: string, message: TranscriptMessage) {
  await mkdir(path.dirname(file), { recursive: true, mode: 0o700 })
  const handle = await Bun.file(file).writer({ highWaterMark: 1 })
  await handle.write(JSON.stringify(message) + "\n")
  await handle.end()
  await chmod(file, 0o600)
}

export async function loadTranscript(file: string): Promise<TranscriptMessage[]> {
  const text = await readText(file)
  return text.split("\n").filter(Boolean).flatMap((line) => {
    try {
      const value = JSON.parse(line) as TranscriptMessage
      return value.role && typeof value.content === "string" ? [value] : []
    } catch {
      return []
    }
  })
}
