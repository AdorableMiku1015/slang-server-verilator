import { readFileSync, writeFileSync } from 'fs'
import { join } from 'path'
import tape from 'tape'
import * as tmp from 'tmp-promise'
import {
  buildLocalVersion,
  injectVersion,
  parseArgs,
  runWithInjectedVersion,
  validateBase,
  versionFromText,
} from '../../scripts/localVersion'

const SAMPLE_PACKAGE_JSON = `{
  "name": "vscode-slang",
  "version": "0.3.1",
  "publisher": "Hudson-River-Trading"
}
`

/** A fixed local time so the timestamp assertions never depend on the clock. */
const FIXED_NOW = new Date(2026, 8, 19, 8, 56, 30)

/** Dependency stubs so version building does not shell out to git. */
function deps(hash: string | undefined, dirty: boolean) {
  return { now: FIXED_NOW, gitHash: () => hash ?? '', gitDirty: () => dirty }
}

tape('versionFromText reads the version field', (assert) => {
  assert.equal(versionFromText(SAMPLE_PACKAGE_JSON), '0.3.1')
  assert.throws(() => versionFromText('{"name":"x"}'), /No "version" field/)
  assert.end()
})

tape('injectVersion rewrites only the version line', (assert) => {
  const updated = injectVersion(SAMPLE_PACKAGE_JSON, '0.3.1-local.202609190856.abc1234')

  assert.ok(updated.includes('"version": "0.3.1-local.202609190856.abc1234"'))
  assert.ok(updated.includes('"name": "vscode-slang"'))
  assert.ok(updated.includes('"publisher": "Hudson-River-Trading"'))
  // Everything except the version value is byte-for-byte preserved.
  assert.equal(updated.replace('0.3.1-local.202609190856.abc1234', '0.3.1'), SAMPLE_PACKAGE_JSON)
  assert.end()
})

tape('injectVersion rejects text without a version field', (assert) => {
  assert.throws(() => injectVersion('{\n  "name": "x"\n}\n', '1.0.0'), /Could not rewrite/)
  assert.end()
})

tape('validateBase accepts only MAJOR.MINOR.PATCH', (assert) => {
  assert.doesNotThrow(() => validateBase('0.3.1'))
  assert.doesNotThrow(() => validateBase('10.20.30'))
  assert.throws(() => validateBase('0.3.1-test.10'), /is not MAJOR.MINOR.PATCH/)
  assert.throws(() => validateBase('0.3'), /is not MAJOR.MINOR.PATCH/)
  assert.throws(() => validateBase(''), /is not MAJOR.MINOR.PATCH/)
  assert.end()
})

tape('buildLocalVersion stamps base, timestamp, hash and dirtiness', (assert) => {
  assert.equal(
    buildLocalVersion('0.3.1', deps('abc1234', false)),
    '0.3.1-local.202609190856.abc1234'
  )
  assert.equal(
    buildLocalVersion('0.3.1', deps('abc1234', true)),
    '0.3.1-local.202609190856.abc1234.dirty'
  )
  // Outside a git checkout the hash is simply omitted.
  assert.equal(buildLocalVersion('0.3.1', deps(undefined, false)), '0.3.1-local.202609190856')
  assert.end()
})

tape('buildLocalVersion zero-pads the timestamp', (assert) => {
  const early = new Date(2026, 0, 2, 3, 4, 5)
  assert.equal(
    buildLocalVersion('0.3.1', { now: early, gitHash: () => '', gitDirty: () => false }),
    '0.3.1-local.202601020304'
  )
  assert.end()
})

tape('buildLocalVersion keeps repeated builds ordered within a minute', (assert) => {
  const earlier = buildLocalVersion('0.3.1', {
    now: new Date(2026, 8, 19, 8, 56),
    gitHash: () => 'abc1234',
    gitDirty: () => false,
  })
  const later = buildLocalVersion('0.3.1', {
    now: new Date(2026, 8, 19, 8, 57),
    gitHash: () => 'abc1234',
    gitDirty: () => false,
  })
  assert.ok(earlier < later, `${earlier} should sort before ${later}`)
  assert.end()
})

