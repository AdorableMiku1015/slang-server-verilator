# Extension Development

Install nvm (Node Version Manager), or install Node 20

```
git clone git@github.com:hudson-trading/slang-server.git
code .
cd slang-server/clients/vscode
nvm use 20
npm install -g pnpm
pnpm install
```

### Debugging

- The server logs to the `slang-server` output channel. These are triggered with the `INFO`, `WARN` and `ERROR` macros.
- The client logs to the `Slang` output channel using logger classes.
- `console.log()` logs to the debug console of the slang-server/ vscode window.
- `vscode.window.showInformationMessage()` is useful for showing popups for debugging. On the server side these can also be triggered with `LspClient::showInfo()`.

### Updating package.json (config paths, commands, etc.)

Traditional vscode extension development requires many strings to match up between package.json and the code. I made a small library within this repo to generate package.json from the code where possible, so there's a single source of truth for these strings, and it's easy to add commands, buttons, and congurations.

To update the package.json after changing one of these components, Run "Extdev: update config (package.json and CONFIG.md)" in the vscode window you're debugging.

### Contributions only load on a restart

Static contributions are read by the workbench when the window loads, so an installed or
sideloaded build keeps using the previous set until vscode is restarted. The Extensions view
is not a reliable check either: it reads the manifest from disk and reports the new version
while the running workbench still holds the old contributions.

`contributes.semanticTokenScopes` is easy to get bitten by, since a new token type or
modifier selector silently keeps its previous color. "Inspect Editor Tokens and Scopes"
shows the giveaway: a token that already reports the new type or modifier still resolves to
the old textmate scope. Reload the window, and restart vscode when the change still does not
appear.
