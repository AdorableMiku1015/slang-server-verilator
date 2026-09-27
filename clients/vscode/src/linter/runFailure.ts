// SPDX-License-Identifier: MIT

/// The part of an `execFile` error that decides whether the tool ran to
/// completion. Kept structural so that the classification can be tested without
/// spawning anything.
export interface ProcessError {
  code?: string | number | null
  killed?: boolean
  signal?: string | null
  message?: string
}

/// Windows reports a crashed process with the high bit of the status set, e.g.
/// 0xC0000005 for an access violation, which is not a status a linter chooses
const CRASH_STATUS_FLOOR = 0x8000_0000

/// Why a run produced nothing that can replace the diagnostics already on screen,
/// or undefined when it produced results worth showing.
///
/// A non-zero exit status is how a linter reports what it found, so the status
/// alone says little: what decides is whether the run got far enough to report
/// anything. A process that could not be started, drowned in its own output, was
/// killed, died of a signal, or crashed leaves nothing usable, and neither does one
/// that exited non-zero without placing a single finding in a file.
export function runFailure(
  error: ProcessError | null,
  timeoutMs: number,
  locatedFindings: number
): string | undefined {
  if (!error) {
    return undefined
  }
  if (error.killed) {
    return `timed out after ${timeoutMs / 1000}s`
  }
  if (error.signal) {
    return `killed by ${error.signal}`
  }
  if (typeof error.code !== 'number') {
    // A string code is a spawn error or the output buffer overflowing, and no code
    // at all means the process died without a status of its own
    return error.message ?? 'did not run'
  }
  if (error.code >= CRASH_STATUS_FLOOR) {
    return `crashed with status ${error.code}`
  }
  if (error.code !== 0 && locatedFindings === 0) {
    return `exited with status ${error.code} without reporting anything in a file`
  }
  return undefined
}