tape('parseArgs splits flags from the packaged command', (assert) => {
  assert.deepEqual(parseArgs([], {}), { base: '', dryRun: false, show: false, command: [] })
  assert.deepEqual(parseArgs(['--show'], {}).show, true)
  assert.deepEqual(parseArgs(['--dry-run'], {}).dryRun, true)
  assert.deepEqual(parseArgs(['--base', '0.4.0'], {}).base, '0.4.0')
  assert.deepEqual(parseArgs(['--', 'pnpm', 'vsce', 'package'], {}).command, [
    'pnpm',
    'vsce',
    'package',
  ])
  // Flags after the separator belong to the packaged command, not to us.
  assert.deepEqual(parseArgs(['--dry-run', '--', 'vsce', '--show'], {}), {
    base: '',
    dryRun: true,
    show: false,
    command: ['vsce', '--show'],
  })
  assert.end()
})

tape('parseArgs reads the base from the environment', (assert) => {
  assert.equal(parseArgs([], { SLANG_LOCAL_BASE_VERSION: '0.5.0' }).base, '0.5.0')
  assert.equal(parseArgs([], {}).base, '')
  // An explicit flag wins over the environment.
  assert.equal(parseArgs(['--base', '0.4.0'], { SLANG_LOCAL_BASE_VERSION: '0.5.0' }).base, '0.4.0')
  assert.end()
})

tape('parseArgs rejects unknown options', (assert) => {
  assert.throws(() => parseArgs(['--bogus'], {}), /Unknown option: --bogus/)
  assert.end()
})

/**
 * Writes a script that copies the version field of a package.json into another
 * file. Recording to disk lets the test observe what the command saw even
 * though package.json is restored before the call returns.
 */
function writeVersionProbe(dirPath: string): { probe: string; observed: string } {
  const probe = join(dirPath, 'probe.js')
  const observed = join(dirPath, 'observed.txt')
  writeFileSync(
    probe,
    'const fs = require("fs");' +
      'fs.writeFileSync(process.argv[3], JSON.parse(fs.readFileSync(process.argv[2], "utf8")).version);'
  )
  return { probe, observed }
}

tape(
  'runWithInjectedVersion shows the local version to the command, then restores',
  async (assert) => {
    await tmp.withDir(
      async ({ path }) => {
        const packageJson = join(path, 'package.json')
        writeFileSync(packageJson, SAMPLE_PACKAGE_JSON)

        const inject = '0.3.1-local.202609190856.abc1234'
        const { probe, observed } = writeVersionProbe(path)
        const status = runWithInjectedVersion(
          [process.execPath, probe, packageJson, observed],
          inject,
          packageJson
        )

        assert.equal(status, 0, 'command succeeds')
        assert.equal(readFileSync(observed, 'utf8'), inject, 'command saw the injected version')
        assert.equal(readFileSync(packageJson, 'utf8'), SAMPLE_PACKAGE_JSON, 'file is restored')
      },
      { unsafeCleanup: true }
    )
    assert.end()
  }
)

tape('runWithInjectedVersion restores package.json after failure', async (assert) => {
  await tmp.withDir(
    async ({ path }) => {
      const packageJson = join(path, 'package.json')
      writeFileSync(packageJson, SAMPLE_PACKAGE_JSON)

      const status = runWithInjectedVersion(
        [process.execPath, '-e', 'process.exit(3)'],
        '0.3.1-local.202609190856.abc1234',
        packageJson
      )

      assert.equal(status, 3, 'exit status is propagated')
      assert.equal(readFileSync(packageJson, 'utf8'), SAMPLE_PACKAGE_JSON, 'file is restored')
    },
    { unsafeCleanup: true }
  )
  assert.end()
})

tape('runWithInjectedVersion requires a command', (assert) => {
  assert.throws(() => runWithInjectedVersion([], '0.3.1-local.1'), /No command given/)
  assert.end()
})
