import * as os from 'os'
import * as path from 'path'
import { parseArgsStringToArgv } from 'string-argv'
import tape from 'tape'
import { ExpansionContext, expandVariables } from '../../src/linter/variableExpansion'

/// Paths that exist on any platform the tests run on
const workspaceFolder = path.join(path.sep, 'work', 'proj')
const file = path.join(workspaceFolder, 'rtl', 'top.sv')

function context(overrides: Partial<ExpansionContext> = {}): ExpansionContext {
  return { workspaceFolder, file, cwd: workspaceFolder, ...overrides }
}

tape('expandVariables: the documented variables resolve against the run', (assert) => {
  const ctx = context({
    env: { INC: path.join(path.sep, 'env', 'inc') },
    config: (id) => (id === 'slang.lint.enabled' ? 'true' : undefined),
  })

  const cases: [string, string][] = [
    ['${workspaceFolder}', workspaceFolder],
    ['${workspaceFolderBasename}', 'proj'],
    ['${fileWorkspaceFolder}', workspaceFolder],
    ['${file}', file],
    ['${fileDirname}', path.join(workspaceFolder, 'rtl')],
    ['${fileBasename}', 'top.sv'],
    ['${fileBasenameNoExtension}', 'top'],
    ['${fileExtname}', '.sv'],
    ['${relativeFile}', 'rtl/top.sv'],
    ['${relativeFileDirname}', 'rtl'],
    ['${cwd}', workspaceFolder],
    ['${pathSeparator}', path.sep],
    ['${/}', path.sep],
    ['${userHome}', os.homedir()],
    ['${env:INC}', path.join(path.sep, 'env', 'inc')],
    ['${config:slang.lint.enabled}', 'true'],
  ]

  for (const [text, expected] of cases) {
    const result = expandVariables(text, ctx)
    assert.equal(result.text, expected, `${text} expands`)
    assert.deepEqual(result.unresolved, [], `${text} resolves`)
  }
  assert.end()
})

tape('expandVariables: variables inside a longer argument, and repeats', (assert) => {
  const ctx = context()
  const include = expandVariables('-I${workspaceFolder}/rtl/inc', ctx)
  assert.equal(include.text, '-I' + workspaceFolder + '/rtl/inc')
  assert.deepEqual(include.unresolved, [])

  const repeated = expandVariables('${fileBasename}:${fileBasename}', ctx)
  assert.equal(repeated.text, 'top.sv:top.sv')

  const mixed = expandVariables('${relativeFileDirname}/${fileBasenameNoExtension}', ctx)
  assert.equal(mixed.text, 'rtl/top')
  assert.end()
})

tape('expandVariables: the basename variables follow the last extension', (assert) => {
  const dotted = context({ file: path.join(workspaceFolder, 'rtl', 'a.b.sv') })
  assert.equal(expandVariables('${fileBasename}', dotted).text, 'a.b.sv')
  assert.equal(expandVariables('${fileBasenameNoExtension}', dotted).text, 'a.b')
  assert.equal(expandVariables('${fileExtname}', dotted).text, '.sv')

  const plain = context({ file: path.join(workspaceFolder, 'rtl', 'noext') })
  assert.equal(expandVariables('${fileBasename}', plain).text, 'noext')
  assert.equal(expandVariables('${fileBasenameNoExtension}', plain).text, 'noext')
  assert.equal(expandVariables('${fileExtname}', plain).text, '')
  assert.deepEqual(expandVariables('${fileExtname}', plain).unresolved, [])
  assert.end()
})

tape('expandVariables: splitting before expanding keeps a spaced path one argument', (assert) => {
  const spaced = path.join(path.sep, 'My Work', 'proj')
  const ctx = context({
    workspaceFolder: spaced,
    file: path.join(spaced, 'top.sv'),
    cwd: spaced,
  })

  const args = parseArgsStringToArgv('-I${workspaceFolder}/rtl/inc')
  assert.deepEqual(args, ['-I${workspaceFolder}/rtl/inc'], 'the entry stays one argument')

  const expanded = args.map((arg) => expandVariables(arg, ctx).text)
  assert.deepEqual(expanded, [`-I${spaced}/rtl/inc`], 'the value is substituted into it')
  assert.equal(expanded.length, 1, 'and it is still a single argument')
  assert.ok(expanded[0].includes(' '), 'with the space inside it, not between it')
  assert.end()
})

tape('expandVariables: environment variables', (assert) => {
  const ctx = context({ env: { SET: 'value', EMPTY: '' } })
  assert.equal(expandVariables('${env:SET}', ctx).text, 'value')
  assert.equal(expandVariables('${env:SET}', ctx).unresolved.length, 0)

  const empty = expandVariables('${env:EMPTY}', ctx)
  assert.equal(empty.text, '', 'a variable that is set to nothing expands to nothing')
  assert.deepEqual(empty.unresolved, [], 'and is not reported as unresolved')

  const unset = expandVariables('${env:NOPE}', ctx)
  assert.equal(unset.text, '${env:NOPE}')
  assert.deepEqual(
    unset.unresolved.map((variable) => variable.name),
    ['${env:NOPE}']
  )
  assert.ok(unset.unresolved[0].reason.includes('NOPE'), 'the reason names the variable')

  const noEnv = expandVariables('${env:NOPE}', context())
  assert.equal(noEnv.text, '${env:NOPE}', 'an environment that is not there at all')
  assert.end()
})

