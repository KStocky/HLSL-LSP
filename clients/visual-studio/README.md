# Visual Studio client

This VSIX supplies DXC diagnostics and IntelliSense through a bundled
`hlsl-lsp.exe`.

It supports the 64-bit Community, Professional, and Enterprise editions of
Visual Studio 2022 17.14 or newer and Visual Studio 2026. Visual Studio 17.14 is
the minimum because the client compiles against and uses the 17.14 Visual
Studio SDK and language-server client.

Visual Studio's HLSL Tools extension claims the same file extensions and is not
currently compatible with HLSL-LSP. Disable or uninstall HLSL Tools before
installing this VSIX.
Go-to-definition for symbols and `#include` paths is provided by the server
through LSP. Workspace symbols also integrate with Visual Studio's All-In-One
Search (`Ctrl+T`). LSP semantic tokens are disabled in Visual Studio because
its classification pipeline can hang the editor while applying them; the
language server still provides richer semantic tokens to other clients. A
separate MEF-only assembly provides immediate lexical classification for HLSL keywords,
preprocessor directives, types, functions, comments, strings, and numbers.
Each category appears as an `HLSL ...` item under **Environment > Fonts and
Colors**, using the same Visual Studio colour picker as HLSL Tools.

The VSIX deliberately does not export a remote content type or language client
through MEF. Those exports can change Visual Studio's workspace-composition graph
during startup and have caused the shell to deadlock. A lightweight text-view
listener records filenames without referencing Visual Studio's LSP APIs; the
bootstrap matches them against the configured HLSL extensions.
In CMake workspaces it waits for the CMake package's asynchronous load to finish;
it then loads an isolated package and language-client assembly through the public
broker and promotes open shaders to dynamic remote subtypes.
Before a folder or solution closes, the package demotes every shader back to its
native content type, so restored documents never begin the next startup as
LSP-backed buffers. Later HLSL documents activate automatically. No user action
is required.

Use the native **Tools > Options > HLSL-LSP > General** Unified Settings page
to set:

- **HLSL file extensions**: a semicolon-separated list such as
  `.hlsl;.hlsli;.usf`. This is also the default. The built-in `.hlsl` and
  `.hlsli` mappings are always available.
- **Default HLSL language version**: the default DXC `-HV` value. A
  `hlsl.languageVersion` value in `shadertoolsconfig.json` takes precedence.

Changes apply to open and future documents. Shutdown never waits for a failed
LSP broker operation and bounds child-process cleanup.

Custom HLSL tool windows use Visual Studio's dynamic theme resources for their
text, backgrounds, hyperlinks, controls, scrollbars, and environment font.
Open windows update with the IDE theme and remain readable in both dark and
light themes.

The editor's **HLSL** submenu is caret-aware. File-wide commands remain
available throughout an HLSL document, while **Memory Layout**, **Call
Hierarchy**, **Entry-Point Data Flow**, and **Compute Visualization** appear
only on symbols where they apply. Applicable commands identify their resolved
target in the label, such as **Call Hierarchy for helper**. Context is resolved
asynchronously, so opening the menu never blocks the Visual Studio UI; while a
new caret position is being resolved, contextual commands are temporarily
disabled rather than acting on stale data.

