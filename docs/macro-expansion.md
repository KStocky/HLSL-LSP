# Macro expansion

HLSL-LSP can show the final compiler-authoritative expansion of a macro
invocation at the caret. The feature is available through
`hlsl/macroExpansion` and through native editor actions:

- Visual Studio: **Expand Macro** in Quick Info or
  **HLSL > Expand Macro** in the editor context menu.
- Visual Studio Code: **Expand Macro** in hover or the **HLSL** editor context
  submenu.

The action is contextual and is hidden when DXC does not report a macro
expansion at the current position.

## Compiler authority

`IDxcIntelliSense` identifies the macro invocation and its exact source range.
The shipped API does not expose expanded replacement tokens, so HLSL-LSP
instruments that invocation with private sentinel tokens and runs the same DXC
runtime through its real preprocessor. The text between those sentinels is
returned as the expansion.

This preserves the active document's unsaved text, open include buffers,
include paths, preprocessor definitions, language version, additional
arguments, and selected shader variant. DXC - not a language-server parser -
handles recursive expansion, function arguments, variadic macros,
stringification, token pasting, and compatibility options.

## Protocol

`hlsl/macroExpansion` accepts the standard text-document position shape:

```json
{
  "textDocument": { "uri": "file:///workspace/shader.hlsl" },
  "position": { "line": 12, "character": 9 }
}
```

When the position resolves to an expandable macro, the result includes the
macro name, original invocation, expanded text, source range, optional
definition location, and effective shader context. A non-macro position
returns `null`.

Requests are tied to the analyzed document version and configuration
generation. An edit, variant change, or configuration change supersedes an
in-flight result rather than allowing stale expansion text to replace a newer
view.

## Limitations

- Expansion text is compiler-preprocessed token spelling. DXC can normalize
  whitespace and remove comments.
- Preprocessor-directive contexts are not instrumented because inserting
  sentinel tokens could change directive semantics.
- The operation performs an on-demand preprocessing pass and is therefore
  more expensive than ordinary symbol hover.
