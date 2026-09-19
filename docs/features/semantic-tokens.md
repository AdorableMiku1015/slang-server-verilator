---
hide:
  - feedback
---

# Semantic Tokens

Semantic tokens highlight identifiers with the meaning the compiler gave them, rather than
what a regular expression grammar can guess. This is what lets the server tell a `net` from
a `variable`, or distinguish the port of an instance from the signal connected to it.

The server implements the standard `textDocument/semanticTokens` LSP routes (both the
`full` and the `range` request), so any client with semantic token support uses them
without extra configuration. Tokens are collected per document and cached until the
document changes, so repeated requests, like the ranged ones a client makes while
scrolling, stay cheap.

Highlighting is enabled by default and can be turned off with
[`semanticTokens.enabled`](#configuration).

## The Legend

A client learns the token types of a server from the *legend* it advertises during
initialization. `slang-server` uses the standard LSP types wherever they fit and adds four
SystemVerilog specific ones. The standard types come first, as clients color those out of
the box, and everything after them needs a client side mapping.

| Type | Reported For |
| ---- | ------------ |
| Standard types | `namespace`, `class`, `interface`, `enum`, `enumMember`, `struct`, `type`, `typeParameter`, `parameter`, `variable`, `property`, `function`, `macro`, `label` |
| `net` | Net declarations (`wire`, `tri`, ...) and their references |
| `port` | Port declarations, their references, and the named port connections of an instance, in both the explicit `.port(sig)` and the shorthand `.port` form |
| `instance` | Instance names, including instance arrays and primitive instances |
| `modport` | Modports, their ports, and clocking blocks |

The legend also advertises four modifiers: `declaration`, `definition`, `readonly` (for
`localparam`, `genvar`, ...), and `defaultLibrary` (macro definitions and system tasks).

### Type Mapping

The four SystemVerilog specific types are mapped onto scopes from the same families that
the bundled
[systemverilog grammar](https://github.com/hudson-trading/slang-server/tree/main/external/vscode-system-verilog)
uses for those constructs, so semantic tokens agree with the colors a file already had. For
example, `port` reuses the very scope the grammar gives to a port name in a connection,
which is what makes the shorthand `.port` and the explicit `.port(sig)` form look alike.

| Type | VSCode scope (`semanticTokenScopes`) | Neovim highlight group |
| ---- | ------------------------------------ | ---------------------- |
| `net` | `variable.other.net.systemverilog` | `@lsp.type.net` |
| `port` | `support.function.port.systemverilog` | `@lsp.type.port` |
| `instance` | `variable.other.module.systemverilog` | `@lsp.type.instance` |
| `modport` | `support.type.scope.systemverilog` | `@lsp.type.modport` |

## What Is Highlighted

Semantic tokens only cover identifiers. Keywords, comments, strings, and numbers keep
coming from the editor's grammar, and semantic tokens are layered on top of it rather than
replacing it.

There are a few cases where no token is reported:

- **Disabled regions.** Tokens inside an untaken `` `ifdef `` branch are skipped, since
  clients already dim those regions.
- **Wildcard connections.** `.*` has no name to highlight; the ports it covers are not
  spelled out.
- **Unnamed tokens.** Tokens that slang inserted while recovering from a syntax error are
  skipped, as they have no text in the document.
- **Escaped identifiers.** For `\foo ` the highlighted range covers the name only, leaving
  out the leading backslash and the trailing space.

Note that a token is classified from the syntax and the symbol index of the current
document, so highlighting still works when a design is not set: a port connection is
colored as a port even if the instantiated module cannot be resolved. The flip side is that
references to a port resolve to the symbol of the port, which for a net type port is the
net behind it; the declaration itself is always reported as a port.

## Client Setup

### VSCode

The extension ships the scope mapping above in its `semanticTokenScopes` contribution,
which is how a custom token type gets a color from the active theme. Semantic highlighting
is on by default in VSCode; if it has been disabled, re-enable it with
`"editor.semanticHighlighting.enabled": true`.

### Neovim

Neovim renders semantic tokens through the `@lsp.type.<type>` highlight groups, which a
colorscheme links for the standard types. The four custom types have no default group, so
they keep the tree-sitter colors until they are given one:

```lua
vim.api.nvim_set_hl(0, "@lsp.type.port", { fg = "#34bfd0" })
```

The engine is part of Neovim itself (`vim.lsp.semantic_tokens`), and runs for servers that
advertise the capability.

## Customizing Colors

Both clients let you recolor a token type without touching the extension.

VSCode, in `settings.json`:

```json
"editor.semanticTokenColorCustomizations": {
  "rules": {
    "port": "#34bfd0",
    "net": "#9CDCFE",
    "instance": { "bold": true },
    "*.declaration": { "bold": true }
  }
}
```

Neovim, which colors the token types through their highlight groups:

```lua
vim.api.nvim_set_hl(0, "@lsp.type.port", { fg = "#34bfd0" })
vim.api.nvim_set_hl(0, "@lsp.type.instance", { fg = "#efbd5d", bold = true })
```

The groups are the ones in the mapping table above, so any type Neovim derives from the
legend can be set, and a colorscheme can link them instead, e.g.
`hi! link @lsp.type.net Identifier`.

## Configuration

### `semanticTokens.enabled`

Type: `boolean`, default `true`. Set it to `false` in
[`.slang/server.json`](/start/config.md) to stop the server from providing tokens and let
the editor's grammar do all of the highlighting.

```json
{
  "semanticTokens": {
    "enabled": false
  }
}
```

The capability stays advertised while disabled, so turning it back on takes effect on the
next request without restarting the client.
