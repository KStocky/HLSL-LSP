# Production behavior and compatibility

## Protocol tracing and privacy

`hlsl-lsp --trace-protocol` writes JSON-RPC traffic to standard error. Standard
output remains reserved for LSP frames. The trace retains only protocol
structure such as methods and numeric request IDs; parameters, results, errors,
and string IDs are replaced with their payload byte count. This also protects
source-derived diagnostics, completion labels, hover text, paths, and workspace
edits rather than redacting only open-buffer fields.

`--trace-source` enables unredacted tracing for a local debugging session. It
implies `--trace-protocol` and can expose complete shader sources in editor
logs; do not use it when logs may be uploaded or shared.

## Crash diagnostics

The language-server executable installs a minimal native crash breadcrumb. DXC
itself is loaded only in persistent `hlsl-analysis-worker` child processes.
Each analysis scheduler owner hashes to exactly one child, which retains that
owner's translation units and serves every compiler query. A DXC crash,
deadline, cancellation, or malformed private-protocol reply terminates that
exact child and invalidates every parent cache entry assigned to it; there is
no in-process compiler fallback that can hang the language server.

Background failures publish a stale-safe
`hlsl-lsp/analysis-unavailable` diagnostic and retain dependency metadata for
future invalidation. A later successful reanalysis clears the diagnostic.
Interactive failures return a JSON-RPC cancellation/internal error and
schedule or require safe reconstruction instead of returning guessed data.

Top-level server startup and LSP framing failures are reported through the
existing error path and return a nonzero process status. Private analysis-worker
failures are contained and recovered as described above.

## Resource limits

All queues and caches are bounded. Defaults and command-line overrides are:

| Resource | Default | Override |
|---|---:|---|
| Analysis workers | 2 | `--analysis-workers N` |
| Analysis queue | 64 | `--analysis-queue-capacity N` |
| Background DXC deadline | 30 seconds | `--analysis-background-timeout-ms N` or `--analysis-background-timeout-seconds N` |
| Interactive DXC deadline | 15 seconds | `--analysis-interactive-timeout-ms N` or `--analysis-interactive-timeout-seconds N` |
| Request workers | 4 | `--request-workers N` |
| Request queue | 64 | `--request-queue-capacity N` |
| Translation units | 16 | `--translation-unit-count N` |
| Translation-unit estimate | 256 MiB | `--translation-unit-memory-mb N` |
| Include metadata entries | 512 | `--include-cache-count N` |
| Include metadata estimate | 8 MiB | `--include-cache-memory-mb N` |
| JSON-RPC payload | 16 MiB | fixed safety limit |
| JSON-RPC header line | 8 KiB | fixed safety limit |
| Private analysis-worker payload | 64 MiB | fixed safety limit |

Every numeric override must be positive, cache capacity must remain sufficient
for every worker, and overflowing memory or duration values are rejected before
startup. `hlsl-lsp --help` prints the complete option list.
Translation-unit memory is an estimate because DXC does not expose its native
allocation size.

## Version and configuration compatibility

The server, Visual Studio extension, and VS Code extension use the same
semantic version. A major version may remove protocol capabilities, custom
notifications, command-line options, or configuration fields. Minor versions
may add capabilities or configuration fields. Patch versions contain
compatible fixes.

`shadertoolsconfig.json` follows the documented HLSL Tools-compatible fields.
New optional fields are additive within a major version. Existing fields do
not change type or meaning within a major version. Invalid known fields fail
with their configuration location instead of silently falling back. See
[`shadertoolsconfig.md`](shadertoolsconfig.md) for precedence and file-group
semantics.

The server targets LSP 3.17 behavior used by the bundled clients. Custom
`hlsl/didChangeClientDefaults` notifications are versioned with the server and
remain backward compatible within a major release.

## Fuzzing

Clang libFuzzer harnesses cover framing, JSON message decoding, file URI/path
conversion, and incremental document edits. Build and run them on Linux:

```bash
cmake -S . -B out/build/fuzz -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBUILD_TESTING=OFF \
  -DHLSL_BUILD_FUZZERS=ON
cmake --build out/build/fuzz
out/build/fuzz/hlsl-framing-fuzzer
out/build/fuzz/hlsl-json-fuzzer
out/build/fuzz/hlsl-uri-fuzzer
out/build/fuzz/hlsl-edits-fuzzer
```

CI performs bounded smoke runs under AddressSanitizer and
UndefinedBehaviorSanitizer. Longer local runs can reuse libFuzzer corpus
directories without committing source code.

## Representative shader coverage

First-party fixtures exercise modern DXC/HLSL 2021 and Shader Model 6.6,
Unreal-style `.usf`/`.ush` virtual includes, extracted Unity-style HLSL
preprocessor conventions, and Vulkan HLSL attributes. Compilation-only
`-spirv` and `-fspv-*` switches are omitted when invoking the IntelliSense API,
which does not accept them; the source syntax and Vulkan attributes are still
analyzed.

Unity ShaderLab containers are intentionally scoped out: HLSL-LSP handles
standalone HLSL documents and does not parse or map embedded `HLSLPROGRAM`
regions in `.shader` files. `tests/corpus/unity/Embedded.shader` records that
boundary but is not sent directly to DXC. Supporting it requires a separate
host-language parser and source mapping layer.

The structural analysis benchmark also generates a compiler-clean shader with
thousands of declarations at runtime. This covers ordinary large-translation-
unit latency and diagnostic quality without checking engine-owned shader
source into the repository. Deterministic timeout, crash, cancellation, and
worker-replacement tests use the private test helper because a real compiler
hang must not be required for a reliable test suite.

## Reproducible artifacts

Release C++ binaries use reproducible compiler/linker flags and are rebuilt in
a second directory for byte comparison. Linux archives use sorted entries, a
commit-derived timestamp, and normalized ownership. VSIX files are rewritten
with sorted entries and fixed ZIP metadata before upload. Release checksums
can therefore be compared across trusted rebuilds from the same commit and
toolchain image.
