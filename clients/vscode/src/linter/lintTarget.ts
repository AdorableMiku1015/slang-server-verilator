// SPDX-License-Identifier: MIT

/// What a linter is pointed at. `vscode.Uri` satisfies this as it is, so the caller does
/// not have to convert anything.
export interface LintTarget {
  readonly scheme: string
  readonly fsPath: string
}

/// Whether the target is a file the tool can read. An external linter is handed a path and
/// never sees an open document, so a comparison view of a historical revision
/// (`git-graph:`, whose `fsPath` comes out as `\file.sv`), an unsaved buffer (`untitled:`)
/// and a file that is gone all have nothing to hand over. `exists` is injected so that the
/// decision can be tested without a file system.
export function isLintableTarget(
  target: LintTarget | undefined,
  exists: (fsPath: string) => boolean
): boolean {
  return target !== undefined && target.scheme === 'file' && exists(target.fsPath)
}
