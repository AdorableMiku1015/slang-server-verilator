// SPDX-License-Identifier: MIT
import * as child_process from 'child_process'
import * as vscode from 'vscode'
import { parseArgsStringToArgv } from 'string-argv'
import { ConfigObject, ExtensionComponent } from '../lib/libconfig'
import { PathConfigObject } from '../lib/pathConfig'
import {
  FindingGroup,
  groupFindingsByFile,
  LinterFinding,
  LinterSeverity,
  LocatedFinding,
} from './lintOutput'
import { ProcessError, runFailure } from './runFailure'
import { ExpansionContext, expandVariables } from './variableExpansion'

/// Linting a large design can take a while, but a run that outlives this is stuck
/// rather than busy
const LINT_TIMEOUT_MS = 30_000
/// A design with a lot of findings prints a lot, and hitting the buffer cap would
/// lose the run along with its findings
const MAX_OUTPUT_BYTES = 8 * 1024 * 1024

interface RunResult {
  /// Where verilator reports everything, diagnostics included
  stderr: string
  /// What went wrong at the process level, or null when it exited on its own. What
  /// that means for the findings is decided by `runFailure`, once they are parsed.
  error: ProcessError | null
}

interface ReportedFile {
  uri: vscode.Uri
  diagnostics: vscode.Diagnostic[]
}

/**
 * An external linter: a command line tool that is run over a file, whose output is
 * shown as diagnostics on the files it talks about. Subclasses say what the tool is
 * called, which arguments it gets for a file, and how to read what it prints.
 */
export abstract class ExternalLinter extends ExtensionComponent {
  readonly enabled: ConfigObject<boolean>
  readonly path: PathConfigObject
  readonly args: ConfigObject<string[]>
  readonly diagnostics: vscode.DiagnosticCollection

  protected readonly toolName: string

  private context: vscode.ExtensionContext | undefined
  private resolvedPath: string | undefined
  private toolLookupFailed: boolean = false
  /// Variables the configured arguments could not resolve, so that a run that keeps
  /// asking for one does not keep saying so
  private warnedUnresolved = new Set<string>()
  /// Files this linter reported on in its last run, so that the ones that are gone
  /// from the current run can have their diagnostics dropped
  private reportedFiles = new Map<string, ReportedFile>()
  private notified = new Set<string>()

  constructor(toolName: string) {
    super()
    this.toolName = toolName
    this.enabled = new ConfigObject({
      default: false,
      description: `Enable ${toolName} lint`,
    })
    this.path = new PathConfigObject(
      { description: `Path to the ${toolName} executable, empty to find it on the PATH` },
      { windows: toolName, linux: toolName, mac: toolName }
    )
    this.args = new ConfigObject({
      default: [],
      description:
        `Additional arguments to pass to ${toolName}. Variables such as ` +
        '`${workspaceFolder}`, `${fileDirname}` and `${env:VAR}` are expanded; one that cannot ' +
        'be resolved is passed on as written.',
    })
    this.diagnostics = vscode.languages.createDiagnosticCollection(toolName)
  }

  async activate(context: vscode.ExtensionContext): Promise<void> {
    this.context = context
    context.subscriptions.push(this.diagnostics)
  }

  /// Run the tool over one file, replacing the diagnostics the previous run reported.
  /// `workspaceFolder` is the folder the run belongs to, which is undefined when the
  /// target is not in any of them.
  async lint(target: vscode.Uri, cwd: string, workspaceFolder?: string): Promise<void> {
    if (!this.enabled.getValue()) {
      return
    }

    try {
      const tool = await this.toolPath()
      if (!tool) {
        return
      }

      const context = this.expansionContext(target, cwd, workspaceFolder)
      const args = [...this.toolArgs(target), ...this.configuredArgs(context), target.fsPath]
      this.logger.info(`${tool} ${args.join(' ')}`)

      const output = await this.run(tool, args, cwd)
      this.logger.debug(output.stderr)

      const findings = this.parseOutput(output.stderr)
      for (const finding of findings) {
        if (!finding.location) {
          // There is nowhere in the editor to show this, but the user still wants to
          // know that the tool said it, whether or not this run is one we can use
          this.logger.info(`${this.toolName}: ${finding.message}`)
        }
      }

      const groups = groupFindingsByFile(findings, cwd)
      const located = groups.reduce((count, group) => count + group.findings.length, 0)

      const failure = runFailure(output.error, LINT_TIMEOUT_MS, located)
      if (failure) {
        // Nothing this run reported can be shown, so what is on screen stays as it is
        this.logger.error(`${this.toolName} lint failed: ${failure}`)
        this.notifyOnce('error', `${this.toolName} lint failed: ${failure}`)
        return
      }
      this.applyDiagnostics(groups)
    } catch (e) {
      // Whatever went wrong here is this linter's problem, and the run it was started
      // from has other linters to get to
      const reason = e instanceof Error ? e.message : String(e)
      this.logger.error(`${this.toolName} lint failed: ${reason}`)
      this.notifyOnce('error', `${this.toolName} lint failed: ${reason}`)
    }
  }

