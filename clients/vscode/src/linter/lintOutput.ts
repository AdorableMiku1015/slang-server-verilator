// SPDX-License-Identifier: MIT
import * as path from 'path'

/// Shared shape for what an external linter reports, kept free of any editor or
/// tool specific type so that the parsing and grouping stay unit testable.

export type LinterSeverity = 'error' | 'warning' | 'info'

export interface LinterFindingLocation {
  file: string
  /// 1-based
  line: number
  /// 1-based
  column: number
  /// Number of characters the tool marked, at least 1
  length: number
}

export interface LinterFinding {
  severity: LinterSeverity
  /// Warning category, e.g. `WIDTH` for `%Warning-WIDTH`
  code: string | undefined
  message: string
  /// Absent for findings the tool reported without a source location
  location: LinterFindingLocation | undefined
}

export type LocatedFinding = LinterFinding & { location: LinterFindingLocation }

export interface FindingGroup {
  /// Absolute path
  file: string
  findings: LocatedFinding[]
}

/// Resolve the file each finding belongs to and group the findings by it. Relative
/// paths are resolved against the directory the tool ran in, and findings without a
/// location are left out.
export function groupFindingsByFile(findings: LinterFinding[], cwd: string): FindingGroup[] {
  const groups = new Map<string, FindingGroup>()

  for (const finding of findings) {
    if (!finding.location) {
      continue
    }
    const file = path.isAbsolute(finding.location.file)
      ? path.normalize(finding.location.file)
      : path.resolve(cwd, finding.location.file)

    let group = groups.get(file)
    if (!group) {
      group = { file, findings: [] }
      groups.set(file, group)
    }
    group.findings.push({ ...finding, location: finding.location })
  }

  return [...groups.values()]
}
