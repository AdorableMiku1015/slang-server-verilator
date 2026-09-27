---
feature: verilator-lint-refactor
status: delivered
updated: 2026-02-01
branch: refactor/verilator-lint
commits: 26e4e8c..ca5f1ee
---

# verilator-lint-refactor

## Report

**What was built** — verilator 外部 lint 按本仓库 VS Code 客户端的约定重写了一遍。配置树收敛为 `slang.lint.*`：`slang.lint.enabled` 保留（语言服务器自身诊断的开关），外部 linter 的 `enabled`/`path`/`args` 从泄漏了内部类名的 `slang.lintManager.verilator.*` 搬到 `slang.lint.verilator.*`，`path` 改用 `PathConfigObject`（平台默认值、`which` 解析、存在性校验），`args` 每个元素经 `parseArgsStringToArgv` 解析后展平，于是既能写 `["--top-module", "foo"]` 也能写 `["--top-module foo"]` 或在其中加引号。`package.json` 与 `CONFIG.md` 已改成 Extdev 生成器会产出的键与顺序。

代码结构换成 `LintManager`（组件 `lint`：触发时机、目标选择、编排）+ `ExternalLinter`（外部 linter 的配置、进程执行、诊断落地）+ `VerilatorLinter`（只提供参数与输出解析）+ 三个不依赖 `vscode` 的纯模块（`lintOutput.ts` 汇总类型与按文件分组、`verilatorOutput.ts` 解析 verilator 输出、`runFailure.ts` 判定一次运行是否可用）。`ProjectComponent` 新增 `onDidChangeCompilationSource` 事件并把 `compilationSource` 的赋值收敛到一处，`LintManager` 通过结构接口订阅它，双方不再互相通过全局 `ext` 调用；`extension.ts` 里内联的 `LintComponent` 删除。

行为上修掉了移植版的若干缺陷：诊断按「本 linter 上次上报过的文件」增量清理而不是整表清空；诊断里的相对路径按本次运行的 cwd 解析；caret 行的长度扫描到下一个 `%` 诊断为止，不再用固定 3 行窗口（既不会漏掉带注释的告警，也不会把下一条诊断的 caret 算到自己头上）；目标为 `.sv`/`.svh` 时加 `-sv`（按扩展名判断，不要求文件已打开）；触发去抖 300ms、运行期间的新请求在结束后重跑一次；非零退出只有在「解析出至少一条可定位诊断」时才视为工具正常报告，崩溃状态（Windows 0xC0000005 这类高位状态）、信号终止、超时、启动失败、输出撑爆缓冲都算失败，失败时保留上一轮诊断并在输出通道之外提示一次。

**Verification**

- `npx tsc -p . --outDir out`（clients/vscode）：PASS，0 error
- `npx tape "out/test/**/*.js"`：PASS 160/160（新增 `verilatorOutput.test.ts`、`runFailure.test.ts`）
- `npx eslint src --ext ts`：FAIL，唯一一条 `src/extension.ts:270` `@typescript-eslint/no-misused-promises` — **PRE-EXISTING**（对 `git show HEAD:` 的原始文件跑同一命令得到同一条），在 `slang/internalError` 通知处理里，与本次改动无关
- `pnpm run check-types`：`gen-types` 步骤失败 — **PRE-EXISTING**（`clients/vscode/pnpm-workspace.yaml` 没有 `packages:`，`-F slanglib` 无匹配），类型检查本身由上一条 tsc 覆盖
- prettier / pre-commit：本机未安装（`node_modules` 里没有 prettier、无 prek），格式按 `.prettierrc` 手工对齐（2 空格、单引号、无分号、100 列、es5 尾逗号），逐文件确认无超长行
- 差分脚本（临时、已删除）把旧的移植版正则与新解析器在五种输出形态上对跑：file/line/column/message/code 一致，差异只有预期中的 severity 归一化与高亮长度（33→6、8→1，即真正的标记区间而非垃圾值）
- **未验证**：本机没有 verilator，也没有可用的扩展宿主测试装置（`integration/runTest.ts` 需要下载 VS Code），所以运行期行为——哪些触发点各跑一次、诊断落在哪个文件、失败只提示一次、去抖与在途重跑——只做到代码级与类型级确认

**Journey log**

