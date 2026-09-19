/**
 * Gives local VS Code extension builds a version that is distinct from the
 * upstream marketplace version, without ever committing that version.
 *
 * `package.json` is kept at the upstream version so upstream merges never
 * conflict on it. The local-only version is computed at build time as
 *
 *     <base>-local.<yyyymmddHHMM>.<shorthash>[.dirty]
 *
 * where `<base>` defaults to the version already in `package.json` and can be
 * overridden with `--base` or `SLANG_LOCAL_BASE_VERSION`. `package.json` is
 * only rewritten for the duration of the spawned packaging command and is
 * restored afterwards, including when the command fails or is interrupted.
 *
 * Usage (run from clients/vscode):
 *   pnpm version:local
 *   pnpm exec ts-node scripts/localVersion.ts --dry-run -- pnpm vsce package --no-dependencies
 *   pnpm exec ts-node scripts/localVersion.ts -- pnpm vsce package --no-dependencies
 */

import { spawnSync } from 'child_process'
import { readFileSync, writeFileSync } from 'fs'
import { join } from 'path'

// Resolves to clients/vscode for both `scripts/` (ts-node) and `out/scripts/` (compiled).
export const EXTENSION_ROOT = join(__dirname, '..')
export const PACKAGE_JSON = join(EXTENSION_ROOT, 'package.json')
const SEMVER_PATTERN = /^\d+\.\d+\.\d+$/
/** The version line is the first "version" key, which is also the top-level one. */
const VERSION_LINE = /^(\s*"version"\s*:\s*)"[^"]*"/m

export interface Options {
  base: string
  dryRun: boolean
  show: boolean
  command: string[]
}

/** Reads `--flag value` style options and everything after `--` as the command. */
export function parseArgs(argv: string[], env: NodeJS.ProcessEnv = process.env): Options {
  const separator = argv.indexOf('--')
  const flags = separator === -1 ? argv : argv.slice(0, separator)
  const command = separator === -1 ? [] : argv.slice(separator + 1)

  let base = env.SLANG_LOCAL_BASE_VERSION ?? ''
  let dryRun = false
  let show = false

  for (let i = 0; i < flags.length; i++) {
    const flag = flags[i]
    if (flag === '--base') {
      base = flags[++i] ?? ''
    } else if (flag === '--dry-run') {
      dryRun = true
    } else if (flag === '--show') {
      show = true
    } else {
      throw new Error(`Unknown option: ${flag}`)
    }
  }

  return { base, dryRun, show, command }
}

/** Reads and validates the version field out of package.json text. */
export function versionFromText(text: string): string {
  const pkg = JSON.parse(text) as { version?: string }
  if (!pkg.version) {
    throw new Error('No "version" field found in package.json')
  }
  return pkg.version
}

/**
 * Replaces only the version line, leaving formatting and field order intact.
 * Returns the new text without writing it, so callers can pair it with a
 * restore step and tests can stay off the real package.json.
 */
export function injectVersion(text: string, version: string): string {
  const updated = text.replace(VERSION_LINE, `$1"${version}"`)
  if (updated === text) {
    throw new Error('Could not rewrite the "version" field in package.json')
  }
  return updated
}

/** Rejects bases that could collide with a version upstream may publish. */
export function validateBase(base: string): void {
  if (!SEMVER_PATTERN.test(base)) {
    throw new Error(
      `Base version "${base}" is not MAJOR.MINOR.PATCH. Upstream publishes patch ` +
        `releases from the base (0.3.1 -> 0.3.2 -> ...), so a base that is not ` +
        `MAJOR.MINOR.PATCH cannot be told apart from a published version.`
    )
  }
}

export interface BuildVersionDeps {
  now?: Date
  /** Returns the short commit hash, or '' when unavailable. */
  gitHash?: () => string
  /** Returns true when the working tree has uncommitted changes. */
  gitDirty?: () => boolean
}

/** Timestamp keeps repeated builds ordered; the hash ties a build to a commit. */
export function buildLocalVersion(base: string, deps: BuildVersionDeps = {}): string {
  const now = deps.now ?? new Date()
  const stamp = [
    now.getFullYear(),
    String(now.getMonth() + 1).padStart(2, '0'),
    String(now.getDate()).padStart(2, '0'),
    String(now.getHours()).padStart(2, '0'),
    String(now.getMinutes()).padStart(2, '0'),
  ].join('')

  const id = [stamp]
  const hash = (deps.gitHash ?? defaultGitHash)()
  if (hash) {
    id.push(hash)
  }
  if ((deps.gitDirty ?? defaultGitDirty)()) {
    id.push('dirty')
  }

  return `${base}-local.${id.join('.')}`
}

function gitOutput(args: string[]): string {
  const result = spawnSync('git', args, { cwd: EXTENSION_ROOT, encoding: 'utf8' })
  return result.status === 0 ? result.stdout.trim() : ''
}

function defaultGitHash(): string {
  return gitOutput(['rev-parse', '--short', 'HEAD'])
}

function defaultGitDirty(): boolean {
  return gitOutput(['status', '--porcelain']) !== ''
}

/**
 * Quotes one argument for the platform shell. on Windows the quoting is also
 * what keeps arguments containing spaces intact through `cmd.exe`.
 */
export function quoteForShell(argument: string): string {
  if (process.platform === 'win32') {
    return `"${argument.replace(/"/g, '\\"')}"`
  }
  return `'${argument.replace(/'/g, `'\\''`)}'`
}

/**
 * Runs `command` with the local version injected into package.json, restoring
 * the previous contents afterwards even when the command fails. Returns the
 * command's exit status.
 */
export function runWithInjectedVersion(
  command: string[],
  local: string,
  packageJsonPath: string = PACKAGE_JSON
): number {
  if (command.length === 0) {
    throw new Error('No command given. Pass one after "--", e.g. -- pnpm vsce package')
  }

  const original = readFileSync(packageJsonPath, 'utf8')
  writeFileSync(packageJsonPath, injectVersion(original, local))

  let status: number | null
  try {
    // A pre-quoted command string (rather than shell + args) keeps `.cmd` shims
    // resolvable on Windows without tripping Node's DEP0190 warning.
    const line = command.map(quoteForShell).join(' ')
    const result = spawnSync(line, { cwd: EXTENSION_ROOT, stdio: 'inherit', shell: true })
    status = result.status
  } finally {
    writeFileSync(packageJsonPath, original)
  }

  return status ?? 1
}

export function main(argv: string[] = process.argv.slice(2)): void {
  const options = parseArgs(argv)
  const base = options.base || versionFromText(readFileSync(PACKAGE_JSON, 'utf8'))
  validateBase(base)
  const local = buildLocalVersion(base)

  if (options.show) {
    console.log(`upstream/committed: ${versionFromText(readFileSync(PACKAGE_JSON, 'utf8'))}`)
    console.log(`local build:        ${local}`)
    return
  }

  console.log(`[localVersion] ${options.dryRun ? 'would package as' : 'packaging as'} ${local}`)
  if (options.dryRun) {
    return
  }

  process.exit(runWithInjectedVersion(options.command, local))
}

// Only run when invoked as a script; importing this module is side-effect free.
if (require.main === module) {
  try {
    main()
  } catch (error) {
    console.error(`[localVersion] ${error instanceof Error ? error.message : String(error)}`)
    process.exit(1)
  }
}
