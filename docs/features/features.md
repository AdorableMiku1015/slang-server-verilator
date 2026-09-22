# Language Features

### Diagnostics

Diagnostics are provided by slang using [Shallow Compilations](design/shallow.md) when a design is not set.
When a [design is set](hdl/hdl.md), the shallow compilation provides diagnostics on each keystroke, and the design's compilation provides diagnostics on each save.

![goto-refs](/assets/images/lints.gif)


### Hovers and Go-to Definition

Hovers are provided on each symbol with the following info if applicable:

- Symbol kind, or Syntax kind in the case of macros.
- Lexical Scope, or file in the case of macros.
- Resolved type info
- Bitwidth, if a value or type
- Value, if a constant value
- The syntax that the symbol is derived from
- Macro usage, if symbol was defined from one.

With an elaborated design, hovers use the [active instance](hdl/hdl.md#active-instances) of the enclosing module or interface. Parameter values, dependent types and widths, interface connections, and driver information therefore reflect the selected instance rather than the default elaboration.

![goto-refs](/assets/images/hovers.gif)

Planned features:

- Multiple definitions, for example with modports.
- Macro expansion on hover, with expand quick-action
- Hovers/Gotos for struct assignments
- Hovers for builtins


### Completions

Completions are currently provided for the following constructs:

- Generic expression completions: parameters, variables, types etc.
- Modules and interfaces
- Functions and macros
- Hierarchical references and struct members

![goto-refs](/assets/images/completions.gif)

Items are ranked in layers so that the list starts with what the cursor can refer to: symbols
visible from the cursor (locals, ports, parameters, instances, types), then members of imported
packages, then keywords and snippets, and only then the modules, interfaces, and classes found
anywhere in the workspace. Clients sort by `sortText` before anything else and keep their own fuzzy
ordering within a layer. Item kinds follow the symbol, grouped the same way as the
[semantic token types](semantic-tokens.md), so a data port is not shown as an interface and a
parameter is not shown as a type.

Planned completions:

- Named assignments (structs, functions, ports, params)
- builtins (`$bits()`, etc.)

### Go-to References / Rename

Go-to references are provided for every nearly all symbols except for macros. Keep in mind this is an expensive operation, and may take some time for a large repo.

![goto-refs](/assets/images/gotorefs.gif)

### Semantic Tokens

Semantic tokens highlight identifiers with the meaning the compiler gave them, instead of
what a grammar can guess: nets, ports, instances, modports, parameters, `localparam`s, and
everything else the standard LSP token types can express. They are layered on top of the
grammar based highlighting, and are provided for every supported client. See
[Semantic Tokens](semantic-tokens.md) for the legend, the client mappings, and how to
customize the colors, or set `semanticTokens.enabled` to `false` to turn them off.

### Inlay Hints

Inlay hints are text that show up inline in the code to provide useful info. They can be hovered for more info, and some can be double clicked to insert some text.

**Ports Types** - Show the type of ports in instances (off by default)

![goto-refs](/assets/images/port_inlays.png)

**Wildcard Ports** - Show which signals are passed through
![goto-refs](/assets/images/port_wildcard_inlays.png)

**Positional args in Macros, Functions, Parameter/Port lists** - Show the argument names
![goto-refs](/assets/images/macro_inlays.png)

**Active parameter values** - Show resolved parameter and localparam values for the [active instance](hdl/hdl.md#active-instances). These hints are enabled by default and can be controlled with `inlayHints.activeParameterValues`.

Planned Inlays:

- **Signals** - Show the value when a waveform is connected and an instance is selected.
- **Wildcard Imports** - Show which symbols are used from the import

### Planned LSP Methods:

**Formatting**
This will likely live in the slang repo and also be shipped as a standalone binary. In order to have fairly nice formatting in hovers and completions, basic formatting functions already exist in this repo. This includes things like squashing white spaces to condense hover and completion types, and left aligning blocks of text for hovers and completion docs.