- 移植版的 caret 行长度是 `pline.length - pline.indexOf('^')`，取到的那一行常常不是 caret 行（`indexOf` 返回 -1），于是 33、8 这样的垃圾长度被写进诊断；差分脚本给出了前后对照，测试把「找不到标记就回退为 1」固定下来。
- 复查指出只按 `signal` 判断崩溃不够：Windows 的访问违例是「数值 code + signal null」，光看信号抓不到。`runFailure()` 因此引入 `0x80000000` 下限，并且只有在解析出至少一条可定位诊断时才承认非零退出是「工具报告了问题」。
- 决定不恢复旧 `LintManager` 里「关闭文档就清诊断」的行为：下一次运行会按已上报文件集合把它清掉，而关掉标签页时立刻抹掉尚未修好的问题反而更糟。
- `lib/runner.ts` 的 `ToggleToolConfig` 看起来正好可用，但继承会把无意义的 `runAtLocation` 设置项带进配置树，且它的 `args` 是单个字符串、与既有数组键不兼容；改为组合 `PathConfigObject`/`ConfigObject` 原语，`runner.ts` 本身（main 上的死代码）不动。
- 三轮独立复审的结论：初版的两个 critical（异常终止被当成成功、运行期没有异常保护）与随后的中等问题都已修掉，判定链最后收敛成一个可测的纯函数；剩下的唯一缺口是本环境无法做的运行期验证。

## [S1] Problem

`custom-feats` 上的 verilator lint 是从另一个项目移植过来的，代码沿用了移植源的结构，跟本仓库 VS Code 客户端的约定不合：

1. 配置基础设施被重新发明。`clients/vscode/src/lib/runner.ts` 里已有为「外部工具 + enabled/path/args」准备的 `ToggleToolConfig`（基于 `PathConfigObject`，带平台默认值、`which` 解析、路径校验），而 `BaseLinter` 用裸 `ConfigObject<string>` 自己造了一份 `path`：没有 PATH 解析（Windows 上 `which` 才能把 `verilator` 解析到 `verilator.exe`）、没有存在性校验。
2. 用户设置项泄漏了内部类名。`slang.lintManager.verilator.*` 里的 `lintManager` 是实现细节，仓库其他设置按功能命名（`slang.formatters`、`slang.inactiveRegions.*`）。
3. 参数语义自相矛盾。schema 声明 `args` 是字符串数组，实现却对每个元素再做 `arg.split(/\s+/)`，于是带空格的参数无法表达；仓库处理同类问题用的是依赖里的 `parseArgsStringToArgv`（`ProjectComponent` 已这样解析 build command）。
4. 双向全局耦合。`LintManager` 反向读全局 `ext.project.topFile`，`ProjectComponent` 又反向调用 `ext.lintManager.verilator.clearAll()` / `lint()`，并直接依赖具体 linter 字段。
5. API 以 `TextDocument` 为中心。verilator 只需要文件路径，诊断解析也只用行列，却逼得管理器 `openTextDocument(topFile)`；`-sv` 判断依赖文档是否在编辑器里打开。
6. 解析逻辑与 `vscode` 强耦合（`FileDiagnostic extends vscode.Diagnostic`、severity 返回 vscode 枚举），无法用仓库现成的 tape 单测覆盖，导致整个功能零测试。
7. 行为缺陷：每次应用诊断前 `clear()` 整张集合；`clear(doc)` 在 top file 模式下直接 return；相对路径按 workspace folder 解析而非本次运行 cwd；caret 行缺失时高亮长度退化为垃圾值；活动编辑器切换不筛语言、无去抖；非零退出码与超时无法区分；失败只进日志。
8. 设置同步缺失。`clients/vscode/CONFIG.md`（由组件树生成）根本没同步这几个新设置。

## [S2] Design

### S2.1 模块结构

```
clients/vscode/src/linter/
  LintManager.ts      组件 `lint`：配置树、触发时机、目标选择、运行编排
  ExternalLinter.ts   抽象基类：外部 linter 的配置、进程执行、诊断落地
  VerilatorLinter.ts  子类：verilator 参数构造、finding → vscode.Diagnostic 转换
  lintOutput.ts       纯模块：finding 类型与按文件分组（相对路径按运行 cwd 解析）
  verilatorOutput.ts  纯模块：解析 verilator 的 stderr
  runFailure.ts       纯模块：判定一次运行是否可用（进程级失败 + 退出码与结果的相容性）
clients/vscode/test/unit/verilatorOutput.test.ts
clients/vscode/test/unit/runFailure.test.ts
```

