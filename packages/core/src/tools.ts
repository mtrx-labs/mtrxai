import { chmod, copyFile, mkdir, readFile, stat, writeFile } from "node:fs/promises"
import path from "node:path"
import { checkedTarget, scrubbedEnvironment, sensitiveFilename } from "./safety"

const skipped = new Set([".git", "build", "dist", "node_modules", "vendor", "target"])

export async function listFiles(root: string, relative = ".") {
  const directory = await checkedTarget(root, relative === "." ? "root" : relative).catch(async (error) => {
    if (relative === ".") return root
    throw error
  })
  const result: string[] = []
  for await (const entry of new Bun.Glob("**/*").scan({ cwd: directory, absolute: true, onlyFiles: true })) {
    const name = path.basename(entry)
    if (sensitiveFilename(name) || entry.split(path.sep).some((part) => skipped.has(part))) continue
    result.push(path.relative(root, entry).split(path.sep).join("/"))
    if (result.length === 500) break
  }
  return result.sort().join("\n") || "(no project files)"
}

export async function readProjectFile(root: string, relative: string, offset = 0) {
  if (sensitiveFilename(path.basename(relative))) throw new Error(`Refusing to read sensitive file: ${relative}`)
  const target = await checkedTarget(root, relative)
  const details = await stat(target)
  if (!details.isFile()) throw new Error(`Not a regular project file: ${relative}`)
  if (details.size > 2 * 1024 * 1024) throw new Error(`File exceeds the 2 MiB read limit: ${relative}`)
  if (offset > details.size) throw new Error(`Offset is past end of file: ${relative}`)
  const content = (await readFile(target)).subarray(offset, offset + 4096)
  if (content.includes(0)) throw new Error(`Refusing to read a binary file: ${relative}`)
  const end = offset + content.length
  return `[bytes ${offset}-${end} of ${details.size}; next_offset=${end}]\n${content.toString("utf8")}`
}

export async function writeProjectFile(root: string, session: string, relative: string, content: string, turn: number) {
  if (sensitiveFilename(path.basename(relative))) throw new Error(`Refusing to write sensitive file: ${relative}`)
  if (Buffer.byteLength(content) > 2 * 1024 * 1024) throw new Error("File exceeds the 2 MiB write limit")
  const target = await checkedTarget(root, relative)
  const existing = await Bun.file(target).exists()
  if (existing && !(await stat(target)).isFile()) throw new Error("Refusing to replace a non-file")
  await mkdir(path.dirname(target), { recursive: true })
  if (existing) {
    const backup = path.join(session, "backups", `${turn}-${path.basename(relative)}`)
    await mkdir(path.dirname(backup), { recursive: true, mode: 0o700 })
    await copyFile(target, backup)
    await chmod(backup, 0o600)
  }
  await writeFile(target, content)
  return `Saved ${relative}${existing ? `; original backed up in ${path.join(session, "backups")}` : ""}`
}

export async function runCommand(root: string, command: string, timeoutSeconds = 300, signal?: AbortSignal) {
  if (!command || command.length > 4096) throw new Error("Command must be between 1 and 4096 characters")
  const timeout = Math.min(300, Math.max(1, timeoutSeconds)) * 1000
  const proc = Bun.spawn(["/bin/sh", "-c", command], { cwd: root, env: scrubbedEnvironment(), stdout: "pipe", stderr: "pipe" })
  const timer = setTimeout(() => proc.kill(), timeout)
  const abort = () => proc.kill()
  signal?.addEventListener("abort", abort, { once: true })
  try {
    const [stdout, stderr] = await Promise.all([new Response(proc.stdout).text(), new Response(proc.stderr).text()])
    const exitCode = await proc.exited
    const text = `${stdout}${stderr}`.slice(0, 16 * 1024)
    return `exit_code=${exitCode}\n${text}${stdout.length + stderr.length > 16 * 1024 ? "\n[output truncated at 16 KiB]" : ""}`
  } finally {
    clearTimeout(timer)
    signal?.removeEventListener("abort", abort)
  }
}
