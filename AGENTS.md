# AGENTS.md

SystemVerilog LSP server in C++20, built on the [Slang](https://github.com/MikePopoloski/slang) library.
Also includes a VS Code extension (`clients/vscode/`, TypeScript) and Neovim plugin (`clients/neovim/`, Lua).

## Build

```bash
cmake -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=1 -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j8
```

Or use a preset from `CMakePresets.json` (e.g. `cmake --preset clang-debug`, `cmake --preset win64-debug`).
CI uses `--preset <name> -DSLANG_CI_BUILD=ON`.

Key targets:
- `slang_server` — the server binary (`build/bin/slang-server`)
- `server_unittests` — primary C++ tests
- `gen_config_schema` — config JSON schema generator

## Test

**Preferred: build + run server_unittests directly:**
```bash
cmake --build build -j8 --target server_unittests && build/bin/server_unittests
```

**Update golden files** when test output intentionally changes:
```bash
build/bin/server_unittests --update
```
Goldens live in `tests/cpp/golden/*.json` and `tests/cpp/golden/*.out`. They are excluded from formatting hooks.

**CTest** (runs all discovered tests):
```bash
ctest --test-dir build --output-on-failure
```

**Python system tests** (pygls-based, require built server binary):
```bash
uv venv && source .venv/bin/activate && uv sync
pytest tests/system/ --binary build/bin/slang-server
```

**Neovim tests** require luarocks + busted (see `DEVELOPING.md` for setup).

## Sandboxed Windows Environments

Notes for running the build and tests inside a sandboxed agent session (DSH) on Windows. The
symptoms below are confusing the first time: the test suite can pass while every compile, link or
`git` write fails.

**Test temp directory.** `server_unittests` creates its workspaces under `%TEMP%`. When the test
process may not write there, point both variables at a directory inside the repo:

```powershell
$env:TMP="$PWD\tmp-unittests"; $env:TEMP=$env:TMP; $env:GIT_CEILING_DIRECTORIES=$PWD
build/win64-release/bin/server_unittests.exe
```

- Do not put that directory under `build/`: the auto-configure tests protect directory *names* that
  appear in the workspace root's ancestor chain, so a `build` ancestor turns into false failures.
- `GIT_CEILING_DIRECTORIES` stops the indexer from treating those temp workspaces as part of this
  git repository (it runs `git ls-files` and `git check-ignore` inside them).

**Directory symlinks on Windows** need Developer Mode or an elevated token. Tests that use one as a
fixture go through `REQUIRE_DIRECTORY_SYMLINK(target, link)` in `tests/cpp/utils/Symlinks.h`, which
skips the test instead of failing. Use that helper for new cases, and do not `SKIP` inside a
`SECTION`: Catch2 runs the test body once more without sections after a section skips, so the
assertions that follow the sections then run against an unset fixture.

**Integrity labels.** A `workspace-write` session makes the DSH sandbox label the workspace root
**Low** (inheritable), and a Low-labelled executable runs at Low integrity everywhere - Windows
takes the lower of the user and the file - so binaries built in the tree cannot write unlabelled
(medium) paths such as `%TEMP%` when you launch them outside the sandbox. In-session commands get a
private writable `TEMP` instead, which is why the C++ tests can pass while the shell tools cannot
write.

```powershell
# Read the label (needs an elevated shell: reading the SACL requires SeSecurityPrivilege).
(Get-Acl "<repo>" -Audit).Audit | Where-Object { $_.IdentityReference -match 'Mandatory' }

# workspace-write: give the sandbox a writable tree. danger-full-access: back to a medium tree.
icacls "<repo>" /setintegritylevel "(OI)(CI)Low" /T
icacls "<repo>" /setintegritylevel "(OI)(CI)Medium" /T
```

`(OI)(CI)` is only valid on directories, so a `/T` pass relabels directories and leaves existing
**files** as they were. Combined with a sandbox that skips its own full-tree propagation once the
root looks labelled, that produces a mixed tree: new files can be created but existing files cannot
be overwritten or deleted, so `cl`, `link` and `git commit` fail on `Access is denied` while the test
suite still passes. Relabel the files without inheritance flags, or reset the root to `Medium` and
let the sandbox re-grant. The cheap recovery is a fresh clone made while `workspace-write` is active:
everything it creates inherits a consistent Low label and the whole toolchain works.

**pnpm inside the session.** pnpm resolves its store to `<repo>\.pnpm-store\v11`, while
`node_modules/.modules.yaml` records the user's global store. Its dependency check then treats the
modules directory as stale and aborts with
`ERR_PNPM_ABORTED_REMOVE_MODULES_DIR_NO_TTY` before running anything. Either disable that check or
call the tools directly:

```powershell
pnpm --config.verify-deps-before-run=false exec tsc --version
clients/vscode/node_modules/.bin/tsc.cmd --noEmit
```

Do not reach for `CI=true` to get past the abort: that is the switch that tells pnpm to go ahead and
purge `node_modules`, and it will rebuild the tree from the session's store.

**Confined sessions also block CIM/WMI and `sh`-based helpers.** `Get-CimInstance` fails with
"cannot access CIM resources", so inspect processes with `Get-Process`; `git submodule` commands can
fail with `couldn't create signal pipe`, so run them outside the sandbox.

## Architecture

```
src/SlangServer.cpp        Main server class; LSP route handlers + HDL extensions
src/ServerDriver.cpp       Slang driver wrapper; manages syntax trees and open documents
src/ast/ServerCompilation  Wrapper around slang Compilation with incremental updates
src/document/SlangDoc.cpp  File + SyntaxTree pair; token index + shallow compilation
src/document/              Core per-document LSP features (indexer, inlay hints, etc.)
src/completions/           Completion providers (members, instances, macros, keywords)
src/codeactions/           Code actions (expand macro, add define)
src/lsp/                   URI handling
src/util/                  Converters, formatting, markdown, extensions
include/                   Public headers mirroring src/ structure
include/lsp/               LSP type stubs (generated from lsprotocol, rarely updated)
```

## Submodules & External

All in `external/`:
- `slang` — core parser/compiler library (git submodule)
- `reflect-cpp` — JSON serialization (git submodule)
- `ctre` — compile-time regex (git submodule)
- `vscode-system-verilog` — grammar/snippets (git submodule)
- Single-header libs: `BS_thread_pool.hpp`, `expected.hpp`, `boost_concurrent.hpp`, `boost_unordered.hpp`

`external/` is excluded from all formatting and linting hooks.

## Codegen & Schema Sync

When `include/Config.h` changes:
1. Pre-commit hook runs `python3 scripts/genconfig.py`
2. This builds `gen_config_schema`, generates `clients/vscode/resources/config.schema.json`, then converts it to `clients/vscode/src/config.gen.ts`
3. The VS Code extension's `package.json` is updated via the "Extdev: update config" command from the debugging VS Code window

## Code Style

- **C++20**, column limit 100, `#pragma once` (no ifdef guards)
- **lowerCase** for functions, parameters, locals (not LLVM-style UpperCase)
- clang-format v20 config: `.clang-format`
- cmake-format for CMake files (2-space indent)
- Prettier for TS/YAML, Ruff for Python
- `external/` excluded from all formatters

Install pre-commit hooks:
```bash
pip install prek && prek install
```

Run all hooks: `prek run --all-files`

## VS Code Client

Located in `clients/vscode/`. Uses pnpm, Node 20.
```bash
cd clients/vscode && pnpm install && pnpm run compile
```

Debug: copy `.vscode/launch.template.jsonc` to `.vscode/launch.json`, configure build with `cmake -B build/vscode -DCMAKE_BUILD_TYPE=Debug`.

## Key Conventions

- CI builds with `-Werror`; always build locally with the same warning level to avoid CI failures
- Tests use Catch2 (fetched via FetchContent, v3.10.0)
- Test data lives in `tests/data/` (subdirectories like `repo1/`, `basic_config/`, etc.)
- Test harness: `ServerHarness` (C++) and `SlangClient` (Python) wrap the server for in-process/IO testing
- Version lives in the `VERSION` file (currently `0.2.10`)
- License: MIT, REUSE-compliant (see `REUSE.toml`)
