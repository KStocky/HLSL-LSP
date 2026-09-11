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

Each view has a **Refresh** action. It always reuses that view's tracked target,
never the active editor: document views retain their URI, Memory Layout retains
its symbol position/tracking point, Call Hierarchy retains its root, and Compute
Visualization retains its last submitted options. Rapid edits are coalesced,
and generation guards prevent an older, cancelled, or superseded request from
restoring **Current** or overwriting a newer result.

The VS Code views remain script-free. Their Refresh links are static
`command:` URIs, and each webview allowlists only its own refresh command plus
the view's existing navigation/export commands.
