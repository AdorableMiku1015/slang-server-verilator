---
feature: hover-doc-comments
status: designed
updated: 2026-09-28
branch: feat/hover-doc-comments
commits:
---

# hover-doc-comments

## Report

## [S1] Problem

hover 只显示声明**上方独占一行**的注释块，同一条语句**行尾**的注释永远不会出现。

实测（`tests/cpp` 探针，源码见下）当前行为：

| 源码 | 当前 hover 注释段 |
| --- | --- |
| `/// doc for a` + `logic a; // trailing a` | `doc for a`（丢掉 `trailing a`） |
| `logic b; // trailing b only` | 空（丢掉 `trailing b only`） |
| `/** block doc */` + `logic c; /* trailing c */` | `block doc for c`（丢掉 `trailing c`） |
| `/// group` + `logic a; // n1` + `logic b; // n2` | hover `b` 时为空 |

第三条正是常见的组织方式：一条独占一行的注释说明整组声明的含义，随后每行声明各自带行尾注释说明自己。此时只有第一条声明能看到组注释，第二条及之后的声明**既看不到组注释，也看不到自己的行尾注释**。

根因：SystemVerilog 的注释是 slang 的 trivia，而 trivia 只作为 leading trivia 挂在**下一个 token** 上，token 本身没有 trailing trivia。`getDocCommentForHover` 只看声明首个 token 的 leading trivia，并且 `findLeadingDocCommentStart` 的设计要求「注释块后面必须还有一个换行」，于是：

- 上方独占一行的注释块 → 在首个 token 的 leading trivia 里，且后面跟着换行 → 能取到。
- 本行行尾注释 → 其实在**下一个 token** 的 leading trivia 里，且位于该 trivia 的第一个换行**之后**（因为换行才是本行的结束） → 取不到。
- 组头注释 → 在**第一条声明**的 leading trivia 里，对第二条声明完全不可见（第二条声明的 trivia 里只有第一条声明的行尾注释）。

## [S2] Design

改动集中在 `src/util/Formatting.cpp` 的 `getDocCommentForHover`，公开签名不变（不需要 `SourceManager`，全部信息都在语法树里），因此 `DefinitionInfo::appendSyntaxTargets` 及三个调用点都不用改。新行为对「hover 声明处」和「hover 引用处」同时生效——两者渲染的是同一个声明语法节点。

### S2.1 注释段的组成

hover 的注释段按顺序由两部分拼成**一段**（沿用现有的逐行渲染，每行以两个空格 + 换行结束）：

1. **头部注释块**：声明自己的上方注释块；若没有，则退化为它所属声明组的组头注释块（S2.3）。
2. **行尾注释**：声明所在行的行尾注释。

两部分都为空时不输出注释段（保持现状：`appendSyntaxTargets` 里 `docComments.empty()` 就不加段落）。

### S2.2 行尾注释的取法

「声明的行尾注释 = 声明自己的 token 之后、到本行换行为止」这段 trivia 里的注释。

从 `node` 的 token 之后按源码顺序遍历后续 token，逐个检查它的 leading trivia：

- trivia 里出现 `EndOfLine` → 本行结束，停止（最常见的情形就是这样：下一条声明在下一行，其 trivia 的第一个元素就是换行）。
- `Directive` trivia（preprocessor 指令）→ 本行结束，但它**自带换行**，上一行末尾的注释是挂在**指令自己的首个 token** 上的，所以要先取 `trivia.syntax()->getFirstToken().trivia()` 里同一行的注释，再停止。
- `DisabledText` 等其它 kind 一律视为行结束（保守处理）。
- 否则收下其中的 `LineComment` / `BlockComment`，继续看下一个 token。

之所以要「继续看下一个 token」而不是只看紧跟的那一个，是因为注释前面可能还隔着别的东西：

- 模块体里的 `localparam` / `parameter` 在 slang 里被包成 `ParameterDeclarationStatement`，hover 渲染的是里面的 `ParameterDeclaration`，`;` 落在包装节点上；
- 端口声明后面跟着列表分隔符 `,`。

两者都实测过：只看紧邻 token 时这两种形状取不到行尾注释。多行 `/* */` 只要求它**起始于**本行，起始行之后的续行照常渲染。

扫描的起点是**被渲染的那个节点**（`selectDisplayNode` 的结果），不是 S2.3 里的包装元素：模块/函数/类的 hover 渲染的是头部，其行尾注释就挂在头部那一行的行尾，若从外层声明节点扫起就会越过这一行。跨多行的声明因此取的是「渲染出来的语法结束的那一行」的行尾注释。

全程不扫描源码文本——两个 token 之间的 trivia 按定义只有空白与注释，所以字符串字面量里的 `//` 不会被当成注释（有回归测试固定这一点；指令那一支也只读它自己的 leading trivia）。

### S2.3 组头注释的继承

「组头注释 = 连续一组同级成员声明**最上方**那个独占一行的注释块」，因此第二条及之后的声明也能看到它。

从 `getDeclarationElement(node)` 出发向上走——即先取「首个 token 与该节点相同的、最外层的祖先」，但**不爬到文件根节点**（根没有父节点也没有同级，爬上去会让后续遍历直接空转）。这样模块体里的 `localparam` 会先归位到 `ParameterDeclarationStatement`，它才是真正排在模块成员列表里的那一项，组头继承因此对参数声明同样生效。随后：

