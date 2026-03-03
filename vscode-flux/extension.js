// extension.js — Flux Language VSCode Extension entry point
// Syntax highlighting is provided entirely via the TextMate grammar
// (syntaxes/flux.tmLanguage.json); no activation logic is needed.
'use strict';

/**
 * @param {import('vscode').ExtensionContext} context
 */
function activate(context) {
    // Reserved for future features:
    //   - Hover type information (flux check --hover)
    //   - Inline diagnostics (flux check on save)
    //   - Format on save (flux fmt)
    //   - REPL integration
}

function deactivate() {}

module.exports = { activate, deactivate };
