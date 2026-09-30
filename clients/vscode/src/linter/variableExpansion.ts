// SPDX-License-Identifier: MIT
import * as os from 'os'
import * as path from 'path'

/// What the `${...}` variables of one tool run resolve against. Anything the caller
/// cannot say is left undefined, and the variables that need it stay unexpanded.
export interface ExpansionContext {
  /// The workspace folder the run belongs to, which is also where the tool runs
  workspaceFolder?: string
  /// The file the tool is run over
  file?: string
  /// The working directory of the run
  cwd?: string
  env?: Record<string, string | undefined>
  /// Look up a VS Code setting, undefined when it has no scalar value
  config?: (id: string) => string | undefined
}

export interface UnresolvedVariable {
  /// The variable as it was written, e.g. `${env:INC}`
  name: string
  /// Why it has no value, for the caller to put in a log line
  reason: string
}

export interface ExpansionResult {
  /// The text with every variable that could be resolved replaced
  text: string
  /// The variables that were left as they were, in the order they appear
  unresolved: UnresolvedVariable[]
}

/// A complete `${...}` with no nesting. What is inside is the variable, which is
/// either one of the names below or `env:NAME` / `config:ID`
const VARIABLE = /\$\{([^{}]*)\}/g

const NO_WORKSPACE_FOLDER = 'no workspace folder is open'
const NO_FILE = 'no file is being linted'
const OUTSIDE_WORKSPACE_FOLDER = 'the file is not inside the workspace folder'
const NOT_SUPPORTED = 'it is not a variable this extension resolves'

/// Either the value to put in the text, or why there is none
type Resolution = { value: string } | { reason: string }

/**
 * Replace the `${...}` variables in one command line argument. Everything that
 * cannot be resolved is left exactly as it was written and reported in
 * `unresolved`, so that a mistyped variable reaches the tool instead of turning
 * into something else. A variable whose value happens to be empty is resolved, and
 * is not reported.
 */
export function expandVariables(text: string, ctx: ExpansionContext): ExpansionResult {
  const unresolved = new Map<string, UnresolvedVariable>()

  const expanded = text.replace(VARIABLE, (match: string, key: string) => {
    const resolution = resolve(key, ctx)
    if ('value' in resolution) {
      return resolution.value
    }
    if (!unresolved.has(match)) {
      unresolved.set(match, { name: match, reason: resolution.reason })
    }
    return match
  })

  return { text: expanded, unresolved: [...unresolved.values()] }
}

function resolve(key: string, ctx: ExpansionContext): Resolution {
  if (key.startsWith('env:')) {
    const name = key.slice('env:'.length)
    const value = ctx.env?.[name]
    // An environment is an ordinary object, so a name it only inherits from its
    // prototype is not a variable the user set
    return typeof value === 'string'
      ? { value }
      : { reason: `the environment variable ${name} is not set` }
  }

  if (key.startsWith('config:')) {
    const id = key.slice('config:'.length)
    const value = ctx.config?.(id)
    return value === undefined ? { reason: `the setting ${id} has no value` } : { value }
  }

  switch (key) {
    // The run is anchored on the folder the file is in, so the two are the same
    case 'workspaceFolder':
    case 'fileWorkspaceFolder':
      return withValue(ctx.workspaceFolder, NO_WORKSPACE_FOLDER)
    case 'workspaceFolderBasename':
      return withValue(ctx.workspaceFolder, NO_WORKSPACE_FOLDER, (folder) => path.basename(folder))
    case 'file':
      return withValue(ctx.file, NO_FILE)
    case 'fileDirname':
      return withValue(ctx.file, NO_FILE, (file) => path.dirname(file))
    case 'fileBasename':
      return withValue(ctx.file, NO_FILE, (file) => path.basename(file))
    case 'fileBasenameNoExtension':
      return withValue(ctx.file, NO_FILE, basenameWithoutExtension)
    case 'fileExtname':
      return withValue(ctx.file, NO_FILE, (file) => path.extname(file))
    case 'relativeFile':
      return relativeToWorkspace(ctx, (relative) => toPosix(relative))
    case 'relativeFileDirname':
      return relativeToWorkspace(ctx, (relative) => {
        const dir = path.dirname(relative)
        return dir === '.' ? '' : toPosix(dir)
      })
    case 'cwd':
      return withValue(ctx.cwd, 'the working directory is unknown')
    case 'pathSeparator':
    case '/':
      return { value: path.sep }
    case 'userHome':
      return { value: os.homedir() }
    default:
      return { reason: NOT_SUPPORTED }
  }
}

function withValue(
  source: string | undefined,
  reason: string,
  derive: (source: string) => string = (source) => source
): Resolution {
  return source === undefined ? { reason } : { value: derive(source) }
}

/// A path relative to the workspace folder, as VS Code writes it: with `/`, and
/// refusing a file that is not under the folder at all
function relativeToWorkspace(
  ctx: ExpansionContext,
  derive: (relative: string) => string
): Resolution {
  if (ctx.workspaceFolder === undefined) {
    return { reason: NO_WORKSPACE_FOLDER }
  }
  if (ctx.file === undefined) {
    return { reason: NO_FILE }
  }

  const relative = path.relative(ctx.workspaceFolder, ctx.file)
  // `..` and `..<separator>` are the escapes out of the folder; a name that merely
  // starts with two dots is a file inside it
  if (path.isAbsolute(relative) || relative === '..' || relative.startsWith(`..${path.sep}`)) {
    return { reason: OUTSIDE_WORKSPACE_FOLDER }
  }
  return { value: derive(relative) }
}

function basenameWithoutExtension(file: string): string {
  const extension = path.extname(file)
  const base = path.basename(file)
  return extension ? base.slice(0, -extension.length) : base
}

function toPosix(file: string): string {
  return file.replace(/\\/g, '/')
}
