---
feature: vscode-var-expansion
status: delivered
updated: 2026-09-30
branch: feat/vscode-var-expansion
commits: cf5dfad..cdd2b6f
---

# vscode-var-expansion

## Report

**What was built** — `slang.lint.verilator.args` 的每个元素现在先经 `parseArgsStringToArgv` 切分、再逐个展开 VS Code 变量（上表那 16 个非交互变量），最后才拼上内建参数与目标文件路径；切分在前，所以含空格的值始终是一个 argv 元素。展开只作用于用户在设置里写的文本，`--lint-only`/`-sv` 与末尾的目标路径不参与。取不到值的变量（名字不认识、上下文缺失、环境变量不是字符串、设置项非标量）原样保留在参数里，并记一条 `logger.warn`（同名每次配置变化只记一次、不弹窗），由工具自己去报错。

`LintManager` 把一次运行锚定到目标文件所在的工作区文件夹：cwd、`${cwd}`、`${workspaceFolder}` 从此是同一个值，多根工作区下 cwd 从「第一个文件夹」改为目标所在文件夹（verilator 打印的相对路径也因此归到同一个根）；窗口没有打开任何文件夹时 cwd 仍是目标所在目录，而 `workspaceFolder` 独立传成 `undefined`，`${workspaceFolder}` 不会被错展开成那个目录。`ExternalLinter` 的 args 描述、`clients/vscode/package.json`、`clients/vscode/CONFIG.md` 三处逐字同步，`docs/features/features.md` 补了变量表、`-I${workspaceFolder}/rtl/inc` 示例与多根/无文件夹的语义，未新增配置键。

**Verification**

- `npx tsc -p . --outDir out`（clients/vscode）：exit 0 — PASS
- `npx tape "out/test/**/*.js"`：276/276 断言 — PASS（改动前基线 169；新增 `variableExpansion.test.ts` 107 条，其中 11 条来自复审后的补测）
- `npx eslint src/linter/variableExpansion.ts src/linter/ExternalLinter.ts src/linter/LintManager.ts`：exit 0 — PASS
- `npx eslint src --ext ts`：exit 0 — PASS。改动前这条命令（也正是 `.github/workflows/vscode-ci.yml` 对 `clients/vscode/**` 跑的 `pnpm lint:ts`）在 `src/extension.ts:270` 有一条 `@typescript-eslint/no-misused-promises`：由 `ca5f1ee` 引入、本特性未改动该文件，属 `PRE-EXISTING(eslint-extension-270)`；按用户要求在同一分支上补修（`async` 通知处理器改为调用具名方法 `reportInternalError`）。这一处修复不在上文复审范围 `cf5dfad..cdd2b6f` 内
- `prettier --check`（本机 pnpm store 里的 3.9.8；hook 钉的是 mirrors-prettier v4.0.0-alpha.8，故为近似）：改动文件全部符合 — PASS
- 真实 verilator 核对（`C:\verilator\verilator_bin.exe`，rev v5.052-271-g0572ed9f5）：9/9 检查通过。含空格路径下 `-I${workspaceFolder}/inc` 展开成单个绝对参数并命中 include（退出码 0、stderr 无 `%Error`）；`-f${env:SLANG_VAR_UNSET}` 原样到达命令行、被报为 unresolved，verilator 自己打印 `%Error: Invalid option: -f${env:SLANG_VAR_UNSET}`。脚本为一次性（已删）；执行时用文件描述符而非管道接子进程输出
- 独立复审两轮：首轮无 critical，报出我新代码的 2 处边界问题（`${env:toString}` 顺原型链被展开、`..foo.sv` 被误判为文件夹之外）、2 处覆盖缺口、1 处注释与文档不符，全部修掉；第二轮确认 4 项均已修复，只剩文档/注释级问题（S2.4 签名草图、T4 验收措辞、`lint()` 契约注释、`features.md` 换行），均已改。收尾这一轮只动注释与文档，用 tsc/tape/eslint/prettier 加逐条比对确认，未再派第三次复审
- **未验证**：扩展宿主内的端到端行为（保存/切换编辑器/设置变化触发、同名变量只记一次 warn、多根工作区下 cwd 实际生效、诊断落点）——本机没有可运行的扩展宿主装置（`integration/runTest.ts` 需要下载 VS Code），与 `verilator-lint-refactor` 的结论一致

**Journey log**

