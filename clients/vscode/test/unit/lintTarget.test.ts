import tape from 'tape'
import { isLintableTarget, LintTarget } from '../../src/linter/lintTarget'

const onDisk = 'C:\\proj\\rtl\\top.sv'
const exists = (fsPath: string) => fsPath === onDisk
const file = (fsPath: string): LintTarget => ({ scheme: 'file', fsPath })

tape('isLintableTarget: a file that is on disk is lintable', (assert) => {
  assert.equal(isLintableTarget(file(onDisk), exists), true)
  assert.end()
})

tape('isLintableTarget: another scheme is not a file the tool can read', (assert) => {
  // Git Graph shows a historical revision from `git-graph:/\file.sv`, and the document it
  // hands out is not the file the tool would be pointed at
  const never = () => {
    throw new Error('a scheme that is not a file should not be looked up')
  }
  assert.equal(isLintableTarget({ scheme: 'git-graph', fsPath: '\\file.sv' }, never), false)
  assert.equal(isLintableTarget({ scheme: 'untitled', fsPath: 'Untitled-1' }, never), false)
  assert.equal(isLintableTarget({ scheme: 'git', fsPath: '\\file.sv' }, never), false)
  assert.end()
})

tape('isLintableTarget: a path that is not there cannot be linted', (assert) => {
  assert.equal(isLintableTarget(file(onDisk), () => false), false)
  assert.equal(isLintableTarget(file('C:\\proj\\rtl\\gone.sv'), exists), false)
  assert.end()
})

tape('isLintableTarget: no target at all is not a file', (assert) => {
  assert.equal(isLintableTarget(undefined, exists), false)
  assert.end()
})

tape('isLintableTarget: asks about the path of the target', (assert) => {
  const asked: string[] = []
  const askedFor = (fsPath: string) => {
    asked.push(fsPath)
    return true
  }
  assert.equal(isLintableTarget(file('C:\\proj\\rtl\\a.sv'), askedFor), true)
  assert.deepEqual(asked, ['C:\\proj\\rtl\\a.sv'])
  assert.end()
})
