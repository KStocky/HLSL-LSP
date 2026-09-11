# Analysis result freshness

Every reusable custom analysis view in both editor clients reports the same
result-freshness state:

- **Current** — the displayed result is the latest accepted response for the
  view's tracked target.
- **Refreshing** — a replacement request is pending. The last good result
  remains visible.
- **Stale** — a known change invalidated the displayed result and no accepted
  replacement has completed.
- **Refresh failed** — the latest accepted refresh failed or was cancelled.
  The last good result remains visible when one exists.

When known, the indicator also reports **Source edit**, **Variant change**,
**Configuration change**, **Disconnected server**, or **Manual refresh**.
Otherwise the cause is **Unknown**. Successful protocol responses are
**Current** even when their authoritative result is “not applicable”, “not
found”, or “compilation failed”; those are analysis outcomes, not transport
failures.

When several triggers coalesce behind an explicit request, the retained cause
uses deterministic precedence: disconnected server, variant change,
configuration change, source edit, manual refresh, then unknown.

Each view has a **Refresh** action and an explicit target mode:

- **Pin this shader** is the default. The view keeps its current shader (and,
  for symbol-specific views, its tracked position or root) while editor focus
  moves elsewhere.
- **Follow active shader** retargets the existing view whenever another HLSL
  editor becomes active. It never creates a duplicate view. If no HLSL editor
  is active, the last useful result stays visible and is marked stale.

The header always identifies the tracked file and offers the opposite mode as
an action, so switching tabs cannot silently change the meaning of a pinned
result. **Refresh** reuses the target selected by that mode. Memory Layout and
Call Hierarchy rebuild their position-sensitive target from the active caret
when following; Compute Visualization retains its submitted options. Closing a
tracked document keeps the last good content visible but marks it stale.
Selected modes survive ordinary refreshes and supported editor view
restoration.

Rapid edits are coalesced, and generation guards prevent an older, cancelled,
or superseded request from restoring **Current** or overwriting a newer result.

The VS Code views remain script-free. Their Refresh and target-mode links are
static `command:` URIs, and each webview allowlists only those commands plus the
view's existing navigation/export commands.
