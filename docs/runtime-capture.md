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
`disconnect()` and destruction join the worker; call them off the compiler
critical path. Start/stop session and connect/disconnect should not be
concurrently invoked on the *same* object.

## LSP session methods

All methods require an initialized server and `{"protocolVersion":1}`.

| Method | Additional request | Result |
| --- | --- | --- |
| `hlsl/capture/start` | none | `{protocolVersion, endpoint, token}` |
| `hlsl/capture/status` | none | `{protocolVersion, active, accepted, rejected, overflow}` |
| `hlsl/capture/snapshot` | none | status plus `entries: [{invocation, count}]` |
| `hlsl/capture/stop` | `token` returned by start | `{protocolVersion:1, active:false}` |

Starting again revokes the previous token and clears the snapshot. Stop,
shutdown, and server destruction close the endpoint and erase the in-memory
snapshot. A stale stop token is rejected. Do not persist or log the endpoint
token; it is redacted in protocol traces even when source tracing is enabled.
Snapshot results may themselves contain sensitive paths, defines, or arguments:
do not forward them to telemetry or untrusted extensions. The editor should
filter transient/generated values and require explicit user selection, naming,
preview, and confirmation before requesting edits from the shared
configuration-authoring API. **The backend does not yet merge captured
selections into configuration previews**; these must be reviewed separately.

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