`BaseLinter.ts` 由 `ExternalLinter.ts` 取代（命名体现「外部工具」与语言服务器自身诊断的区别），`FileDiagnostic`/`LintOutput` 这类胶合接口取消。

职责边界：`LintManager` 决定「何时、对哪个文件跑」；`ExternalLinter` 负责「把工具跑起来、把结果落到编辑器」；子类只提供工具专属的「参数」与「输出解析」。运行环境（cwd、超时、输出上限、诊断集合）由基类统一处理，不含任何 verilator 知识。三个纯模块不 import `vscode`，因此可以用 tape 覆盖。

### S2.2 配置（用户可见）

| 键 | 类型 | 默认 | 说明 |
| --- | --- | --- | --- |
| `slang.lint.enabled` | boolean | `true` | 语言服务器自身诊断的开关。**键名不变**，仍由 middleware 过滤 |
| `slang.lint.verilator.enabled` | boolean | `false` | 启用 verilator 外部 lint（原 `slang.lintManager.verilator.enabled`） |
| `slang.lint.verilator.path` | string | `""` → 平台默认 `verilator` | verilator 可执行文件；空值走 `which` 解析 |
| `slang.lint.verilator.args` | string[] | `[]` | 追加参数，每个元素经 `parseArgsStringToArgv` 解析后展平 |

组件树形态：`SlangExtension.lint: LintManager`（字段名即配置前缀 `slang.lint`），其下 `enabled`（服务器诊断）与 `verilator: VerilatorLinter`。`extension.ts` 中内联的 `LintComponent` 删除。

配置原语复用方式：`path` 用 `PathConfigObject`（`which` 解析、存在性校验、平台默认值、`resolveToolPath`），`enabled`/`args` 用 `ConfigObject`，一并接入组件树，从而 `package.json` 与 `CONFIG.md` 可由既有的 Extdev 生成流程产出（生成顺序 = 字段声明顺序，`slang.lint.*` 因此在 `slang.path` 之前）。**不继承** `lib/runner.ts` 的 `ToggleToolConfig`：它挂在 `Runner.runAtFileLocation` 下，继承会把无意义的设置项带进配置树，且它的 `args` 是单个字符串。`lib/runner.ts` 本身是死代码，本次不动。

`slang.lintManager.*` 被移除。该功能只存在于未发布分支，不提供迁移垫片。

### S2.3 组件与依赖方向

- `ProjectComponent` 新增公开事件 `onDidChangeCompilationSource: vscode.Event<void>`；`compilationSource` 的 7 处赋值收敛到私有 `setCompilationSource()`，只有 `compilationKey()` 变化时才触发事件（命令式 build 在每次保存后重新生成 .f，编译内容没变，不该再触发一轮 lint）。
- `LintManager.watchCompilationSource(source: CompilationSource)`，其中结构接口为 `{ readonly topFile: vscode.Uri | undefined; readonly onDidChangeCompilationSource: vscode.Event<void> }`（定义在 `LintManager.ts`）。`ProjectComponent` 天然满足，无需互相 import。
- 删除 `ProjectComponent` 里对 `ext.lintManager.*` 的两处调用；`LintManager` 不再 import `../extension`。
- 服务器诊断开关：`extension.ts` 的 middleware 读 `this.lint.enabled.getValue()`；诊断清空改由 `this.onConfigUpdated(...)`（`libconfig` 自带助手）处理，替代 `activate()` 里裸的 `vscode.workspace.onDidChangeConfiguration` 块。
- wiring 在 `extension.ts`：`this.lint.watchCompilationSource(this.project)`。子组件在 post-order 中先于根被 activate，因此订阅时 `LintManager` 已持有 `context`。

### S2.4 触发与目标选择

目标文件：`project.topFile ?? 活动 verilog 文档`；两者皆无则跳过本次运行并清空既有诊断（编译目标没了，旧诊断不该继续挂在文件上）。

触发条件（`LintManager` 内集中实现）：

1. 保存文档，且文档语言属于 `AnyVerilogLanguages`；
2. 活动编辑器切换，且切到 verilog 文档，**且当前没有 top file**（有 top file 时切编辑器不改变目标，不重跑）；
3. compilation source 变化（设置/清除 top level、build file、build command）；
4. `slang.lint.*` 配置变化：清空既有诊断、忘记已解析的可执行文件后重跑。

节奏控制：触发后 300ms 去抖合并；运行期间再次触发只置一个 pending 标记，当前运行结束后按最新状态再跑一次，避免进程堆叠与旧结果覆盖新结果。去抖定时器在扩展 dispose 时清掉，避免向已释放的诊断集合写入。