- 展开必须放在 `parseArgsStringToArgv` **之后**按参数进行：`C:\Users\My Name\...` 这类值若先展开再切分，`-I${workspaceFolder}/inc` 会裂成两个 argv 元素；真机核对里那个带空格的工作区用例就是固定这件事的。
- `${workspaceFolder}` 与 cwd 必须同时锚定：只改其中一个，同一个 run 里「相对路径相对谁」就会有两个答案；由此还发现无工作区窗口要把 `workspaceFolder` 独立于 cwd 传下去，否则 `${workspaceFolder}` 会被错展开成目标文件所在目录。
- 环境对象是普通对象：`ctx.env?.[name] !== undefined` 会把 `toString`/`constructor`/`__proto__` 展开成函数与对象源码；只认字符串既符合「未设置就原样保留」，也不会让非字符串顺着 `{ value: string }` 的类型撒谎。
- `relative.startsWith('..')` 是错的判据（`..foo.sv` 是文件夹内的合法文件名），正确判据是 `path.isAbsolute(relative) || relative === '..' || relative.startsWith('..' + path.sep)`，其中第一项负责跨盘与 UNC。`clients/vscode/src/sidebar/BuildConfigUtils.ts:76` 有同一形状的启发式，属另一模块，本次未动。
- 本机沙箱禁止 Node 子进程的管道 stdio（`execFile` 直接 `EPERM`）：真实 verilator 核对改成把子进程 stdout/stderr 绑到文件描述符再读文件，否则最有力的这条证据根本跑不出来。

## [S1] Problem

`slang.lint.verilator.args` 的每个元素只经 `parseArgsStringToArgv` 展平，然后原样拼进命令行：

```ts
// clients/vscode/src/linter/ExternalLinter.ts:171
private configuredArgs(): string[] {
  return this.args.getValue().flatMap((arg) => parseArgsStringToArgv(arg))
}
```

用户按 VS Code `tasks.json` / `launch.json` 的习惯写下的 `${workspaceFolder}` 不会被展开，verilator 收到的是字面量：

```jsonc
"slang.lint.verilator.args": ["-I${workspaceFolder}/rtl/inc", "--top-module top"]
// verilator 实际收到：-I${workspaceFolder}/rtl/inc
// → %Error: ... Can't find file '${workspaceFolder}/rtl/inc'
```

今天能「凑出」的只有一部分场景：lint 进程的 cwd 已经是工作区文件夹（`docs/features/features.md`、`LintManager.lintTarget()`），所以 `-Irtl/inc` 这种相对写法可用，但它们都有代价或覆盖不到：

| 写法 | 现状 | 问题 |
| --- | --- | --- |
| `-I${workspaceFolder}/rtl/inc` | 字面量传给 verilator | 静默失效（只在 verilator 的报错里看出来） |
| `-Irtl/inc` | 可用（相对 cwd） | 依赖 cwd；表达式只能表达「相对工作区根」，写不出绝对路径 |
| `${env:VERILATOR_ROOT}/include` | 字面量传给 verilator | 工具链位置没有相对写法 |
| `${userHome}/.verilator/...` | 字面量传给 verilator | 同上 |
| `${fileDirname}` 及其它文件变量 | 字面量传给 verilator | 没有相对写法 |
| 多根工作区 | cwd 恒为**第一个**文件夹 | 目标文件在第二个文件夹时，相对路径指向错的根 |

变量展开是 VS Code 对「用户写的命令行」的既有约定，客户端里唯一处理 verilator 参数的地方却没有它，于是同一段文本在 `.vscode/tasks.json` 里能用、在设置里不能用。

## [S2] Design

### S2.1 变量表与取值

只支持 VS Code 文档中**非交互**的那部分变量（用户已确认），取值全部锚定在**本次 lint 运行**上：

