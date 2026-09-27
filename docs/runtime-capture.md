# Runtime compilation capture (backend v1)

This is an **opt-in, read-only** capture channel. It does not hook DXC, load
into another process, inspect shader source, discover missing paths, or write
`shadertoolsconfig.json`. An engine calls the SDK at its existing compile
wrapper with metadata it already knows. Generated/in-memory shaders require a
stable host-supplied logical `source` identity (for example
`virtual:/materials/wood.hlsl`); never substitute an invented disk path.
Custom include handlers may supply `virtual_mappings` and include directories.

## C++23 integration

Link the `hlsl_capture` static target (and its transitive dependencies). Include
`<hlsl_intellisense/capture/capture.h>`. The public API is source-compatible
within protocol v1, not a stable binary ABI: build the SDK with your
application's C++ runtime/compiler. The SDK archive and headers are installed
with the `SDK` component; external builds must also link Windows `bcrypt` and
`advapi32` where applicable. There is no DXC dependency in the SDK.

Build the standalone example with
`cmake --build --preset windows-msvc-debug --target hlsl-capture-example`
(or the corresponding platform preset). Run it with an existing shader path:
`hlsl-capture-example <shader-source-path>`. It first reports while
disconnected, then reads the endpoint and token of an **active** editor capture
session on separate standard-input lines, connects, reports one illustrative
`Main`/`ps_6_7` invocation, and disconnects. Review and apply in the editor
before stopping the session. The example does not compile a shader: in a real
engine, populate invocation metadata from the actual compile request and keep
the endpoint and token in a private control channel, never in command-line
arguments, logs, or source control.

```cpp
namespace shader_capture = hlsl_intellisense::capture;
shader_capture::Client client; // disconnected: report() returns false immediately
// After the user starts an editor capture session, pass endpoint and token
// securely to the engine (for example via its existing local control plane).
client.connect(endpoint, token);
shader_capture::Invocation invocation;
invocation.source = "virtual:/materials/wood.hlsl";
invocation.entry_point = "MainPS";
invocation.target_profile = "ps_6_7";
invocation.language_version = "2021";
invocation.defines = {{"QUALITY", "HIGH"}};
invocation.include_directories = {"Shaders/Includes"};
invocation.virtual_mappings = {{"virtual:/materials", "Shaders/Materials"}};
invocation.arguments = {"-Zi"};
invocation.output_mode = "spirv";
invocation.pipeline = "Forward";
invocation.stage = "pixel";
(void)client.report(invocation); // never gate compilation on this result
client.disconnect();
```

`report` is thread-safe and `noexcept`; a
disconnected call only checks an atomic flag. Connected calls serialize
metadata and try to enqueue without waiting for IPC or a contended lock.
`false` means the report was not queued. The worker may drop reports if the
server disconnects; no compilation success/failure depends on capture.
`disconnect()` and destruction drain queued reports and join the worker (delivery
is still best effort); call them off the compiler critical path. Start/stop session and connect/disconnect should not be
concurrently invoked on the *same* object.

## LSP session methods

All methods require an initialized server and `{"protocolVersion":1}`.

| Method | Additional request | Result |
| --- | --- | --- |
| `hlsl/capture/start` | none | `{protocolVersion, sessionId, endpoint, token}` |
| `hlsl/capture/status` | none | `{protocolVersion, sessionId, active, accepted, rejected, overflow}` |
| `hlsl/capture/snapshot` | optional `workspaceFolder: {uri}` (initialized folder) | status plus `entries: [{id, invocation, count, review}]` |
| `hlsl/capture/stop` | `token` returned by start | `{protocolVersion:1, active:false}` |
| `hlsl/capture/preview` | see below | `{protocolVersion, sessionId, selectedEntryIds, configuration, preview}` |

Starting again revokes the previous token and clears the snapshot. Stop,
shutdown, and server destruction close the endpoint and erase the in-memory
snapshot. A stale stop token is rejected. Do not persist or log the endpoint
token; it is redacted in protocol traces even when source tracing is enabled.
`sessionId` is a non-secret 71-character `sha256:` identifier for the active
session (null when inactive). Entry `id` values have the same format and are
deterministic for the same invocation **within that session**. Starting a new
session changes both identifiers; old IDs cannot be previewed. An ID is not
an IPC credential: only `token` authorizes the engine connection.
Snapshot results may themselves contain sensitive paths, defines, or arguments:
do not forward them to telemetry or untrusted extensions. The editor should
filter transient/generated values and require explicit user selection, naming,
preview, and confirmation before requesting edits from the shared
configuration-authoring API.