  /// Forget the executable we resolved and the notifications we already showed, so
  /// that the settings the user just changed are picked up again
  onSettingsChanged(): void {
    this.resolvedPath = undefined
    this.toolLookupFailed = false
    this.notified.clear()
    this.warnedUnresolved.clear()
  }

  /// Drop every diagnostic this linter reported
  clearAll(): void {
    this.diagnostics.clear()
    this.reportedFiles.clear()
  }

  /// The arguments that describe what to lint, without the linter's settings
  protected abstract toolArgs(target: vscode.Uri): string[]
  /// Read the tool's output
  protected abstract parseOutput(output: string): LinterFinding[]

  private async toolPath(): Promise<string | undefined> {
    if (this.resolvedPath !== undefined) {
      return this.resolvedPath
    }
    if (this.toolLookupFailed || this.context === undefined) {
      return undefined
    }

    const resolved = await this.path.resolveToolPath(this.context, this.logger)
    if (!resolved) {
      this.toolLookupFailed = true
      const setting = this.path.configPath
      const message = `${this.toolName} was not found, so linting is off. Set ${setting}.`
      this.logger.warn(message)
      this.notifyOnce('warning', message)
      return undefined
    }

    this.resolvedPath = resolved
    this.logger.info(`Using ${resolved}`)
    return resolved
  }

  /// Each entry may hold several arguments and may quote them, so that an argument
  /// containing a space can still be written the way a shell would take it. The
  /// variables are expanded after the split, so that a value holding a space stays a
  /// single argument
  private configuredArgs(context: ExpansionContext): string[] {
    return this.args
      .getValue()
      .flatMap((arg) => parseArgsStringToArgv(arg))
      .map((arg) => this.expand(arg, context))
  }

  /// What the variables in the configured arguments mean for this run
  private expansionContext(
    target: vscode.Uri,
    cwd: string,
    workspaceFolder: string | undefined
  ): ExpansionContext {
    return {
      workspaceFolder,
      file: target.fsPath,
      cwd,
      env: process.env,
      config: (id) => {
        const value = vscode.workspace.getConfiguration().get(id)
        return typeof value === 'string' || typeof value === 'number' || typeof value === 'boolean'
          ? String(value)
          : undefined
      },
    }
  }

  /// A variable that cannot be resolved is left in the argument for the tool to
  /// complain about, and said out loud once per settings change
  private expand(arg: string, context: ExpansionContext): string {
    const { text, unresolved } = expandVariables(arg, context)
    for (const variable of unresolved) {
      if (this.warnedUnresolved.has(variable.name)) {
        continue
      }
      this.warnedUnresolved.add(variable.name)
      this.logger.warn(
        `${this.args.configPath}: ${variable.name} was not expanded because ${variable.reason}`
      )
    }
    return text
  }

  private run(tool: string, args: string[], cwd: string): Promise<RunResult> {
    return new Promise((resolve) => {
      child_process.execFile(
        tool,
        args,
        { cwd, encoding: 'utf-8', timeout: LINT_TIMEOUT_MS, maxBuffer: MAX_OUTPUT_BYTES },
        (error, _stdout, stderr) => {
          resolve({ stderr: stderr ?? '', error })
        }
      )
    })
  }

  private applyDiagnostics(groups: FindingGroup[]): void {
    const reported = new Map<string, ReportedFile>()

    for (const group of groups) {
      const uri = vscode.Uri.file(group.file)
      reported.set(uri.toString(), {
        uri,
        diagnostics: group.findings.map((finding) => this.toDiagnostic(finding)),
      })
    }

    // A file that stopped showing up in the output is either fixed or gone, so its
    // diagnostics go with it. Only files this linter reported on are touched, and
    // only the ones this run knows about are replaced.
    for (const [key, file] of this.reportedFiles) {
      if (!reported.has(key)) {
        this.diagnostics.delete(file.uri)
      }
    }
    for (const file of reported.values()) {
      this.diagnostics.set(file.uri, file.diagnostics)
    }
    this.reportedFiles = reported
  }

  private toDiagnostic(finding: LocatedFinding): vscode.Diagnostic {
    const location = finding.location
    // These positions come from outside the editor, and a document cannot hold a
    // negative one
    const line = Math.max(0, location.line - 1)
    const start = Math.max(0, location.column - 1)
    const diagnostic = new vscode.Diagnostic(
      new vscode.Range(line, start, line, start + location.length),
      finding.message,
      severityOf(finding.severity)
    )
    diagnostic.source = this.toolName
    if (finding.code) {
      diagnostic.code = finding.code
    }
    return diagnostic
  }

  private notifyOnce(level: 'warning' | 'error', message: string): void {
    if (this.notified.has(message)) {
      return
    }
    this.notified.add(message)
    if (level === 'error') {
      void vscode.window.showErrorMessage(message)
    } else {
      void vscode.window.showWarningMessage(message)
    }
  }
}

function severityOf(severity: LinterSeverity): vscode.DiagnosticSeverity {
  switch (severity) {
    case 'error':
      return vscode.DiagnosticSeverity.Error
    case 'warning':
      return vscode.DiagnosticSeverity.Warning
    default:
      return vscode.DiagnosticSeverity.Information
  }
}