| 变量 | 展开为 | 取不到值时 |
| --- | --- | --- |
| `${workspaceFolder}` | 本次运行所属的工作区文件夹绝对路径（= cwd，见 S2.3） | 原样保留 |
| `${workspaceFolderBasename}` | 上一项的末段 | 原样保留 |
| `${fileWorkspaceFolder}` | 目标文件所属的工作区文件夹。因为运行就锚定在目标文件的文件夹上，它与 `${workspaceFolder}` 恒等（保留此名是为了让 `tasks.json` 里已有的写法直接可用） | 原样保留 |
| `${file}` | 本次 lint 的目标文件绝对路径（设了 top level 就是它，否则是当前编辑器文件） | 原样保留 |
| `${fileDirname}` | `${file}` 的目录 | 同上 |
| `${fileBasename}` | `${file}` 的文件名（含扩展名） | 同上 |
| `${fileBasenameNoExtension}` | 文件名去扩展名 | 同上 |
| `${fileExtname}` | 扩展名（含 `.`） | 同上 |
| `${relativeFile}` | `${file}` 相对 `${workspaceFolder}`，分隔符统一为 `/` | 文件不在工作区文件夹之下时原样保留 |
| `${relativeFileDirname}` | 上一项的目录部分 | 同上 |
| `${cwd}` | 本次运行的进程 cwd（= `${workspaceFolder}`） | 原样保留 |
| `${pathSeparator}`、`${/}` | 本平台路径分隔符 | 不会取不到 |
| `${userHome}` | 用户主目录（`os.homedir()`） | 不会取不到 |
| `${env:NAME}` | `process.env[NAME]` | 值不是字符串（未设置，或只从原型链上取到）时原样保留 |
| `${config:ID}` | VS Code 设置项 `ID` 的值（string/number/boolean 转字符串） | 设置项不存在、值为 `undefined` 或非标量时原样保留 |

**「取不到值」的判定**：变量名不在上表内，或该变量所需的上下文缺失（无工作区文件夹、无目标文件、文件不在工作区文件夹下、环境变量未设置、设置项不存在/非标量）。

两条边界由复审补上并已修：`${env:NAME}` 只认**值为字符串**的项——环境对象是普通对象，`${env:toString}` 这类只从原型链上取到的名字不算用户设置的环境变量；`${relativeFile}` / `${relativeFileDirname}` 只把 `..` 与 `..<分隔符>` 当作「在文件夹之外」（跨盘、UNC 目标算出来的相对路径本身是绝对路径，仍由 `path.isAbsolute` 拦下），名字以两个点开头的文件（`..foo.sv`）仍在文件夹内。

**「算出来是空串」不算取不到值**：`${env:EMPTY}`（变量已设为空串）、`${relativeFileDirname}`（文件就在工作区根下）都按空串替换，不记警告。空串替换后参数可能只剩下前缀（如 `-I`），这是用户自己写的表达式算出来的结果。

**退化行为**（用户已确认）：取不到值一律**原样保留**在参数里，同时 `logger.warn` 记一条，说明是哪个设置项、哪个变量、为什么没展开。lint 会在每次保存时跑，所以只记日志、不弹窗；**同一个变量名在一次配置变化内只记一次**（与 `notified` 同样在 `onSettingsChanged()` 里清空）。原样保留意味着 verilator 会自己对这个参数报错，用户能看见，而不是参数被悄悄吃掉或改写成别的东西。

不支持的变量（`${lineNumber}`、`${selectedText}`、`${execPath}`、`${command:...}`、`${input:...}`）走同一条退化路径：名字不在表内 → 原样保留 + warn。自动触发的 lint 不应该弹 QuickPick/输入框，也不应该依赖光标状态。

### S2.2 展开时机：按「切分后的单个参数」展开

顺序固定为 **先 `parseArgsStringToArgv` 展平 → 再逐个参数展开 → 最后拼上目标文件路径**。

理由：变量的值里可能有空格（`C:\Users\My Name\...`、`${env:PROGRAMFILES}`）。若先对整串展开再切分，`-I${workspaceFolder}/inc` 会裂成两个参数，而按参数展开后一个含空格的路径仍是一个 argv 元素，交给 `execFile` 时不需要引号、也不会被 shell 二次解析（`execFile` 不经 shell）。

`toolArgs()` 产出的内建参数（`--lint-only`、`-sv`）与末尾拼上的目标文件路径都是客户端自己算出来的，不参与展开——只有用户在设置里写的文本参与。

### S2.3 cwd 与 `${workspaceFolder}` 的锚定

现在 `LintManager.lintTarget()` 把 cwd 定为 `getWorkspaceFolder() ?? path.dirname(target.fsPath)`，而 `getWorkspaceFolder()` 是 `workspaceFolders[0]`：多根工作区里永远是第一个文件夹。改成锚定**目标文件所在的工作区文件夹**：

```ts
const folder = vscode.workspace.getWorkspaceFolder(target)?.uri.fsPath ?? getWorkspaceFolder()
const cwd = folder ?? path.dirname(target.fsPath)
```

