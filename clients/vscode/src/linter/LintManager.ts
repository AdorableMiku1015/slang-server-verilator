// SPDX-License-Identifier: MIT
import * as path from 'path'
import * as vscode from 'vscode'
import { ConfigObject, ExtensionComponent } from '../lib/libconfig'
import { getWorkspaceFolder, isAnyVerilog } from '../utils'
import { ExternalLinter } from './ExternalLinter'
import { VerilatorLinter } from './VerilatorLinter'

/// Triggers arrive in bursts, and the tools are being run over a whole compilation,
/// so the linters wait until the burst is over
const LINT_DEBOUNCE_MS = 300

/// What the project is compiling, which is where the linters point when the user has
/// selected a top level. `ProjectComponent` satisfies this without either side having
/// to know about the other.
export interface CompilationSource {
  readonly topFile: vscode.Uri | undefined
  readonly onDidChangeCompilationSource: vscode.Event<void>
}

export class LintManager extends ExtensionComponent {
  /// Diagnostics of the slang language server itself, which the extension's
  /// middleware forwards. External linters have their own switches, below.
  enabled: ConfigObject<boolean> = new ConfigObject({
    default: true,
    description: 'Enable diagnostics from the slang language server',
  })

  /// External linters
  verilator: VerilatorLinter = new VerilatorLinter()

  private linters: ExternalLinter[] = [this.verilator]

  private context: vscode.ExtensionContext | undefined
  private source: CompilationSource | undefined
  private pendingLint: ReturnType<typeof setTimeout> | undefined
  private running: boolean = false
  private rerunRequested: boolean = false

  async activate(context: vscode.ExtensionContext): Promise<void> {
    this.context = context

    for (const linter of this.linters) {
      await linter.activate(context)
    }

    context.subscriptions.push(
      vscode.workspace.onDidSaveTextDocument((doc) => {
        // A saved file can change what the tools report for the whole compilation,
        // which is not limited to the file that was saved
        if (isAnyVerilog(doc.languageId)) {
          this.scheduleLint()
        }
      }),

      vscode.window.onDidChangeActiveTextEditor((editor) => {
        // With a top file the target does not depend on the editor, so moving between
        // files cannot change what the linters would report
        if (editor && !this.source?.topFile && isAnyVerilog(editor.document.languageId)) {
          this.scheduleLint()
        }
      }),

      this.onConfigUpdated(() => {
        for (const linter of this.linters) {
          linter.clearAll()
          linter.onSettingsChanged()
        }
        this.scheduleLint()
      }),

      // A run waiting on the debounce has to go with the rest of the extension, or it
      // would start writing into a collection that has been disposed
      {
        dispose: () => {
          if (this.pendingLint !== undefined) {
            clearTimeout(this.pendingLint)
          }
        },
      }
    )
  }

  /// Follow the compilation the user selected, so that choosing or clearing a top
  /// level lints the new target and lets go of the old one
  watchCompilationSource(source: CompilationSource): void {
    this.source = source
    this.context?.subscriptions.push(source.onDidChangeCompilationSource(() => this.scheduleLint()))
  }

  private scheduleLint(): void {
    if (this.pendingLint !== undefined) {
      clearTimeout(this.pendingLint)
    }
    this.pendingLint = setTimeout(() => {
      this.pendingLint = undefined
      void this.runLinters()
    }, LINT_DEBOUNCE_MS)
  }

  private async runLinters(): Promise<void> {
    if (this.running) {
      // Whatever arrives while the tools run is picked up by the next run, which
      // works from the state things are in by then
      this.rerunRequested = true
      return
    }

    this.running = true
    try {
      await this.lintTarget()
    } finally {
      this.running = false
    }

    if (this.rerunRequested) {
      this.rerunRequested = false
      await this.runLinters()
    }
  }

  private async lintTarget(): Promise<void> {
    const target = this.target()
    if (!target) {
      // Nothing is being compiled any more, so diagnostics pointing into the files of
      // the old target would be left behind on files nobody asked about
      for (const linter of this.linters) {
        linter.clearAll()
      }
      return
    }

    this.logger.info(`Linting ${target.fsPath}`)
    const cwd = getWorkspaceFolder() ?? path.dirname(target.fsPath)
    await Promise.all(this.linters.map((linter) => linter.lint(target, cwd)))
  }

  private target(): vscode.Uri | undefined {
    const topFile = this.source?.topFile
    if (topFile) {
      return topFile
    }
    const editor = vscode.window.activeTextEditor
    return editor && isAnyVerilog(editor.document.languageId) ? editor.document.uri : undefined
  }
}
