import * as path from 'path'
import tape from 'tape'
import { groupFindingsByFile, LinterFinding } from '../../src/linter/lintOutput'
import { parseVerilatorOutput } from '../../src/linter/verilatorOutput'

const syntaxError = [
  "%Error: t/t_math.sv:8:8: syntax error, unexpected '('",
  '    8 |     wire x = ((a + b);',
  '      |             ^',
  '%Error: Exiting due to 1 error(s)',
].join('\n')

tape('parseVerilatorOutput: reads the location and severity of an error', (assert) => {
  const findings = parseVerilatorOutput(syntaxError)

  assert.equal(findings.length, 1, 'the summary line is not a finding')
  assert.equal(findings[0].severity, 'error')
  assert.equal(findings[0].code, undefined)
  assert.equal(findings[0].message, "syntax error, unexpected '('")
  assert.deepEqual(findings[0].location, {
    file: 't/t_math.sv',
    line: 8,
    column: 8,
    length: 1,
  })
  assert.end()
})

tape('parseVerilatorOutput: keeps the warning category and the marked range', (assert) => {
  const output = [
    '%Warning-WIDTH: t/t_math.sv:20:10: Operator ASSIGNW expects 4 bits on the Assign RHS.',
    "                                      : ... In instance 'top'",
    "   20 |     assign y = 16'h1234;",
    '      |              ^~~~~~',
  ].join('\n')

  const findings = parseVerilatorOutput(output)

  assert.equal(findings.length, 1)
  assert.equal(findings[0].severity, 'warning')
  assert.equal(findings[0].code, 'WIDTH')
  assert.equal(findings[0].message, 'Operator ASSIGNW expects 4 bits on the Assign RHS.')
  assert.deepEqual(findings[0].location, {
    file: 't/t_math.sv',
    line: 20,
    column: 10,
    length: 6,
  })
  assert.end()
})

tape('parseVerilatorOutput: reads a marker made only of tildes', (assert) => {
  const output = [
    '%Warning-UNUSED: t/t_math.sv:4:9: Signal is not used.',
    '    4 |     wire unused_sig;',
    '      |         ~~~~~~',
  ].join('\n')

  const findings = parseVerilatorOutput(output)

  assert.equal(findings[0].location?.length, 6)
  assert.end()
})

tape('parseVerilatorOutput: finds the marker under the notes of a warning', (assert) => {
  const output = [
    '%Warning-WIDTH: t/t_top.sv:20:10: Operator ASSIGNW expects 4 bits on the Assign RHS.',
    "                                    : ... In instance 'top'",
    "                                    : ... note: In instance 'top.u_sub'",
    "                                    : ... In instance 'top'",
    "   20 |     assign y = 16'h1234;",
    '      |              ^~~~~~',
  ].join('\n')

  const findings = parseVerilatorOutput(output)

  assert.equal(findings.length, 1)
  assert.equal(findings[0].location?.length, 6)
  assert.end()
})

tape('parseVerilatorOutput: keeps each marker with its own diagnostic', (assert) => {
  const output = [
    '%Warning-WIDTH: t/t_top.sv:20:10: Operator ASSIGNW expects 4 bits on the Assign RHS.',
    "                                    : ... In instance 'top'",
    "                                    : ... note: In instance 'top.u_sub'",
    "                                    : ... In instance 'top'",
    "   20 |     assign y = 16'h1234;",
    '      |              ^~~~~~',
    '%Warning-UNUSED: t/t_top.sv:30:9: Signal is not used.',
    '   30 |     wire unused_sig;',
    '      |         ~~~~~~',
  ].join('\n')

  const findings = parseVerilatorOutput(output)

  assert.equal(findings.length, 2)
  assert.equal(findings[0].severity, 'warning')
  assert.equal(findings[0].location?.line, 20)
  assert.equal(findings[0].location?.length, 6)
  assert.equal(findings[1].location?.line, 30)
  assert.equal(findings[1].location?.length, 6)
  assert.end()
})

