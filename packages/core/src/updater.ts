import { createHash } from "node:crypto"
import { chmod, mkdir, rename, unlink } from "node:fs/promises"
import path from "node:path"

export type UpdateCandidate = { url: string; sha256: string; version: string }

function validateDownload(url: string) {
  const parsed = new URL(url)
  if (parsed.protocol !== "https:" || parsed.username || parsed.password) throw new Error("Updates require an HTTPS URL without embedded credentials")
  return parsed
}

export async function verifySha256(bytes: Uint8Array, expected: string) {
  const digest = createHash("sha256").update(bytes).digest("hex")
  return digest.toLowerCase() === expected.trim().toLowerCase()
}

export async function installUpdate(candidate: UpdateCandidate, executable: string, approve: (candidate: UpdateCandidate) => Promise<boolean>) {
  validateDownload(candidate.url)
  if (!/^[a-f0-9]{64}$/iu.test(candidate.sha256)) throw new Error("Update metadata has an invalid SHA-256 digest")
  if (!(await approve(candidate))) return false
  const response = await fetch(candidate.url, { redirect: "manual", signal: AbortSignal.timeout(300_000) })
  if (response.status >= 300 && response.status < 400) throw new Error("Update redirects are not followed")
  if (!response.ok) throw new Error(`Update download failed with HTTP ${response.status}`)
  const bytes = new Uint8Array(await response.arrayBuffer())
  if (!(await verifySha256(bytes, candidate.sha256))) throw new Error("Update SHA-256 verification failed")
  const temporary = `${executable}.download-${process.pid}`
  await Bun.write(temporary, bytes)
  await chmod(temporary, 0o755)
  try {
    await rename(temporary, executable)
  } catch {
    await unlink(temporary).catch(() => {})
    const fallback = path.join(process.env.XDG_BIN_HOME ?? path.join(process.env.HOME ?? ".", ".local", "bin"), "mtrxai")
    await mkdir(path.dirname(fallback), { recursive: true, mode: 0o700 })
    await Bun.write(fallback, bytes)
    await chmod(fallback, 0o755)
  }
  return true
}