tape('expandVariables: settings', (assert) => {
  const ctx = context({
    config: (id) => (id === 'slang.lint.verilator.enabled' ? 'false' : undefined),
  })
  assert.equal(expandVariables('${config:slang.lint.verilator.enabled}', ctx).text, 'false')

  const missing = expandVariables('${config:slang.unknown}', ctx)
  assert.equal(missing.text, '${config:slang.unknown}')
  assert.deepEqual(
    missing.unresolved.map((variable) => variable.name),
    ['${config:slang.unknown}']
  )

  const noLookup = expandVariables('${config:slang.unknown}', context())
  assert.equal(noLookup.text, '${config:slang.unknown}')
  assert.end()
})

tape('expandVariables: what is not a documented variable stays as written', (assert) => {
  const ctx = context()
  for (const text of [
    '${command:foo}',
    '${input:bar}',
    '${lineNumber}',
    '${selectedText}',
    '${execPath}',
    '${nonsense}',
  ]) {
    const result = expandVariables(text, ctx)
    assert.equal(result.text, text, `${text} is left alone`)
    assert.deepEqual(
      result.unresolved.map((variable) => variable.name),
      [text],
      `${text} is reported`
    )
    assert.ok(result.unresolved[0].reason.length > 0, `${text} comes with a reason`)
  }
  assert.end()
})

tape('expandVariables: a variable whose context is missing stays as written', (assert) => {
  const noFolder = expandVariables('${workspaceFolder}', {
    file,
    cwd: path.dirname(file),
    env: {},
  })
  assert.equal(noFolder.text, '${workspaceFolder}')
  assert.deepEqual(
    noFolder.unresolved.map((variable) => variable.name),
    ['${workspaceFolder}']
  )

  const noFile = expandVariables('${file}', { workspaceFolder, cwd: workspaceFolder, env: {} })
  assert.equal(noFile.text, '${file}')
  assert.deepEqual(
    noFile.unresolved.map((variable) => variable.name),
    ['${file}']
  )

  const outside = expandVariables('${relativeFile}', {
    workspaceFolder,
    file: path.join(path.sep, 'elsewhere', 'other.sv'),
    cwd: workspaceFolder,
    env: {},
  })
  assert.equal(outside.text, '${relativeFile}')
  assert.ok(outside.unresolved[0].reason.includes('not inside'))
  assert.end()
})

tape('expandVariables: a file at the workspace root has no relative dirname', (assert) => {
  const ctx = context({ file: path.join(workspaceFolder, 'top.sv') })
  assert.equal(expandVariables('${relativeFile}', ctx).text, 'top.sv')

  const dirname = expandVariables('${relativeFileDirname}', ctx)
  assert.equal(dirname.text, '', 'the empty dirname is a value, not a missing one')
  assert.deepEqual(dirname.unresolved, [])
  assert.end()
})

tape('expandVariables: text with nothing to expand comes back untouched', (assert) => {
  const ctx = context()
  assert.deepEqual(expandVariables('--lint-only -Wall', ctx), {
    text: '--lint-only -Wall',
    unresolved: [],
  })
  assert.deepEqual(expandVariables('${workspaceFolder', ctx), {
    text: '${workspaceFolder',
    unresolved: [],
  })
  assert.deepEqual(expandVariables('', ctx), { text: '', unresolved: [] })
  assert.end()
})

tape('expandVariables: arguments that belong to the tool are not rewritten', (assert) => {
  const ctx = context({ env: {} })
  const define = expandVariables('+define+X=${Y}', ctx)
  assert.equal(define.text, '+define+X=${Y}', 'an unknown name reaches the tool as written')
  assert.deepEqual(
    define.unresolved.map((variable) => variable.name),
    ['${Y}']
  )
  assert.deepEqual(expandVariables('--prefix Vtop', ctx), {
    text: '--prefix Vtop',
    unresolved: [],
  })
  assert.end()
})

tape('expandVariables: the same variable is reported once, values are not rescanned', (assert) => {
  const ctx = context({ env: { NOPE: undefined, NESTED: '${workspaceFolder}' } })
  const repeated = expandVariables('-I${env:NOPE} -I${env:NOPE}', ctx)
  assert.equal(repeated.text, '-I${env:NOPE} -I${env:NOPE}')
  assert.equal(repeated.unresolved.length, 1, 'one entry for one variable')

  const nested = expandVariables('${env:NESTED}', ctx)
  assert.equal(nested.text, '${workspaceFolder}', 'a value is not expanded again')
  assert.deepEqual(nested.unresolved, [])
  assert.end()
})