运行环境：`cwd = getWorkspaceFolder() ?? path.dirname(target)`，超时 30s，输出上限 8 MiB（具名常量）；非零退出码本身不代表失败。

### S2.5 诊断落地

- 本次运行的结果按目标文件分组；解析诊断里的相对路径时使用**本次运行的 cwd**（而非 `getWorkspaceFolder()`），与工具进程的工作目录一致；绝对路径直接用。
- `ExternalLinter` 维护「本 linter 上次上报过的文件集合」；每次应用后删除本次未再出现的文件的诊断，而不是整表 `clear()`，避免 A 文件的陈旧诊断被 B 文件的结果连带清掉或残留。
- `clearAll()` 清理集合本身与追踪集合，用于关闭 linter、配置变更、编译目标清空。
- 关闭文档时不主动清理：该文件的诊断在下一轮运行中按上面的规则消失。旧实现里的 `onDidCloseTextDocument` 清理（且只在没有 top file 时生效）不保留——用户刚关掉标签页就抹掉尚未修好的问题，比留到下一轮更糟。

### S2.6 输出解析（`verilatorOutput.ts` / `lintOutput.ts`，纯函数）

```ts
// lintOutput.ts
export type LinterSeverity = 'error' | 'warning' | 'info'
export interface LinterFinding {
  severity: LinterSeverity
  code: string | undefined              // %Warning-WIDTH → 'WIDTH'
  message: string
  location: { file: string; line: number; column: number; length: number } | undefined // line/column 1-based
}
export interface FindingGroup { file: string; findings: LocatedFinding[] }  // file 为绝对路径
export function groupFindingsByFile(findings: LinterFinding[], cwd: string): FindingGroup[]

// verilatorOutput.ts
export function parseVerilatorOutput(stderr: string): LinterFinding[]
```

规则：

- 诊断行匹配 `/^%(\w+)(?:-(\w+))?:\s(.*)$/`；`Error`/`Fatal` → `error`，`Warning` → `warning`，其余 → `info`。
- 消息体先用 `/^(.*):(\d+):(\d+): (.*)$/`、再退到 `/^(.*):(\d+): (.*)$/` 拆出 `file:line[:col]:`，file 部分贪婪匹配以兼容 Windows 盘符（`C:\dir\a.sv:12:5: msg`）；缺列时列取 1。
- 无位置信息（如 `%Error: Cannot open file ...`）→ `location = undefined`，由 vscode 层记日志，不落到编辑器。
- verilator 自身的汇总行（`Exiting due to N error(s)`）不作为 finding。
- 高亮长度取诊断行之后、**下一条 `%` 诊断之前**第一处 `^`/`~` 连续段的长度（形如 `      |              ^~~~~~`）；找不到回退为 1。取到下一条诊断前为止，既容得下诊断与源码片段之间数量不定的 `: ... In instance` 注释行，也不会把下一条诊断的 caret 算到自己头上。取代原先 `indexOf('^')` 得到 `-1` 时算出垃圾长度的写法。
- `groupFindingsByFile` 把相对路径按 cwd 解析为绝对路径并分组，使用 `node:path`，保持可单测。

### S2.7 一次运行是否可用（`runFailure.ts`，纯函数）

```ts
export function runFailure(
  error: ProcessError | null,   // { code?, killed?, signal?, message? }
  timeoutMs: number,
  locatedFindings: number
): string | undefined           // undefined 表示这轮结果可以替换屏幕上的诊断
```

判定顺序：被杀（超时）→ 信号终止 → 字符串 code（启动失败 / 输出撑爆缓冲）/ 无 code → 数值 code ≥ `0x80000000`（Windows 崩溃状态，如 0xC0000005，带 signal null，光看信号抓不到）→ 非零且**一条可定位诊断都没有**。其余（退出码为 0，或非零但至少产出一条可定位诊断）视为工具正常报告。

这样「工具崩溃」与「工具报告了问题」不再混为一谈：不可用的运行既不替换诊断，也通过 `logger.error` 与一次性通知显式暴露；无位置的输出始终记进日志（哪怕这轮被判为不可用），用户仍能看到工具说了什么。

可执行文件的解析失败（`slang.lint.verilator.enabled` 打开但找不到 verilator）在首次需要运行时判定：`logger.warn` + 一次 `showWarningMessage`，并暂停运行直到配置变化，避免每次保存都弹窗、反复 `which`。

