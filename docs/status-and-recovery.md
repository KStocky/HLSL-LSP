# HLSL-LSP status and recovery

Both editor clients provide a compact status indicator and a shared status
surface for diagnosing activation, runtime, configuration, and analysis
problems without reading protocol traces.

Open the surface with **HLSL > Status and Recovery** in an HLSL editor. In
Visual Studio Code, the same command is available as **HLSL: Show Status** and
from the HLSL status-bar item.

The surface reports:

- Language-server lifecycle and connection state.
- Active DXC source and compiler version, including a pending runtime restart.
- Active shader file, variant, entry point, and target profile.
- Effective configuration provenance when available.
- The last actionable client or activation failure.

Healthy state remains a single compact status indicator. Warning and stopped
states remain visible until recovery succeeds so a transient failure is not
lost when a notification closes.

## Recovery actions

- **Restart** restarts the HLSL language server and reapplies editor settings.
- **Open Output** opens the editor's HLSL-LSP output or diagnostics log.
- **Open Configuration** opens the effective file-backed
  `shadertoolsconfig.json` for the active shader when one exists.
- **Copy Diagnostics** copies a concise report suitable for a bug report.
- **Refresh** queries the current lifecycle, runtime, and shader context again.

Copied diagnostics never contain shader source. The clients omit full shader
and configuration paths and redact absolute paths from failure text. Full paths
can still appear in the local editor output when they are needed to repair a
local configuration, but that output is not copied automatically.

The status surface is diagnostic rather than a second configuration system.
Runtime and shader-context values come from the same server requests used by
the rest of the extension, and configuration changes must still be made in
editor settings or `shadertoolsconfig.json`.