tape('parseVerilatorOutput: does not take the marker of the diagnostic after it', (assert) => {
  const output = [
    '%Error: t/t_math.sv:3:1: syntax error',
    '%Error: t/t_math.sv:7:2: syntax error',
    '    7 |     foo bar;',
    '      |     ^~~',
  ].join('\n')

  const findings = parseVerilatorOutput(output)

  assert.equal(findings.length, 2)
  assert.equal(findings[0].location?.length, 1, 'the first has no snippet of its own')
  assert.equal(findings[1].location?.length, 3)
  assert.end()
})

tape('parseVerilatorOutput: keeps findings that have no location', (assert) => {
  const findings = parseVerilatorOutput("%Error: Cannot open file 'missing.sv'")

  assert.equal(findings.length, 1)
  assert.equal(findings[0].severity, 'error')
  assert.equal(findings[0].message, "Cannot open file 'missing.sv'")
  assert.equal(findings[0].location, undefined)
  assert.end()
})

tape('parseVerilatorOutput: falls back to one character without a marker line', (assert) => {
  const findings = parseVerilatorOutput('%Error: t/t.v:3:1: Cannot find module: missing')

  assert.equal(findings.length, 1)
  assert.equal(findings[0].message, 'Cannot find module: missing')
  assert.deepEqual(findings[0].location, {
    file: 't/t.v',
    line: 3,
    column: 1,
    length: 1,
  })
  assert.end()
})

tape('parseVerilatorOutput: keeps a Windows path with a drive letter', (assert) => {
  const output = [
    "%Warning-UNUSED: C:\\work\\src\\top.sv:42:3: Signal is not used: 'unused_sig'",
    '   42 |     logic unused_sig;',
    '      |           ^~~~~~~~~~',
  ].join('\n')

  const findings = parseVerilatorOutput(output)

  assert.equal(findings[0].location?.file, 'C:\\work\\src\\top.sv')
  assert.equal(findings[0].location?.line, 42)
  assert.equal(findings[0].location?.column, 3)
  assert.equal(findings[0].location?.length, 10)
  assert.end()
})

tape('parseVerilatorOutput: defaults the column when the tool leaves it out', (assert) => {
  const findings = parseVerilatorOutput("%Error: t/t.v:12: syntax error, unexpected '('")

  assert.deepEqual(findings[0].location, {
    file: 't/t.v',
    line: 12,
    column: 1,
    length: 1,
  })
  assert.end()
})

tape('parseVerilatorOutput: returns nothing for empty output', (assert) => {
  assert.deepEqual(parseVerilatorOutput(''), [])
  assert.end()
})

// Captured from verilator v5.050 (MSYS2 build) running `verilator_bin --lint-only -sv width.sv`
const realWidthOutput = `%Warning-WIDTHEXPAND: width.sv:2:16: Operator ADD expects 16 bits on the LHS, but LHS's VARREF 'a' generates 4 bits.
                                   : ... note: In instance 'width'
    2 |   assign y = a + 16'h1234;
      |                ^
                      ... For warning description see https://verilator.org/warn/WIDTHEXPAND?v=0.000
                      ... Use "/* verilator lint_off WIDTHEXPAND */" and lint_on around source to disable this message.
%Warning-WIDTHTRUNC: width.sv:2:12: Operator ASSIGNW expects 4 bits on the Assign RHS, but Assign RHS's ADD generates 16 bits.
                                  : ... note: In instance 'width'
    2 |   assign y = a + 16'h1234;
      |            ^
                     ... For warning description see https://verilator.org/warn/WIDTHTRUNC?v=0.000
                     ... Use "/* verilator lint_off WIDTHTRUNC */" and lint_on around source to disable this message.
%Error: Exiting due to 2 warning(s)`