When a `shadertoolsconfig.json` declares named compilation variants under
`hlsl.variants`, right-click the shader and choose
**HLSL > Select Shader Variant**. The picker contains only variants applicable
to that file; on a configured entry point it narrows further to that entry
point. Changing the active variant reanalyzes open
documents and restarts the language server only if the variant selects a
different DXC runtime. See the repository's
[named compilation variants](../../docs/shadertoolsconfig.md#named-compilation-variants)
reference for details. Quick fixes offered through the native lightbulb
(`Ctrl+.`, see [code actions](../../docs/code-actions.md)) can also change the
active variant through the same underlying mechanism: the client's cached
active variant and the picker's own state stay in sync either way, because
the server reports its resulting variant back through a
`hlsl/activeVariantChanged` notification after the command completes.

Each HLSL editor shows a compact extension-owned indicator with the
server-authoritative active variant, effective entry point, and effective
target profile. Hover it for the full file and configuration-origin context;
click it to invoke **HLSL > Select Shader Variant**. The indicator updates
asynchronously, is removed with its editor view, and never overwrites Visual
Studio's shared status text or performs RPC from `BeforeQueryStatus`. Every
custom HLSL tool window uses the same
**File**, **Variant**, **Entry point**, **Target profile**, and available
**Configuration origins** header (`Default` and `Not configured` are used
consistently).

Choose **HLSL > Shader Compilation** from the editor context menu to open a tool window with the active
HLSL document's effective compiler configuration, compiler success/failure and
diagnostics, output type and size, DXC reflection (signatures, resource
bindings, and thread-group size), and include directories/resolved include
paths. The request always analyzes the document's current unsaved content and
active variant. If the window is already open, it refreshes automatically
when the active variant changes and when the shown document is saved. See the
repository's [shader compilation](../../docs/compilation-info.md) reference
for the full protocol, including DXIL vs SPIR-V reflection availability.

Right-click the active shader and choose **HLSL > Resource Bindings** to open a
dedicated tool window with its resource bindings grouped by register space then
CBV/SRV/UAV/sampler class, provable register-range collisions, the embedded
root signature's state and full details when available, and whether the
reflected resources are compatible with that root signature. It reuses the
same request, current unsaved document, and refresh triggers (active-variant
change, shown-document save) as Shader Compilation, in its own independent
tool window so opening one never disturbs the other. Resource names and
collision participants are clickable, without stealing focus during
background refresh, only when DXC's reflection supplies an unambiguous
declaration location for them; absence or ambiguity is intentionally
non-clickable rather than a guessed link. See the
repository's [resource bindings](../../docs/resource-bindings.md) reference
for the full protocol, grouping/collision semantics, root-signature states
(including the Windows-only detail-deserialization requirement and SPIR-V's
"not applicable" state), compatibility meanings, and the bindless descriptor-heap
limitation.

## Call hierarchy and entry-point data flow

Visual Studio 17.14's generic LSP client
(`Microsoft.VisualStudio.LanguageServer.Client`) does **not** route the
editor's built-in **right-click > View Call Hierarchy** command to any
language client, regardless of whether it advertises the standard
`callHierarchyProvider: true` capability -- unlike hover, signature help,
and go-to-definition, that SDK has no bespoke call-hierarchy hookup at all.
A prior version of this document assumed otherwise; this has since been
confirmed incorrect, and this client now ships a **custom** editor-context command
and tool window instead of relying on any built-in surface.

Right-click a function and choose **HLSL > Call Hierarchy** to open a
dedicated tool window. It issues `textDocument/prepareCallHierarchy` at the
caret, then `callHierarchy/incomingCalls` and `callHierarchy/outgoingCalls`
for the resolved item, and shows the selected callable plus its incoming
callers and outgoing callees (each with a call-site count). Clicking
"Explore calls" on any caller/callee re-centers the view on that item
(fetching its own incoming/outgoing calls without a new
`prepareCallHierarchy`, since its identity is already known), and **Back**
returns to the previous view without any new request. This is a one-level
(immediate callers/callees) view per step, matching the LSP spec's own
per-request shape; drilling in is how deeper traversal is reached, since
the server does not offer a whole-tree response. The window refreshes its
current item's calls (not a fresh caret resolution) after the active
variant changes or a relevant HLSL/header/`shadertoolsconfig.json` file is
saved, preserving the last successful content if the refresh fails or the
item has gone stale (the server's standard `ContentModified` response,
translated client-side into a plain, LSP-agnostic exception so the
Bootstrap assembly issuing the tool window never depends on
StreamJsonRpc/LSP wire types).

Right-click an entry point and choose **HLSL > Entry-Point Data Flow** to open a tool window that
traces every function transitively reachable from the active HLSL
document's configured entry point, alongside the globals/resources those
functions read or write and the functions/declarations unused for the
active variant. The request always analyzes the document's current unsaved
content and active variant, matching Shader Compilation/Resource Bindings;
if the window is already open, it refreshes automatically when the active
variant changes and when a relevant HLSL/header/`shadertoolsconfig.json`
file is saved (unsaved edits to `shadertoolsconfig.json` do **not** trigger
a refresh, since the server only reads that file from disk), while keeping
the last successful content on screen for a transient failure. A clearly
labelled header reports whether an entry point was found (and why not, when
absent) and whether traversal was truncated by the server's function-visit
budget, with separate sections for reachable functions (depth and
recursion labelled per entry), global/resource accesses (read/write/both
labelled per entry), unreachable functions, and unused declarations;
section-specific truncation (function-visit, definition-collection,
global-access, and unused-declaration budgets are each independent) is
called out where it applies rather than uniformly blaming the function
graph. Unreachable functions is shown as "not determined" rather than an
empty list whenever either the function-visit or definition-collection
budget was hit, since the server leaves that list empty in both cases
(an unvisited or uncollected function can't be proven dead code).

Right-click a configured compute shader and choose
**HLSL > Compute Visualization** to inspect its reflected `numthreads` size,
logical workload, derived D3D
`Dispatch()` group count, launched and inactive edge threads, system-value
mappings, group-shared memory, barriers, wave-size requirements, and optional
hardware-dependent occupancy estimate. The X/Y/Z inputs are the desired total
logical threads or elements - not `Dispatch()` group counts - and may all be
left blank to default to exactly one reflected thread group. Hardware limits
are optional and occupancy is never guessed without them. Edited values take
effect only after **Apply / Refresh**; automatic background refreshes continue
using the last successfully applied values.

Both windows navigate every compiler-supplied symbol/location via its
`selectionRange` (falling back to the wider `range` only if the
`selectionRange` is absent or malformed) -- never a location guessed from
the symbol's name -- and both report reads (`"read"`), writes (`"write"`),
or both (`"readWrite"`) conservatively: an access is only ever narrowed to
read-only or write-only when the compiler's own cursor tree proves it. See
the repository's
[call hierarchy and entry-point data flow](../../docs/call-hierarchy.md)
reference for the full protocol, the `CallHierarchyItem.data` identity
envelope, and DXC's known limitations.

## Install

Download `HlslLsp.VisualStudio.vsix` from the
[latest GitHub release](https://github.com/KStocky/HLSL-LSP/releases/latest).
Close Visual Studio, run the VSIX, and follow the installer prompts.

Release artifacts are not yet code-signed. Windows Smart App Control may block
the extension or language server unless Developer Mode is enabled.

## Build from source

Build the native server first:

```powershell
cmake --preset windows-msvc
cmake --build --preset windows-msvc-release --target hlsl-lsp
```

Then build the VSIX with Visual Studio MSBuild:

```powershell
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$msbuild = & $vswhere -latest -products * `
  -requires Microsoft.Component.MSBuild `
  -find MSBuild\**\Bin\MSBuild.exe |
  Select-Object -First 1
$serverDir = (Resolve-Path 'out\build\windows-msvc\Release').Path

& $msbuild clients\visual-studio\HlslLsp.VisualStudio\HlslLsp.VisualStudio.csproj `
  /restore `
  /p:Configuration=Release `
  "/p:HlslLspServerDir=$serverDir"
```

The VSIX is written to
`clients\visual-studio\HlslLsp.VisualStudio\bin\Release\net472\HlslLsp.VisualStudio.vsix`.
Close Visual Studio and run that file to install the extension. Open an
`.hlsl`, `.hlsli`, or configured HLSL file; the server starts automatically
once the host workspace is ready.

Use `.vs\VSWorkspaceSettings.json` for editor overrides:

```json
{
  "hlsl.preprocessorDefinitions": {
    "EDITOR_BUILD": 1
  },
  "hlsl.additionalIncludeDirectories": [
    "Shaders/Includes"
  ]
}
```

Visual Studio LSP traces can be enabled with `"hlsl.trace.server": "Verbose"`.

## Tests

`HlslLsp.VisualStudio.Tests` covers the language client's custom-notification
wiring (`hlsl/activeVariantChanged`, `hlsl/didChangeActiveVariant`,
`hlsl/dxcRuntimeRestartRequired`, `hlsl/configurationChanged`, and explicit
configuration-file save notifications) with real `StreamJsonRpc` instances
connected over an in-memory duplex stream, driven through the same
`ILanguageClientCustomMessage2` entry points (`CustomMessageTarget`,
`AttachForCustomMessageAsync`) Visual Studio itself uses — concrete
protocol-level evidence rather than an assumption that the generic LSP SDK
client supports these notifications. It does not require Visual Studio to be
installed or running. Run it with:

```powershell
dotnet test clients\visual-studio\HlslLsp.VisualStudio.Tests\HlslLsp.VisualStudio.Tests.csproj
```

The rest of the extension's Visual Studio-specific integration surface
(commands, tool windows, editor content-type switching) requires the full VS
SDK experimental instance and is verified manually per the build/run
instructions above; it is not covered by this automated project.