1. 该元素自己有上方注释块 → 用它，结束。
2. 否则取「同一父节点下、紧邻的上一个成员声明」 `P`：
   - `P` 不存在 → 没有组头注释。
   - `P` 与当前节点之间有空行或不在相邻两行（即当前节点首 token 的 leading trivia 里 `EndOfLine` 数量不为 1）→ 没有组头注释（空行切断分组）。
   - `P` 的 kind 不是 `syntax::MemberSyntax`（例如模块头的 `ModuleHeader`、列表里的分隔 token）→ 没有组头注释。
   - `P` 自己有上方注释块 → 它就是组头注释，结束。
   - 否则令当前节点 = `P` 继续向上。

约束到成员声明（`MemberSyntax`）是为了不越出声明组：模块体第一个成员的「上一个同级」是模块头，若不加这条判断，文件顶部的版权注释会在第一个成员上被当成组头注释；同理也不会跨进过程块里的语句。

**已知取舍**：语法上「独占注释 + 一组声明」与「只给第一条声明写的文档注释」不可区分，因此本规则会把上方注释块共享给整组（用户已确认选择不带闸的继承）。仓库自带测试数据里两种情况都能看到，golden 的差异就是实证：

```systemverilog
// tests/data/repo1/cycle_test.sv —— 目标用法
// Use types from base_pkg (which exports util_pkg types)
base_pkg::config_t system_config;
base_pkg::result_t operation_result;  // This should resolve via export
base_pkg::data_width_t bus_width;
```
`operation_result`、`bus_width` 的 hover 现在都会带出组头注释，`operation_result` 另外带出自己的行尾注释。

```systemverilog
// tests/data/hdl_test.sv:91 —— 不带闸的代价
    input logic rst,
    // some useful info
    input logic          [test_pkg::WIDTH-1:0] width_port,
    input test_pkg::id_t [test_pkg::WIDTH-1:0] id_array,
```
这条注释原本像是只写给 `width_port`，现在同组的后续端口 hover 也会带上它。想让注释只属于单条声明，就在它与下一条之间留一个空行。

### S2.4 渲染

复用现有的逐行渲染：`LineComment` 去掉 `///` / `//` 前缀，`BlockComment` 去掉 `/* */` 与每行前导 `*`；`plaintext` 模式逐行转义 markdown，`markdown` 模式原样输出。因此新旧注释的行为完全一致，`docCommentFormat` 的语义不变。

### S2.5 边界

- `raw` 模式不变：它输出节点源码原文（`formatCodeWithLeadingComments`），本次不把行尾注释、组头注释拼进代码块。
- 不新增配置项：这是注释**收集范围**的扩展，不是渲染方式的改变，`hovers.docCommentFormat` 不动，也无需同步 config schema / `config.gen.ts`。
- `getDocCommentForHover` 的调用点、hover 的段落结构（注释段 / `---` / 代码块）保持不变。

## [S3] Out of Scope

- `raw` 模式下的行尾注释与组头注释（见 S2.5）。
- 其他特性（completion、code lens、文档符号等）的注释展示——它们不走 `getDocCommentForHover`。
- 过程块语句、结构体/枚举成员的组头继承（S2.3 已限制在成员声明；端口声明与 `ParameterDeclarationStatement` 本身是成员，参数与端口都在覆盖范围内）。
- **hover 的 `Driven by` 段（驱动语句）**：`DefinitionInfo.cpp` 走 `renderCode(paragraph, sm, /*rawDocComments=*/true)`，把驱动语句连同 leading 注释原样渲染进代码块，不经过注释段逻辑，所以驱动语句的行尾注释不显示（保持现状）。
- 通过配置开关关闭新行为。

实测覆盖（都有回归用例）：宏展开出来的声明（`` `DECL(mac); // mac note ``）能取到行尾注释；声明后面紧跟 `` `ifdef `` 时不会丢注释；`` `define Z 1 // width `` 那一行的注释不会被算到上一行声明头上；文件首个 token 属于某条声明时（例如文件以 `typedef` 开头）依然取得到行尾注释；模块/函数/类头部那一行的行尾注释会显示。

## Tasks

- [ ] T1: 抽出注释渲染与 trivia 扫描的公共辅助（`appendCommentLine` / 首个换行前注释的抽取 / 后继 token 查找 / 上一个成员声明查找），`getDocCommentForHover` 行为不变 — 验收：全量 `server_unittests` 仍全绿（covers: S2.4）
- [ ] T2: 行尾注释合并进注释段 — 验收：`logic a; // trailing a`（含上方注释、仅有行尾注释、`/* */` 行尾三种写法）hover 都带出行尾注释（covers: S2.1, S2.2, S2.4；depends: T1）
- [ ] T3: 组头注释继承 — 验收：组内第 2、3 条声明 hover 显示组头注释 + 自己的行尾注释；空行分隔、模块头、跨类型非成员节点都不会误继承（covers: S2.3；depends: T1, T2）
- [ ] T4: 回归测试 — 验收：`tests/cpp/HoverTests.cpp` 新增用例覆盖 T2/T3 的正例与边界（空行、宏展开声明、`plaintext` 模式、`raw` 模式不变），`--update` 后无 golden 意外变动（covers: S2.2, S2.3, S2.5；depends: T2, T3）
- [ ] T5: 构建 + 全量测试 — 验收：`cmake --build build/win64-release --target server_unittests && build/win64-release/bin/server_unittests.exe` 全绿（covers: S2.2, S2.3, S2.5）
