import tape from 'tape'
import { runFailure } from '../../src/linter/runFailure'

const timeoutMs = 30_000

tape('runFailure: a clean run is a run whose findings can be shown', (assert) => {
  assert.equal(runFailure(null, timeoutMs, 0), undefined)
  assert.equal(runFailure({ code: 0, killed: false, signal: null }, timeoutMs, 3), undefined)
  assert.end()
})

tape('runFailure: a non-zero exit that placed a finding is the tool reporting', (assert) => {
  assert.equal(runFailure({ code: 1, killed: false, signal: null }, timeoutMs, 1), undefined)
  assert.equal(runFailure({ code: 1, killed: false, signal: null }, timeoutMs, 7), undefined)
  assert.end()
})

tape('runFailure: a non-zero exit with nothing to show keeps the old findings', (assert) => {
  assert.equal(
    runFailure({ code: 1, killed: false, signal: null }, timeoutMs, 0),
    'exited with status 1 without reporting anything in a file'
  )
  assert.end()
})

tape('runFailure: a crash status is a failure even with output', (assert) => {
  const crash = { code: 0xc0000005, signal: null }
  assert.equal(runFailure(crash, timeoutMs, 0), 'crashed with status 3221225477')
  assert.equal(runFailure(crash, timeoutMs, 2), 'crashed with status 3221225477')
  assert.end()
})

tape('runFailure: a tool that could not be started is a failure', (assert) => {
  assert.equal(
    runFailure({ code: 'ENOENT', message: 'spawn verilator ENOENT' }, timeoutMs, 0),
    'spawn verilator ENOENT'
  )
  assert.end()
})

tape('runFailure: output over the buffer limit is a failure', (assert) => {
  assert.equal(
    runFailure(
      { code: 'ERR_CHILD_PROCESS_STDIO_MAXBUFFER', message: 'stderr maxBuffer length exceeded' },
      timeoutMs,
      0
    ),
    'stderr maxBuffer length exceeded'
  )
  assert.end()
})

tape('runFailure: a run we had to kill is reported as a timeout', (assert) => {
  assert.equal(runFailure({ killed: true, signal: 'SIGTERM' }, timeoutMs, 0), 'timed out after 30s')
  assert.end()
})

tape('runFailure: a tool that died by signal is a failure', (assert) => {
  assert.equal(
    runFailure({ code: null, killed: false, signal: 'SIGSEGV' }, timeoutMs, 0),
    'killed by SIGSEGV'
  )
  assert.end()
})

tape('runFailure: a tool that died without a status is a failure', (assert) => {
  assert.equal(runFailure({ code: null, killed: false, signal: null }, timeoutMs, 0), 'did not run')
  assert.equal(runFailure({ code: null, message: 'boom' }, timeoutMs, 0), 'boom')
  assert.end()
})
