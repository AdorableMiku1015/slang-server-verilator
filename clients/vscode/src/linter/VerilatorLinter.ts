// SPDX-License-Identifier: MIT
import * as vscode from 'vscode'
import { isSystemVerilogPath } from '../utils'
import { ExternalLinter } from './ExternalLinter'
import { LinterFinding } from './lintOutput'
import { parseVerilatorOutput } from './verilatorOutput'

export class VerilatorLinter extends ExternalLinter {
  constructor() {
    super('verilator')
  }

  protected toolArgs(target: vscode.Uri): string[] {
    const args = ['--lint-only']
    if (isSystemVerilogPath(target.fsPath)) {
      args.push('-sv')
    }
    return args
  }

  protected parseOutput(output: string): LinterFinding[] {
    return parseVerilatorOutput(output)
  }
}