于是 `${workspaceFolder}`、`${cwd}`、lint 进程的 cwd 三者是同一个值，`${fileWorkspaceFolder}` 也等于它；`workspaceFolder` 作为**独立于 cwd 的值**传给 linter（单文件模式没有工作区文件夹时它必须是 `undefined`，否则 `${workspaceFolder}` 会被错误地展开成目标文件所在目录）。

**已知行为变化**（用户在选择变量表时已确认）：多根工作区里 cwd 从「第一个文件夹」变成「目标文件所在文件夹」。这一并修正了两处：`-I<相对路径>` 相对的是目标所在根；`groupFindingsByFile(findings, cwd)` 解析 verilator 打印的相对路径时也以同一个根为准（verilator 的相对路径本来就是相对它自己的 cwd 打印的）。单文件夹工作区、单文件模式（无工作区文件夹）下 cwd 与今天完全一致。

### S2.4 代码落点

- 新增纯模块 `clients/vscode/src/linter/variableExpansion.ts`，**不 import `vscode`**（与 `lintOutput.ts` / `verilatorOutput.ts` / `runFailure.ts` 一样，可被 tape 直接覆盖）：

  ```ts
  export interface ExpansionContext {
    workspaceFolder?: string
    file?: string
    cwd?: string
    env?: Record<string, string | undefined>
    config?: (id: string) => string | undefined
  }

  export interface UnresolvedVariable {
    /// 用户写下的原文，例如 `${env:INC}`
    name: string
    /// 没有值的原因，供调用方写进日志
    reason: string
  }

  export interface ExpansionResult {
    /// 每个能取到值的变量都已替换过的文本
    text: string
    /// 原样留下、需要报出来的变量，按出现顺序
    unresolved: UnresolvedVariable[]
  }

  /// 展开已知的 ${...}；认不出或取不到值的原样留下，并在 unresolved 里报出名字与原因
  export function expandVariables(text: string, ctx: ExpansionContext): ExpansionResult
  ```

- `ExternalLinter`：`configuredArgs(ctx: ExpansionContext)` 接收上下文，展开后把 `unresolved` 逐个记 warn（同名一次）；新增 `private warnedUnresolved = new Set<string>()`，在 `onSettingsChanged()` 里随 `notified` 一起清空。上下文在 `lint()` 里组装：`file` = `target.fsPath`、`cwd`/`workspaceFolder` 来自参数、`env` = `process.env`、`config` 用 `vscode.workspace.getConfiguration().get(id)` 适配（string/number/boolean → `String(value)`，其它 → `undefined`）。`lint()` 签名加上工作区文件夹：`lint(target, cwd, workspaceFolder?: string)`。`${config:ID}` 读的是不带资源的设置查找结果（与 `ConfigObject.getValue()` 同一条路径），逐文件夹的资源级取值不在范围内。
- `LintManager.lintTarget()`：按 S2.3 算出 folder 与 cwd，并把 folder 传给每个 linter。
- `VerilatorLinter` 不改动：它只提供工具参数与输出解析，展开是基类的事。
- `warn` 文案形如：`slang.lint.verilator.args: ${env:INC} was not expanded because the environment variable INC is not set`——带上设置项路径与原因，否则用户不知道去改哪里、为什么没生效。

### S2.5 配置与文档同步

- `ExternalLinter` 构造里 `args` 的 description 追加一句：变量（如 `${workspaceFolder}`、`${env:VAR}`）会被展开，取不到值的原样传给工具。同一句话要同步到 `clients/vscode/package.json`（`slang.lint.verilator.args`）与 `clients/vscode/CONFIG.md`（Extdev 生成器产出的两处，手工保持一致，措辞逐字相同）。
- `docs/features/features.md` 的 External Linting 段补一句变量支持，并给一个 `-I${workspaceFolder}/rtl/inc` 的例子；同时把「按切分后的单个参数展开」（S2.2）写清楚，因为它是「路径里有空格」这类问题的答案。
- **不新增配置项**：展开是对参数写法的解释，不是行为开关；`package.json` / `config.schema.json` / `config.gen.ts` 的键与默认值都不动。

### S2.6 测试边界