### S2.8 参数构造

`['--lint-only', ...(目标是 .sv/.svh ? ['-sv'] : []), ...用户参数解析结果, target]`，目标文件始终在最后。`-sv` 依据扩展名判断（`utils.ts` 的 `isSystemVerilogPath()`，与既有 `isSystemVerilog(langid)` 并列），不再依赖文档是否在编辑器里打开。整个运行体（含可执行文件解析）包在 try/catch 内，任何异常都只影响这一个 linter，并记日志 + 一次提示。

### S2.9 测试

- tape 覆盖 `parseVerilatorOutput` / `groupFindingsByFile`：绝对路径、相对路径、Windows 盘符、带类别警告、无位置消息、汇总行、caret 与 `~~~` 长度、注释行在前的告警、下一条诊断的 caret 不可混用、一次多条诊断、缺 caret 回退、空输出；`runFailure`：退出码 0、非零且有/无定位结果、崩溃状态、字符串 code、超时、信号、无状态。
- 触发、进程执行、诊断落地依赖 `vscode`，与仓库现状一致不做单测；由 `tsc`、`eslint`、`tape` 与代码审查覆盖，运行期行为需要在装有 verilator 的环境里人工确认（见 T7）。

## [S3] Out of Scope

- C++ / Neovim 客户端：外部 linter 只存在于 VS Code 客户端，不动服务器与 nvim 插件。
- 不新增 linter 工具（iverilog、slang 自带 lint 等），不改成 `slang.formatters` 那样的列表式多工具配置。
- 不支持多根工作区，沿用 `getWorkspaceFolder()` 的既有约定。
- 不删除、不改造 `lib/runner.ts`（main 上就是死代码）。
- 不做 verilator 文件列表（`-f`）/ build file 集成：目标仍是单个文件。
- 不处理 `.bat`/`.cmd` 形式的 verilator 包装脚本（`execFile` 不经 shell）。
- 工具把诊断写到 stdout 的情形（verilator 不这样）不特殊对待。
- 不修 `extension.ts:270` 的 `no-misused-promises`：它在 HEAD 上就存在，属 `slang/internalError` 通知处理，不是本次范围。

## Tasks

- [x] T1: 抽出纯解析模块并补单测 — 新增 `verilatorOutput.ts`/`lintOutput.ts` 与 `test/unit/verilatorOutput.test.ts`；验收：`tape` 通过，覆盖 S2.6 与 S2.9 列出的输出形态 (covers: S2.6, S2.9)
- [x] T2: 重构配置与组件结构 — `ExternalLinter` 基类（`PathConfigObject` path + `ConfigObject` enabled/args，参数经 `parseArgsStringToArgv` 展平）、`VerilatorLinter` 子类、`LintManager` 作为 `lint` 节点，删除内联 `LintComponent`；验收：键为 `slang.lint.enabled` 与 `slang.lint.verilator.{enabled,path,args}`，`CONFIG.md` 同生成形态，`tsc`/`eslint` 通过 (covers: S2.1, S2.2, S2.8)
- [x] T3: 解开全局耦合、收敛触发逻辑 — `ProjectComponent` 增加 `onDidChangeCompilationSource` 与 `compilationKey`，`LintManager.watchCompilationSource()` + 去抖 + 在途重跑，删除 `ext` 双向引用；验收：`linter/` 下无 `../extension` import、`ProjectComponent` 无 `lintManager` 引用（grep 验证） (covers: S2.3, S2.4; depends: T2)
- [x] T4: 修正诊断落地与失败路径 — 相对路径按运行 cwd 解析、按文件增量清理、`-sv` 依据扩展名、`runFailure` 判定与失败可见性；验收：`tape`/`tsc`/`eslint` 通过，判定逻辑有单测覆盖 (covers: S2.5, S2.7; depends: T2)
- [x] T5: 同步文档并全量验证 — 更新 `clients/vscode/CONFIG.md` 与 `docs/features/features.md`；验收：文档与实际生成形态一致，`tsc`、`tape` 绿，`eslint` 仅剩预存在的 `extension.ts:270` (covers: S2.2)
- [ ] T7: 在装有 verilator 的环境人工确认运行期行为（触发点各跑一次、诊断落点、无 verilator 时只提示一次、去抖与在途重跑）— 本机无 verilator 与可用扩展宿主装置，属环境阻塞 (depends: T2, T4)