// Captured from verilator v5.050 (MSYS2 build) running `verilator_bin --lint-only -sv uses_inc.sv`
const realIncludeOutput = `%Warning-WIDTHTRUNC: inc.svh:1:26: Operator ASSIGN expects 4 bits on the Assign RHS, but Assign RHS's CONST '16'h1234' generates 16 bits.
                                 : ... note: In instance 'uses_inc'
    1 |   logic [3:0] head_sig = 16'h1234;
      |                          ^~~~~~~~
                     uses_inc.sv:3:1: ... note: In file included from 'uses_inc.sv'
                     ... For warning description see https://verilator.org/warn/WIDTHTRUNC?v=0.000
                     ... Use "/* verilator lint_off WIDTHTRUNC */" and lint_on around source to disable this message.
%Error: Exiting due to 1 warning(s)`

// Captured from verilator v5.050 (MSYS2 build) running `verilator_bin --lint-only -sv missing_file.sv`,
// with the "Looked in" listing it prints in between left out
const realMissingTargetOutput = `%Error: Cannot find file containing module: 'missing_file.sv'
        ... See the manual at https://verilator.org/verilator_doc.html?v=0.000 for more assistance.
%Error: Exiting due to 1 error(s)`

tape('parseVerilatorOutput: reads what verilator actually prints for a warning', (assert) => {
  const findings = parseVerilatorOutput(realWidthOutput)

  assert.deepEqual(
    findings.map((f) => [
      f.severity,
      f.code,
      f.location?.line,
      f.location?.column,
      f.location?.length,
    ]),
    [
      ['warning', 'WIDTHEXPAND', 2, 16, 1],
      ['warning', 'WIDTHTRUNC', 2, 12, 1],
    ],
    'the notes, the description lines and the summary are not findings of their own'
  )
  assert.equal(
    findings[0].message,
    "Operator ADD expects 16 bits on the LHS, but LHS's VARREF 'a' generates 4 bits."
  )
  assert.end()
})

tape('parseVerilatorOutput: attributes a warning in an included header to the header', (assert) => {
  const findings = parseVerilatorOutput(realIncludeOutput)

  assert.equal(findings.length, 1, 'the "included from" note is not a finding of its own')
  assert.equal(findings[0].code, 'WIDTHTRUNC')
  assert.deepEqual(findings[0].location, {
    file: 'inc.svh',
    line: 1,
    column: 26,
    length: 8,
  })
  assert.end()
})

tape('parseVerilatorOutput: keeps the message of an error that has no file', (assert) => {
  const findings = parseVerilatorOutput(realMissingTargetOutput)

  assert.equal(findings.length, 1)
  assert.equal(findings[0].severity, 'error')
  assert.equal(findings[0].message, "Cannot find file containing module: 'missing_file.sv'")
  assert.equal(findings[0].location, undefined)
  assert.end()
})

const cwd = path.resolve('ws')

function finding(file: string, line: number): LinterFinding {
  return {
    severity: 'error',
    code: undefined,
    message: 'boom',
    location: { file, line, column: 1, length: 1 },
  }
}

tape('groupFindingsByFile: resolves relative paths against the run directory', (assert) => {
  const groups = groupFindingsByFile([finding('t/t_math.sv', 8)], cwd)

  assert.equal(groups.length, 1)
  assert.equal(groups[0].file, path.join(cwd, 't/t_math.sv'))
  assert.equal(groups[0].findings[0].location?.line, 8)
  assert.end()
})

tape('groupFindingsByFile: keeps absolute paths and collects per file', (assert) => {
  const absolute = path.join(cwd, 'top.sv')
  const groups = groupFindingsByFile(
    [finding(absolute, 1), finding('t/t_math.sv', 2), finding('t/t_math.sv', 3)],
    cwd
  )

  assert.equal(groups.length, 2)
  assert.equal(groups[0].file, absolute)
  assert.equal(groups[0].findings.length, 1)
  assert.equal(groups[1].file, path.join(cwd, 't/t_math.sv'))
  assert.equal(groups[1].findings.length, 2)
  assert.end()
})

tape('groupFindingsByFile: drops findings without a location', (assert) => {
  const unlocated: LinterFinding = {
    severity: 'error',
    code: undefined,
    message: 'Cannot open file',
    location: undefined,
  }

  assert.deepEqual(groupFindingsByFile([unlocated], cwd), [])
  assert.end()
})
