// SPDX-License-Identifier: MIT
import { LinterFinding, LinterSeverity } from './lintOutput'

/// `%Error: file.sv:12:5: message`, `%Warning-WIDTH: file.sv:12:5: message`
const DIAGNOSTIC_RE = /^%(\w+)(?:-(\w+))?:\s(.*)$/
/// The location part of a message, with and without a column
const LOCATION_WITH_COLUMN_RE = /^(.*):(\d+):(\d+): (.*)$/
const LOCATION_RE = /^(.*):(\d+): (.*)$/
/// Verilator's own tally of what it found, not a finding itself
const SUMMARY_RE = /^Exiting due to/
/// The line under a source snippet that marks what the finding covers
const MARKER_RE = /^[\s|]*([\^~]+)/

/// Parse verilator's stderr into findings.
///
/// Diagnostics start with `%Severity` (optionally `-Category`) and carry a
/// `file:line:column:` location unless they describe something that has no source
/// position at all, such as a missing file or an unknown option. Those are returned
/// with `location` unset so the caller can decide whether to show or just log them.
export function parseVerilatorOutput(stderr: string): LinterFinding[] {
  const lines = stderr.split(/\r?\n/)
  const findings: LinterFinding[] = []

  for (let n = 0; n < lines.length; n++) {
    const diagnostic = DIAGNOSTIC_RE.exec(lines[n])
    if (!diagnostic) {
      continue
    }
    const severity = severityOf(diagnostic[1])
    const code = diagnostic[2]
    const body = diagnostic[3]
    if (SUMMARY_RE.test(body)) {
      continue
    }

    const located = splitLocation(body)
    if (!located) {
      findings.push({ severity, code, message: body, location: undefined })
      continue
    }

    findings.push({
      severity,
      code,
      message: located.message,
      location: {
        file: located.file,
        line: located.line,
        column: located.column,
        length: markerLength(lines, n),
      },
    })
  }

  return findings
}

function severityOf(word: string): LinterSeverity {
  switch (word) {
    case 'Error':
    case 'Fatal':
      return 'error'
    case 'Warning':
      return 'warning'
    default:
      return 'info'
  }
}

interface MessageLocation {
  file: string
  line: number
  column: number
  message: string
}

/// Split `file:line:column: message` into its parts. The file part is matched
/// greedily so that a path containing a colon, such as a Windows drive letter,
/// stays intact.
function splitLocation(body: string): MessageLocation | undefined {
  const withColumn = LOCATION_WITH_COLUMN_RE.exec(body)
  if (withColumn) {
    return {
      file: withColumn[1],
      line: Number(withColumn[2]),
      column: Number(withColumn[3]),
      message: withColumn[4],
    }
  }

  const withoutColumn = LOCATION_RE.exec(body)
  if (withoutColumn) {
    return {
      file: withoutColumn[1],
      line: Number(withoutColumn[2]),
      column: 1,
      message: withoutColumn[3],
    }
  }

  return undefined
}

/// The marker line under the source snippet says how much of the line the finding
/// covers. Verilator prints it as carets and tildes, e.g. `      |      ^~~~~~`, and
/// how many notes it puts in between varies with the nesting, so everything up to
/// the next diagnostic belongs to this one.
function markerLength(lines: string[], diagnosticLine: number): number {
  for (let n = diagnosticLine + 1; n < lines.length && !lines[n].startsWith('%'); n++) {
    const marker = MARKER_RE.exec(lines[n])
    if (marker) {
      return marker[1].length
    }
  }
  return 1
}