For each entry, `review` contains a bounded, read-only projection:

```json
{
  "eligible": true,
  "requiresConfirmation": true,
  "selection": {
    "relativePath": "Shaders/Materials/wood.hlsl",
    "entryPoint": "MainPS",
    "targetProfile": "ps_6_7"
  },
  "settings": {
    "hlsl.entryPoint": "MainPS",
    "hlsl.targetProfile": "ps_6_7",
    "hlsl.languageVersion": "2021",
    "hlsl.preprocessorDefinitions": {"QUALITY": "HIGH"},
    "hlsl.additionalIncludeDirectories": ["Shaders/Includes"],
    "hlsl.virtualDirectoryMappings": {"/Project": "Shaders"},
    "hlsl.additionalArguments": ["-Zi"]
  },
  "fileGroup": {
    "files": ["Shaders/Materials/wood.hlsl"],
    "hlsl.entryPoint": "MainPS",
    "hlsl.targetProfile": "ps_6_7",
    "hlsl.languageVersion": "2021",
    "hlsl.preprocessorDefinitions": {"QUALITY": "HIGH"},
    "hlsl.additionalIncludeDirectories": ["Shaders/Includes"],
    "hlsl.virtualDirectoryMappings": {"/Project": "Shaders"},
    "hlsl.additionalArguments": ["-Zi"]
  },
  "warningCodes": ["includeDirectories", "arguments", "virtualMappings"],
  "warnings": [
    "Review include directories relative to the configuration.",
    "Review compiler arguments for transient paths and secrets.",
    "Review virtual mapping targets relative to the configuration."
  ]
}
```

Without `workspaceFolder`, `settings` is still populated, but `eligible` is
false and `selection`/`fileGroup` are null. A file group is offered only for
an existing regular source file lexically *and canonically* inside the selected
workspace, not in a nested configuration, with nonempty entry point and no
conflicting/empty definition or mapping keys. Logical/in-memory identities,
missing files, paths outside the workspace, network-backed paths, and ambiguous keys remain raw
entries with warnings; the server never guesses a disk location. Definitions
are strings as captured. `outputMode` and pipeline/stage correlation remain
only on the raw invocation: the configuration schema has no output-mode field,
and pipeline relationships/names must be chosen by the user.
`warningCodes` is stable for client decisions; `warnings` is display text in
the same order. Possible codes are `includeDirectories`, `arguments`,
`outputMode`, `virtualMappings`, `conflictingKeys`, `workspaceRequired`,
`logicalIdentity`, `networkPath`, `outsideWorkspace`, `missingFile`,
`resolvedOutsideWorkspace`, `nestedConfiguration`, and `entryPointOrSettings`.

`eligible` does **not** mean settings have passed production validation. A
`review.selection` is advisory: do not put it in discovery `selections` unless
DXC discovery also returned that exact candidate.

## Explicit capture merge preview

`hlsl/capture/preview` performs a **read-only**, versioned merge of explicitly
selected captured IDs into the root workspace configuration. Example request:

```json
{
  "protocolVersion": 1,
  "workspaceFolder": {"uri": "file:///C:/work/project"},
  "sessionId": "sha256:<64 hex digits from start/snapshot>",
  "selectedEntryIds": ["sha256:<captured entry ID>"],
  "existingConfiguration": {
    "content": "{\"root\":true}",
    "version": 12,
    "contentHash": "sha256:<hash of content>"
  },
  "variants": [{"entryId": "sha256:<captured entry ID>", "name": "High Quality"}],
  "pipelines": [{
    "name": "Forward",
    "source": "host",
    "stages": {
      "vertex": "sha256:<selected vertex ID>",
      "pixel": "sha256:<selected pixel ID>"
    }
  }]
}
```

`existingConfiguration`, `variants`, and `pipelines` are optional.
`existingConfiguration.content`, `version`, and `contentHash` are independently
optional as in `hlsl/configurationAuthoring`. Omitted content reads the root
configuration from disk; a supplied hash must match the supplied or disk
content. The request must identify an initialized workspace and active
session. Select 1–64 *unique*, eligible entry IDs. Unscoped/logical/outside
workspace captures cannot be merged. Multiple distinct captured invocations
for one file require **each** to have an explicitly named variant, avoiding
last-file-group-wins ambiguity. Variant names must be unique, nonempty, and at
most 128 characters; at most 64 variants are accepted. Equivalent invocations
have one ID and one merged fragment regardless of occurrence count.