tape 覆盖纯模块（`clients/vscode/test/unit/variableExpansion.test.ts`）：表中每个变量的展开值；`${/}`；含空格的路径展开后仍是**一个**参数（S2.2）；一个参数里多个变量、同一变量出现两次；`${env:}` 已设/未设/设为空串；`${config:}` 回调有值/返回 `undefined`；未知名与不支持的变量（`${command:x}`、`${lineNumber}`）进 `unresolved` 且文本原样；没有 `workspaceFolder`、没有 `file`、`file` 不在 `workspaceFolder` 之下时 `relativeFile`/`relativeFileDirname` 进 `unresolved`；文件就在工作区根下时 `${relativeFileDirname}` 展开成空串而**不**进 `unresolved`；文本里没有 `${` 时原样返回且 `unresolved` 为空；verilator 自己的参数形态（`+define+X=${Y}`、`--prefix`）不被误改。

`LintManager` / `ExternalLinter` 依赖 `vscode`，按仓库现状不做单测；cwd 锚定（S2.3）与 warn 只记一次的行为在扩展宿主内确认（本机没有可运行的扩展宿主装置，见 Report），但参数构造与真实 verilator 的交互用复刻脚本核对（T4）。

## [S3] Out of Scope

- **其他配置项**：`slang.lint.verilator.path`、`slang.formatters[].command`、`slang.builds[].command`（已有自己的相对路径解析）、`slang.args` / `slang.debugArgs` 都不展开——用户已确认只做 verilator 的 args。纯模块与上下文接口可复用，将来要扩只需接上上下文。
- **交互式与编辑器状态变量**：`${command:...}`、`${input:...}`、`${lineNumber}`、`${selectedText}`、`${execPath}` 不实现（S2.1 的退化路径会原样保留 + warn）。
- **递归展开**：`${config:ID}` 的值里再出现 `${...}` 不再展开；`$$` 转义、`${env:${...}}` 这类嵌套也不支持。
- **不新增开关**，不给取不到值的变量弹窗，不改 `toolArgs()` 的内建参数与末尾的目标文件路径。
- verilator 之外的 linter 没有实现，本次不为它预留配置（`ExternalLinter` 是基类，行为自动共享）。

## Tasks

- [x] T1: 新增纯展开模块 `variableExpansion.ts` 与 `test/unit/variableExpansion.test.ts` — 验收：`pnpm build-tests && npx tape "out/test/**/*.js"` 全绿，用例覆盖 S2.1 全表、S2.2 的按参数展开、S2.6 列出的退化与边界 (covers: S2.1, S2.2, S2.6)
- [x] T2: 接入 verilator args — `ExternalLinter` 组装上下文、展开、每变量一次 warn（`onSettingsChanged()` 清空），`lint()` 增加工作区文件夹参数；`LintManager` 按 S2.3 锚定 cwd 与 folder — 验收：`npx tsc --noEmit` 通过、改动文件 `eslint` 干净（整仓 `pnpm lint:ts` 另有既有的 `src/extension.ts:270` `no-misused-promises` 报错，记为 `PRE-EXISTING(eslint-extension-270)`，本分支未改动该文件），`slang.lint.verilator.args` 里的 `${workspaceFolder}` 出现在 `execFile` 的实参里（由 T4 核对） (covers: S2.3, S2.4; depends: T1)
- [x] T3: 同步配置文本与功能文档 — `ExternalLinter` 的 args 描述、`clients/vscode/package.json`、`clients/vscode/CONFIG.md` 三处逐字一致，`docs/features/features.md` 补变量说明与 `-I${workspaceFolder}/rtl/inc` 例子 — 验收：四处文本相互一致，且没有新增配置键（`config.schema.json` / `config.gen.ts` 无变化） (covers: S2.5; depends: T2)
- [x] T4: 构建、全量单测与真实 verilator 核对 — 验收：`pnpm build-tests`、`npx tape "out/test/**/*.js"`、`npx tsc --noEmit` 全绿，改动文件 `eslint` 干净（整仓 lint 的既有报错与 T2 同一条，记为 `PRE-EXISTING(eslint-extension-270)`）；用 `C:\verilator\verilator_bin.exe`（v5.052-271-g0572ed9f5）复刻 `configuredArgs` + `execFile` 的运行路径，确认含空格的 `${workspaceFolder}` 展开后 `-I<dir>` 能命中 include，且未设置的 `${env:...}` 确实以字面量到达命令行（verilator 对其报错） (covers: S2.1, S2.2, S2.4; depends: T2)
