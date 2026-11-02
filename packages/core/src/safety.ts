import { lstat, realpath } from "node:fs/promises"
import path from "node:path"

const exact = new Set([".env", ".netrc", ".npmrc", ".pypirc", "credentials.json"])
const suffixes = [".pem", ".key", ".p12", ".pfx"]

export function sensitiveFilename(name: string) {
  const lower = path.basename(name).toLowerCase()
  return exact.has(lower) || lower.startsWith(".env.") || suffixes.some((suffix) => lower.endsWith(suffix)) || lower.startsWith("id_rsa") || lower.startsWith("id_ed25519")
}

export async function checkedTarget(root: string, relative: string) {
  if (!relative || path.isAbsolute(relative) || relative.split(/[\\/]/u).includes("..")) throw new Error(`Unsafe project path: ${relative}`)
  const resolvedRoot = await realpath(root)
  const target = path.resolve(resolvedRoot, relative)
  if (target !== resolvedRoot && !target.startsWith(resolvedRoot + path.sep)) throw new Error(`Path escapes project: ${relative}`)
  const existing = await realpath(target).catch(() => undefined)
  if (existing && existing !== resolvedRoot && !existing.startsWith(resolvedRoot + path.sep)) throw new Error(`Symlink escapes project: ${relative}`)
  const parent = await realpath(path.dirname(target)).catch(() => undefined)
  if (parent && parent !== resolvedRoot && !parent.startsWith(resolvedRoot + path.sep)) throw new Error(`Parent escapes project: ${relative}`)
  return target
}

export async function assertRegularTarget(target: string) {
  const stat = await lstat(target)
  if (!stat.isFile()) throw new Error("Target is not a regular file")
}

export const commandEnvironmentNames = ["HOME", "PATH", "LANG", "LC_ALL", "TERM", "TMPDIR", "CC", "CXX", "CFLAGS", "CXXFLAGS", "LDFLAGS", "PKG_CONFIG_PATH", "CMAKE_PREFIX_PATH", "VIRTUAL_ENV", "CONDA_PREFIX"]

export function scrubbedEnvironment() {
  const environment: Record<string, string> = {}
  for (const name of commandEnvironmentNames) {
    const value = process.env[name]
    if (value !== undefined) environment[name] = value
  }
  environment.PATH ||= "/usr/local/bin:/usr/bin:/bin"
  return environment
}