At most 16 named pipelines may be supplied. Each requires `source:"host"` or
`"user"`, a `stages` object mapping stage names to selected entry IDs, and
both `vertex` and `pixel`. Optional stages are `geometry`, `hull`, and
`domain` (the latter two together). The captured target-profile prefix must
match each stage (`vs_`, `ps_`, `gs_`, `hs_`, `ds_`). `source:"host"` additionally
requires every stage's captured `pipeline` and `stage` correlation to match the
requested name and stage. `source:"user"` records the user's *explicit*
grouping; the server never infers missing relationships. Pipeline stages
include the corresponding named variant if one was selected.

The result has the existing authoring envelope:

```json
{
  "protocolVersion": 1,
  "sessionId": "sha256:<active session ID>",
  "selectedEntryIds": ["sha256:<selected entry ID>"],
  "configuration": {
    "uri": "file:///C:/work/project/shadertoolsconfig.json",
    "exists": true,
    "expectedContentVersion": 12,
    "expectedContentHash": "sha256:<original content hash>"
  },
  "preview": {
    "content": "{\n  ...\n}\n",
    "contentHash": "sha256:<preview content hash>",
    "valid": true,
    "changed": true,
    "errors": []
  }
}
```

Malformed, duplicate selected IDs or request-side names, unauthorized-workspace,
stale-session/entry, invalid stage/correlation, and oversize inputs fail with
`InvalidParams` (-32602).
Existing content and final preview are each limited to 2 MiB. A valid request
whose existing JSON is malformed, whose existing explicit group/name conflicts
with the capture, or whose merged content fails the production parser returns
`preview.valid:false` with field-addressed `{code,field,message}` errors and
does not overwrite existing content. Existing JSON nesting beyond 64 levels
also returns a `capture-limit` preview error. Unknown properties are retained.
Applying the preview remains an **editor decision**: compare expected version
and hash with the current buffer, show the entire diff and warnings, obtain
confirmation, then apply an editor workspace edit. For repairs or further
editing, send edited preview text as `draftContent` to
`hlsl/configurationAuthoring`. No server method writes configuration.

## Local wire protocol and limits

Windows: a single-instance `\\.\pipe\hlsl-capture-<random>` byte pipe with
current-user-only ACL and remote clients rejected. Linux: a `0600` Unix-domain
socket in an existing private `XDG_RUNTIME_DIR` (owned by the current uid,
with no group/other permissions); `SO_PEERCRED` checks uid. If no such runtime
directory exists, start fails closed. No TCP or network transport.

Each connection transmits one frame: 4-byte unsigned big-endian length
(1..32768), followed by UTF-8 JSON:

```json
{
  "version": 1,
  "token": "<64 lowercase hex digits returned by start>",
  "event": {
    "source": "virtual:/materials/wood.hlsl",
    "entryPoint": "MainPS",
    "targetProfile": "ps_6_7",
    "languageVersion": "2021",
    "defines": [["QUALITY", "HIGH"]],
    "includeDirectories": ["Shaders/Includes"],
    "virtualMappings": [["virtual:/materials", "Shaders/Materials"]],
    "arguments": ["-Zi"],
    "outputMode": "spirv",
    "pipeline": "Forward",
    "stage": "pixel"
  }
}
```

The listener replies with the same length-prefix format containing either
`{"status":"accepted"}` or `{"status":"rejected"}`. On Windows the client
sends one confirmation byte after receiving the reply, allowing the server to
disconnect the pipe without discarding unread reply bytes. All peer reads and
writes have bounded waits; SDK reports remain asynchronous and are not
guaranteed to reach the listener.

The listener counts and rejects invalid JSON, JSON nesting deeper than 16,
wrong schema/version/token, oversized frames, and incomplete frames; an
authenticated valid invocation is
counted once per accepted connection. It retains at most 256 distinct
invocations in canonical JSON-key order, with repeated equivalent events
incrementing `count`. Beyond that bound, `overflow` increments instead of
retaining new permutations. `accepted` counts valid reports even when the
distinct-invocation bound is reached; `rejected` counts invalid connections.
Each field and list has a size bound; SDK pending queue holds at most 64 frames.
There is no retry of already dropped events, file logging, or persistent data.
Treat the snapshot as a *sample*, not a complete telemetry log.

Callers must deliberately omit secrets in compiler arguments/defines and
avoid reporting shader contents. Identity and mappings are sent as supplied;
the backend never tests whether paths actually exist.
