export interface MemoryLayoutMember {
  readonly name: string;
  readonly type: string;
  readonly kind: "scalar" | "vector" | "matrix" | "array" | "record";
  readonly offset: number;
  readonly size: number;
  readonly allocationSize: number;
  readonly alignment: number;
  readonly paddingBefore: number;
  readonly arrayIndex?: number;
  readonly arrayStride?: number;
  readonly arrayDimensions?: readonly number[];
  readonly matrixStride?: number;
  readonly rowMajor?: boolean;
  readonly members: readonly MemoryLayoutMember[];
}

export interface MemoryLayout {
  readonly name: string;
  readonly type: string;
  readonly mode: "natural" | "constantBuffer";
  readonly size: number;
  readonly alignment: number;
  readonly allocationSize: number;
  readonly members: readonly MemoryLayoutMember[];
  readonly diagnostics: readonly string[];
}

export interface MemoryLayoutSegment {
  readonly name: string;
  readonly offset: number;
  readonly size: number;
  readonly depth: number;
  readonly kind:
    "value" | "internal-padding" | "inter-member-padding" | "trailing-padding";
}

function escapeHtml(value: string): string {
  return value
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;")
    .replaceAll("'", "&#39;");
}

function flattenMembers(
  members: readonly MemoryLayoutMember[],
  baseOffset = 0,
  depth = 0,
  parentName = "",
  parentKind?: MemoryLayoutMember["kind"],
  parentRowMajor = false,
  extent?: number,
): MemoryLayoutSegment[] {
  const result: MemoryLayoutSegment[] = [];
  let previousEnd = 0;
  for (const member of members) {
    const offset = baseOffset + member.offset;
    if (member.offset > previousEnd) {
      result.push({
        name:
          parentKind === "array" || parentKind === "matrix"
            ? "internal padding"
            : "inter-member padding",
        offset: baseOffset + previousEnd,
        size: member.offset - previousEnd,
        depth,
        kind:
          parentKind === "array" || parentKind === "matrix"
            ? "internal-padding"
            : "inter-member-padding",
      });
    }
    const indexedName =
      parentKind === "matrix"
        ? `${parentName}.${parentRowMajor ? "row" : "column"}${member.name}`
        : `${parentName}${member.name}`;
    const name =
      parentName.length === 0
        ? member.name
        : member.name.startsWith("[")
          ? indexedName
          : `${parentName}.${member.name}`;
    if (member.members.length === 0) {
      result.push({
        name,
        offset,
        size: member.size,
        depth,
        kind: "value",
      });
      if (member.allocationSize > member.size) {
        result.push({
          name: "internal padding",
          offset: offset + member.size,
          size: member.allocationSize - member.size,
          depth: depth + 1,
          kind: "internal-padding",
        });
      }
    } else {
      result.push(
        ...flattenMembers(
          member.members,
          offset,
          depth + 1,
          name,
          member.kind,
          member.rowMajor,
          member.allocationSize,
        ),
      );
    }
    previousEnd = Math.max(previousEnd, member.offset + member.allocationSize);
  }
  if (extent !== undefined && extent > previousEnd) {
    result.push({
      name: "trailing padding",
      offset: baseOffset + previousEnd,
      size: extent - previousEnd,
      depth,
      kind: "trailing-padding",
    });
  }
  return result;
}

export function memoryLayoutSegments(
  layout: MemoryLayout,
): MemoryLayoutSegment[] {
  return flattenMembers(
    layout.members,
    0,
    0,
    "",
    undefined,
    false,
    layout.allocationSize,
  );
}

function rowDiagram(layout: MemoryLayout): string {
  const rowSize = 16;
  const total = Math.max(layout.allocationSize, layout.size, rowSize);
  const rows = Math.ceil(total / rowSize);
  const segments = memoryLayoutSegments(layout);
  const blocks: string[] = [];
  for (let row = 0; row < rows; ++row) {
    const start = row * rowSize;
    const end = start + rowSize;
    const rowBlocks = segments
      .filter(
        (segment) =>
          segment.size > 0 &&
          segment.offset < end &&
          segment.offset + segment.size > start,
      )
      .map((segment) => {
        const segmentStart = Math.max(segment.offset, start);
        const segmentEnd = Math.min(segment.offset + segment.size, end);
        const left = ((segmentStart - start) / rowSize) * 100;
        const width = ((segmentEnd - segmentStart) / rowSize) * 100;
        const title = `${segment.name}: offset ${String(segment.offset)}, ${String(segment.size)} bytes`;
        return `<div class="block ${segment.kind} depth-${String(segment.depth % 5)}" style="left:${String(left)}%;width:${String(width)}%" title="${escapeHtml(title)}">${escapeHtml(segment.name)}</div>`;
      })
      .join("");
    blocks.push(
      `<div class="row"><div class="offset">${String(start)}</div><div class="bytes">${rowBlocks}</div></div>`,
    );
  }
  return blocks.join("");
}

