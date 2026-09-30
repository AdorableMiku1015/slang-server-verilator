# Configuration Settings

- `slang.formatters`: array = []

  List of formatter configurations. Each entry specifies a command, directories to format, and language IDs. File input is sent to stdin, and formatted output is read from stdout.

- `slang.inactiveRegions.style`: string = "opacity"

  How to visually indicate inactive preprocessor regions.

  Options:
  - opacity
  - background
  - none
- `slang.inactiveRegions.opacity`: number = 0.55

  Opacity of inactive regions (used only when style is "opacity").

- `slang.inactiveRegions.backgroundColor`: string = "#1212124C"

  Background color for inactive regions (used only when style is "background").

- `slang.lint.enabled`: boolean = true

  Enable diagnostics from the slang language server

- `slang.lint.verilator.enabled`: boolean = false

  Enable verilator lint

- `slang.lint.verilator.path`: path

  Platform Defaults:

    linux:   `verilator`

    mac:     `verilator`

    windows: `verilator`

- `slang.lint.verilator.args`: array = []

  Additional arguments to pass to verilator. Variables such as `${workspaceFolder}`, `${fileDirname}` and `${env:VAR}` are expanded; one that cannot be resolved is passed on as written.

- `slang.path`: path

  Platform Defaults:

    linux:   `slang-server`

    mac:     `slang-server`

    windows: `slang-server.exe`

- `slang.args`: array = []

  Arguments to pass to the slang-server. These are different from slang flags; for those open `.slang/server.json`

- `slang.debugArgs`: array = []

  Arguments to pass to slang-server when debugging
