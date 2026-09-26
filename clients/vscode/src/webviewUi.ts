export type AnalysisStatusTone =
  "success" | "warning" | "error" | "info" | "neutral";

export interface AnalysisSummaryItem {
  readonly label: string;
  readonly value: string;
  readonly tone?: AnalysisStatusTone;
}

function escapeHtml(value: string): string {
  return value
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;")
    .replaceAll("'", "&#39;");
}

export function analysisSummaryHtml(
  items: readonly AnalysisSummaryItem[],
): string {
  return `<section class="analysis-summary" aria-label="Analysis summary">
${items
  .map(
    (item) =>
      `<div class="summary-item ${item.tone ?? "neutral"}"><span class="summary-label">${escapeHtml(item.label)}</span><strong>${escapeHtml(item.value)}</strong></div>`,
  )
  .join("")}
</section>`;
}

export function analysisDetailsHtml(
  key: string,
  title: string,
  body: string,
  options: { readonly meta?: string; readonly open?: boolean } = {},
): string {
  const meta =
    options.meta === undefined
      ? ""
      : `<span class="detail-meta">${escapeHtml(options.meta)}</span>`;
  return `<details data-state-key="${escapeHtml(key)}"${options.open === true ? " open" : ""}>
<summary><span>${escapeHtml(title)}</span>${meta}</summary>
<div class="detail-body">${body}</div>
</details>`;
}

export function analysisFilterHtml(
  key: string,
  label: string,
  placeholder: string,
): string {
  const escapedKey = escapeHtml(key);
  return `<div class="analysis-filter">
<label for="${escapedKey}">${escapeHtml(label)}</label>
<input id="${escapedKey}" type="search" placeholder="${escapeHtml(placeholder)}" data-filter-input="${escapedKey}" autocomplete="off">
<span class="filter-count" data-filter-count="${escapedKey}" role="status" aria-live="polite"></span>
</div>`;
}

export function filterableAttributes(key: string, text: string): string {
  return `data-filter-group="${escapeHtml(key)}" data-filter-text="${escapeHtml(text.toLocaleLowerCase("en-US"))}"`;
}

export const analysisWebviewStyles = `
  .analysis-summary { display:grid; grid-template-columns:repeat(auto-fit,minmax(9rem,1fr)); gap:.55rem; margin:.75rem 0 1rem; }
  .summary-item { border:1px solid var(--vscode-panel-border); border-left-width:4px; border-radius:4px; padding:.55rem .65rem; background:var(--vscode-editor-inactiveSelectionBackground); }
  .summary-item.success { border-left-color:var(--vscode-testing-iconPassed, #73c991); }
  .summary-item.warning { border-left-color:var(--vscode-editorWarning-foreground, #cca700); }
  .summary-item.error { border-left-color:var(--vscode-testing-iconFailed, #f14c4c); }
  .summary-item.info { border-left-color:var(--vscode-textLink-foreground, #3794ff); }
  .summary-item.neutral { border-left-color:var(--vscode-descriptionForeground); }
  .summary-label { display:block; margin-bottom:.2rem; color:var(--vscode-descriptionForeground); font-size:.82rem; }
  details { max-width:70rem; margin:.55rem 0; border:1px solid var(--vscode-panel-border); border-radius:4px; }
  summary { display:flex; align-items:baseline; justify-content:space-between; gap:1rem; padding:.6rem .7rem; cursor:pointer; font-weight:600; }
  summary:hover { background:var(--vscode-list-hoverBackground); }
  summary:focus-visible, input:focus-visible, a:focus-visible { outline:1px solid var(--vscode-focusBorder); outline-offset:2px; }
  .detail-meta { color:var(--vscode-descriptionForeground); font-size:.85rem; font-weight:400; }
  .detail-body { padding:0 .75rem .75rem; overflow-x:auto; }
  .detail-body > section { margin:0; }
  .detail-body > section > h2:first-child { display:none; }
  .analysis-filter { display:flex; flex-wrap:wrap; align-items:center; gap:.45rem; margin:.35rem 0 .65rem; }
  .analysis-filter label { font-weight:600; }
  .analysis-filter input { min-width:min(26rem,100%); flex:1; color:var(--vscode-input-foreground); background:var(--vscode-input-background); border:1px solid var(--vscode-input-border, var(--vscode-panel-border)); padding:.35rem .5rem; }
  .filter-count { min-width:5rem; color:var(--vscode-descriptionForeground); font-size:.85rem; text-align:right; }
  [hidden] { display:none !important; }
  .status-badge { display:inline-block; padding:.05rem .4rem; border:1px solid currentColor; border-radius:.7rem; font-size:.78rem; white-space:nowrap; }
  .status-badge.success { color:var(--vscode-testing-iconPassed, #73c991); }
  .status-badge.warning { color:var(--vscode-editorWarning-foreground, #cca700); }
  .status-badge.error { color:var(--vscode-testing-iconFailed, #f14c4c); }
  .status-badge.info { color:var(--vscode-textLink-foreground, #3794ff); }
  .status-badge.neutral { color:var(--vscode-descriptionForeground); }
`;

export const analysisWebviewScript = `<script>
(() => {
  const vscode = acquireVsCodeApi();
  const previous = vscode.getState() || {};
  const detailsState = previous.details || {};
  const filters = previous.filters || {};
  let scrollY = Number.isFinite(previous.scrollY) ? previous.scrollY : 0;
  let scrollFrame;

  const save = () => vscode.setState({ details: detailsState, filters, scrollY });
  const applyFilter = (input) => {
    const key = input.dataset.filterInput;
    if (!key) return;
    const query = input.value.trim().toLocaleLowerCase("en-US");
    let visible = 0;
    let total = 0;
    document.querySelectorAll("[data-filter-group]").forEach((item) => {
      if (item.dataset.filterGroup !== key) return;
      total += 1;
      const matches = query === "" || (item.dataset.filterText || "").includes(query);
      item.hidden = !matches;
      if (matches) visible += 1;
    });
    const count = document.querySelector('[data-filter-count="' + key + '"]');
    if (count) count.textContent = query === "" ? total + " items" : visible + " of " + total;
  };

  document.querySelectorAll("details[data-state-key]").forEach((details) => {
    const key = details.dataset.stateKey;
    if (!key) return;
    if (Object.prototype.hasOwnProperty.call(detailsState, key)) {
      details.open = detailsState[key] === true;
    }
    details.addEventListener("toggle", () => {
      detailsState[key] = details.open;
      save();
    });
  });

  document.querySelectorAll("input[data-filter-input]").forEach((input) => {
    const key = input.dataset.filterInput;
    if (!key) return;
    input.value = typeof filters[key] === "string" ? filters[key] : "";
    applyFilter(input);
    input.addEventListener("input", () => {
      filters[key] = input.value;
      applyFilter(input);
      save();
    });
  });

  window.addEventListener("scroll", () => {
    if (scrollFrame !== undefined) cancelAnimationFrame(scrollFrame);
    scrollFrame = requestAnimationFrame(() => {
      scrollY = window.scrollY;
      save();
    });
  }, { passive: true });
  requestAnimationFrame(() => window.scrollTo(0, scrollY));
})();
</script>`;
