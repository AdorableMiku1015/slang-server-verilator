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
- Port and parameter lists of an instance: inside `u_inst (` the ports of the instantiated module
  that are not connected yet, and inside `#(` its unassigned parameters, inserted as `.port(port),`
  with the expression selected so it can be typed over

![goto-refs](/assets/images/completions.gif)

Items are ranked in layers so that the list starts with what the cursor can refer to: symbols
visible from the cursor (locals, ports, parameters, instances, types), then members of imported
packages, then keywords and snippets, and only then the modules, interfaces, and classes found
anywhere in the workspace. Clients sort by `sortText` before anything else and keep their own fuzzy
ordering within a layer. Item kinds follow the symbol, grouped the same way as the
[semantic token types](semantic-tokens.md), so a data port is not shown as an interface and a
parameter is not shown as a type.

What is offered depends on the cursor: a local declared after it is left out (a symbol has to be
declared before it is used in a procedural block), a shadowed symbol appears once as the one that
would actually resolve, and the values of the enum being assigned to are ranked first.

The standard package is left out. Every compilation unit imports `std` implicitly, so its
verification classes (`mailbox`, `semaphore`, `process`, `weak_reference`) used to be offered at
every declaration position, where they were the only items and never what was being typed.

Accepting any item leaves the document parseable. What a suggestion inserts matches the position it
is offered in: a word that is already being typed is replaced rather than appended to (so completing
a name over a keyword gives that name and not the two joined together), a statement keyword is only
offered where a module item can start and not inside a declaration or a parameter list, a connection
that follows one without a comma brings its own separator, and a constructor is not offered as a
member of an instance. The `always_ff` snippet carries a sensitivity list so that accepting it is
not a syntax error on its own.

Positions that cannot take a completion offer nothing at all, rather than a list that has to be
dismissed: inside a literal or at the end of one (a number is a complete value, and clients ask
again for every digit that is typed into it, because digits are word characters), and after a lone
`:`, which a client opens the list for as soon as it is typed. `::` still completes the scope, and
invoking completion by hand after a colon still gives what can follow it.

A package name is completed wherever a `pkg::member` reference can be written, so an unimported
package is reached by typing part of its name and then `::`, in an expression as much as in a
declaration. The first word of an item is what decides what it is — a type, a package, or a module
to instantiate — so while that word is still being written the whole list of the scope it starts in
is offered, even when the parser glued the word onto the declaration that follows it. A module name
that already has its instance written keeps its source shape: the completion replaces the name
rather than adding a second instance, and an item the parser only guessed at still brings the
instantiation with it.

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