function memberRows(
  members: readonly MemoryLayoutMember[],
  baseOffset = 0,
  depth = 0,
): string {
  return members
    .map((member) => {
      const absoluteOffset = baseOffset + member.offset;
      const row = `<tr><td style="padding-left:${String(depth * 1.25 + 0.5)}rem">${escapeHtml(member.name)}</td><td><code>${escapeHtml(member.type)}</code></td><td>${String(absoluteOffset)}</td><td>${String(member.size)}</td><td>${String(member.alignment)}</td><td>${String(member.paddingBefore)}</td></tr>`;
      return row + memberRows(member.members, absoluteOffset, depth + 1);
    })
    .join("");
}

export function memoryLayoutHtml(layout: MemoryLayout): string {
  const mode =
    layout.mode === "constantBuffer"
      ? "Constant-buffer packing"
      : "Natural / structured-buffer layout";

  const hasContent = layout.members.length > 0;

  const diagnostics =
    layout.diagnostics.length === 0
      ? ""
      : `<section class="diagnostics">${layout.diagnostics.map((message) => `<p>${escapeHtml(message)}</p>`).join("")}</section>`;

  const summary = hasContent
    ? `<div class="summary">${mode} · size ${String(layout.size)} bytes · alignment ${String(layout.alignment)} bytes · allocation ${String(layout.allocationSize)} bytes</div>`
    : `<div class="summary">${mode}</div>`;

  const diagram = hasContent
    ? `<div class="diagram">${rowDiagram(layout)}</div>`
    : "";

  const table = hasContent
    ? `<table>
<thead><tr><th>Member</th><th>Type</th><th>Offset</th><th>Size</th><th>Alignment</th><th>Padding before</th></tr></thead>
<tbody>${memberRows(layout.members)}</tbody>
</table>`
    : "";

  return `<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
  body { color: var(--vscode-foreground); background: var(--vscode-editor-background); font-family: var(--vscode-font-family); padding: 1rem 1.5rem; }
  h1 { font-size: 1.35rem; margin: 0 0 .25rem; }
  .summary { color: var(--vscode-descriptionForeground); margin-bottom: 1.25rem; }
  .diagram { max-width: 70rem; margin: 1rem 0 1.5rem; }
  .row { display: grid; grid-template-columns: 4rem 1fr; min-height: 2.25rem; margin-bottom: .25rem; }
  .offset { text-align: right; padding: .55rem .75rem 0 0; color: var(--vscode-descriptionForeground); font-family: var(--vscode-editor-font-family); }
  .bytes { position: relative; border: 1px solid var(--vscode-panel-border); background: repeating-linear-gradient(90deg, transparent 0, transparent calc(25% - 1px), var(--vscode-panel-border) calc(25% - 1px), var(--vscode-panel-border) 25%); }
  .block { position: absolute; box-sizing: border-box; height: 100%; overflow: hidden; padding: .45rem .35rem; border: 2px solid; text-align: center; white-space: nowrap; text-overflow: ellipsis; background: color-mix(in srgb, var(--vscode-symbolIcon-fieldForeground) 20%, var(--vscode-editor-background)); }
  .depth-0 { border-color: var(--vscode-symbolIcon-fieldForeground); }
  .depth-1 { border-color: var(--vscode-symbolIcon-structForeground); }
  .depth-2 { border-color: var(--vscode-symbolIcon-variableForeground); }
  .depth-3 { border-color: var(--vscode-symbolIcon-arrayForeground); }
  .depth-4 { border-color: var(--vscode-symbolIcon-numberForeground); }
  .internal-padding { border: 1px dashed var(--vscode-descriptionForeground); background: repeating-linear-gradient(135deg, transparent 0, transparent 5px, color-mix(in srgb, var(--vscode-descriptionForeground) 18%, transparent) 5px, color-mix(in srgb, var(--vscode-descriptionForeground) 18%, transparent) 8px); color: var(--vscode-descriptionForeground); }
  .inter-member-padding { border: 1px dotted var(--vscode-editorWarning-foreground); background: color-mix(in srgb, var(--vscode-editorWarning-foreground) 10%, transparent); color: var(--vscode-descriptionForeground); }
  .trailing-padding { border: 1px dotted var(--vscode-disabledForeground); background: color-mix(in srgb, var(--vscode-disabledForeground) 10%, transparent); color: var(--vscode-descriptionForeground); }
  table { border-collapse: collapse; width: 100%; max-width: 70rem; }
  th, td { border-bottom: 1px solid var(--vscode-panel-border); padding: .45rem .5rem; text-align: left; }
  th { color: var(--vscode-descriptionForeground); }
  .diagnostics { border-left: 3px solid var(--vscode-editorWarning-foreground); padding-left: .75rem; margin: 1rem 0; }
</style>
</head>
<body>
<h1>${escapeHtml(layout.name || layout.type)}</h1>
${summary}
${diagnostics}
${diagram}
${table}
</body>
</html>`;
}
