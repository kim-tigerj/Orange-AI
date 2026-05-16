let activeRun = null;
let activeView = "logs";
let autotestSocket = null;
let llmQuerySocket = null;
let logRows = [];
let autotestRows = [];
let questionTouched = false;
const VIEW_KEY = "q2.activeView";

function rememberView(view) {
  activeView = view;
  try {
    localStorage.setItem(VIEW_KEY, view);
  } catch (_err) {
  }
}

function rememberedView() {
  try {
    return localStorage.getItem(VIEW_KEY) || "localCompare";
  } catch (_err) {
    return "localCompare";
  }
}

async function api(path, body, timeoutMs = 60000) {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), timeoutMs);
  const options = body ? {
    method: "POST",
    headers: {"Content-Type": "application/json"},
    body: JSON.stringify(body),
    signal: controller.signal
  } : {signal: controller.signal};
  try {
    const res = await fetch(path, options);
    const text = await res.text();
    let data = {};
    try {
      data = text ? JSON.parse(text) : {};
    } catch (err) {
      throw new Error(`HTTP ${res.status}: ${text.slice(0, 400)}`);
    }
    if (!res.ok) throw new Error(`HTTP ${res.status}: ${text.slice(0, 400)}`);
    return data;
  } catch (err) {
    if (err.name === "AbortError") throw new Error(`${Math.round(timeoutMs / 1000)}초 동안 응답이 없습니다.`);
    throw err;
  } finally {
    clearTimeout(timer);
  }
}

function pretty(value) {
  return JSON.stringify(value ?? null, null, 2);
}

function escapeHtml(value) {
  return String(value ?? "")
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;");
}

function formatValue(value) {
  if (value === null || value === undefined) return "";
  if (typeof value === "number") {
    return Number.isInteger(value) ? value.toLocaleString() : value.toLocaleString(undefined, {maximumFractionDigits: 2});
  }
  if (typeof value === "object") return JSON.stringify(value);
  return String(value);
}

function isTimeColumn(name) {
  const col = String(name || "").toLowerCase();
  if (/elapsed|execution|total_ms|time_ms|duration|timeout/.test(col)) return false;
  if (["시간", "시각", "일시", "생성일", "수정일"].includes(String(name || ""))) return true;
  return /(^|_)(created|updated|timestamp|lasttime|eventtime|createtime|installtime|starttime|endtime|date|time)($|_)/.test(col)
    || /time$/.test(col);
}

function formatLocalDateTime(value) {
  if (value === null || value === undefined || value === "") return "";
  let date = null;
  if (typeof value === "number") {
    if (value > 100000000000) date = new Date(value);
    else if (value > 1000000000) date = new Date(value * 1000);
  } else if (typeof value === "string") {
    const trimmed = value.trim();
    const hasTimezone = /(?:z|[+-]\d{2}:?\d{2})$/i.test(trimmed);
    const isoLike = /^\d{4}-\d{2}-\d{2}t\d{2}:\d{2}/i.test(trimmed);
    const parsed = new Date(isoLike && !hasTimezone ? `${trimmed}Z` : trimmed);
    if (!Number.isNaN(parsed.getTime())) date = parsed;
  }
  if (!date || Number.isNaN(date.getTime())) return formatValue(value);
  const pad = number => String(number).padStart(2, "0");
  return [
    date.getFullYear(),
    pad(date.getMonth() + 1),
    pad(date.getDate())
  ].join("-") + " " + [
    pad(date.getHours()),
    pad(date.getMinutes()),
    pad(date.getSeconds())
  ].join(":");
}

function formatCellValue(column, value) {
  return isTimeColumn(column) ? formatLocalDateTime(value) : formatValue(value);
}

function formatTableValue(col, value) {
  if (col.format === "time") return formatLocalDateTime(value);
  return formatCellValue(col.key || col.label, value);
}

function orderResultColumns(cols) {
  const preferred = [
    "node_label",
    "node_user",
    "node_computer",
    "node_ip",
    "id",
    "product",
    "ProductName",
    "ProductVersion",
    "FileVersion",
    "CompanyName",
    "node_os",
    "node_manufacturer",
    "node_model"
  ];
  const seen = new Set();
  const ordered = [];
  preferred.forEach(col => {
    if (cols.includes(col) && !seen.has(col)) {
      ordered.push(col);
      seen.add(col);
    }
  });
  cols.forEach(col => {
    if (!seen.has(col)) ordered.push(col);
  });
  return ordered;
}

function statusOf(row) {
  if (row.expected_match === false) return "mismatch";
  if (row.quality_status) return row.quality_status;
  return row.ok ? "ok" : "failed";
}

function toggle(id) {
  document.getElementById(id).classList.toggle("hidden");
}

function hideSidePanels() {
  document.getElementById("candidatePanel").classList.add("hidden");
  document.getElementById("logPanel").classList.add("hidden");
}

function wsUrl(path) {
  const protocol = window.location.protocol === "https:" ? "wss:" : "ws:";
  return `${protocol}//${window.location.host}${path}`;
}

function stopAutotestLive() {
  if (autotestSocket) {
    autotestSocket.close();
    autotestSocket = null;
  }
}

function stopLogLive() {
  if (llmQuerySocket) {
    llmQuerySocket.close();
    llmQuerySocket = null;
  }
}

function stopLiveExcept(view) {
  if (view !== "autotest") stopAutotestLive();
}

function startAutotestLive() {
  if (autotestSocket && autotestSocket.readyState <= 1) return;
  autotestSocket = new WebSocket(wsUrl("/ws/autotest"));
  autotestSocket.onmessage = event => {
    const data = JSON.parse(event.data);
    document.getElementById("meta").textContent = `${data.state || "status"} / completed=${data.completed ?? 0} / updated=${formatLocalDateTime(data.updated_at) || "-"}`;
    renderAutotestView(data);
  };
  autotestSocket.onclose = () => {
    if (activeView === "autotest") {
      document.getElementById("meta").textContent = "자동 테스트 실시간 연결 종료";
    }
  };
}

function startLogLive() {
  if (llmQuerySocket && llmQuerySocket.readyState <= 1) return;
  llmQuerySocket = new WebSocket(wsUrl("/ws/llm-query"));
  llmQuerySocket.onmessage = event => {
    const payload = JSON.parse(event.data);
    const rows = payload.type === "snapshot" ? (payload.rows || []) : [payload];
    rows.forEach(row => {
      if (!row || !row.question) return;
      const key = `${row.created_at || ""}:${row.question}`;
      if (logRows.some(existing => `${existing.created_at || ""}:${existing.question}` === key)) return;
      logRows.unshift(row);
    });
    logRows = logRows.slice(0, 100);
    setDefaultQuestionFromRows(logRows);
    if (activeView === "logs") renderLogView(logRows);
    renderLogSide(logRows);
    if (activeView === "logs") document.getElementById("meta").textContent = `logs=${logRows.length} / live`;
  };
  llmQuerySocket.onclose = () => {
    if (activeView === "logs") {
      document.getElementById("meta").textContent = "최근 로그 실시간 연결 종료";
    }
    setTimeout(startLogLive, 3000);
  };
}

async function bootstrapLogLive() {
  try {
    const data = await api("/api/llm-query/recent?limit=100", null, 15000);
    logRows = data.rows || [];
    setDefaultQuestionFromRows(logRows);
    renderLogSide(logRows);
  } catch (_err) {
  }
  startLogLive();
}

function setCliResult(text, state = "") {
  const result = document.getElementById("cliResult");
  result.className = `cli-result-line ${state}`.trim();
  result.textContent = text;
}

function resizeQuestionInput() {
  const input = document.getElementById("question");
  input.style.height = "auto";
  input.style.height = `${Math.min(input.scrollHeight, 150)}px`;
}

function latestQuestionFromRows(rows) {
  const row = (rows || []).find(item => item && item.question);
  return row ? String(row.question || "").trim() : "";
}

function setDefaultQuestionFromRows(rows, force = false) {
  const question = latestQuestionFromRows(rows);
  if (!question) return;
  const input = document.getElementById("question");
  if (!force && questionTouched && input.value.trim()) return;
  input.value = question;
  input.placeholder = "자연어 질의 입력";
  resizeQuestionInput();
}

function setActiveStats(data) {
  const plan = data.plan || {};
  const rowCount = data.result_count ?? (data.result_preview ? data.result_preview.length : 0);
  document.getElementById("activeCollection").textContent = plan.collection || "-";
  document.getElementById("activeRows").textContent = rowCount == null ? "-" : formatValue(rowCount);
  document.getElementById("activeQuality").textContent = data.quality_status || (data.ok ? "ok" : "failed");
  document.getElementById("activeTime").textContent = data.execution_ms == null ? "-" : `${formatValue(data.execution_ms)}ms`;
}

function setDefaultPanelTitles() {
  document.getElementById("leftPanelTitle").textContent = "Natural Language CLI";
}

function localLmNextActions(evalRows) {
  const failed = (evalRows || []).filter(row => !row.semantic_same);
  if (!failed.length) return "대표 평가 세트는 모두 통과했습니다. 다음은 평가 질문 수를 늘려야 합니다.";
  return failed.map(row => {
    const comparison = row.comparison || {};
    const reason = ["collection_match", "group_by_match", "metric_semantic_match", "filter_match"]
      .filter(key => comparison[key] === false)
      .join(", ") || "plan mismatch";
    return `${row.question}: ${row.reference_collection || "-"} -> ${row.local_collection || "-"} / ${reason}`;
  }).join("\n");
}

function renderDataTable(title, meta, rows, mode = "result") {
  document.getElementById("dataTitle").textContent = title;
  document.getElementById("dataMeta").textContent = meta || "";
  const target = document.getElementById("dataGrid");
  if (!rows || !rows.length) {
    target.className = "table-empty";
    target.textContent = "조회 결과가 없습니다.";
    return;
  }

  let cols;
  if (mode === "logs") {
    cols = ["created_at", "ok", "repeat_count", "quality_status", "collection", "result_count", "total_ms", "question"];
  } else {
    cols = orderResultColumns([...new Set(rows.flatMap(row => Object.keys(row)))]);
  }

  target.className = "table-wrap";
  const labelFor = col => mode === "logs" && col === "created_at" ? "발생 시각" : col;
  target.innerHTML = `
    <table>
      <thead><tr>${cols.map(col => `<th>${escapeHtml(labelFor(col))}</th>`).join("")}</tr></thead>
      <tbody>
        ${rows.map(row => `
          <tr>
            ${cols.map(col => {
              const value = col === "ok" ? (row[col] ? "OK" : "FAIL") : row[col];
              const cls = col === "ok" ? ` class="status-cell ${row[col] ? "status-ok" : "status-failed"}"` : "";
              if (mode === "logs" && col === "question") {
                return `<td><button class="query-link" data-question="${escapeHtml(row[col] || "")}">${escapeHtml(formatCellValue(col, value))}</button></td>`;
              }
              return `<td${cls}>${escapeHtml(formatCellValue(col, value))}</td>`;
            }).join("")}
          </tr>
        `).join("")}
      </tbody>
    </table>
  `;
  wireQueryLinks(target);
}

function renderFixedTable(title, meta, rows, cols) {
  document.getElementById("dataTitle").textContent = title;
  document.getElementById("dataMeta").textContent = meta || "";
  const target = document.getElementById("dataGrid");
  if (!rows || !rows.length) {
    target.className = "table-empty";
    target.textContent = "표시할 데이터가 없습니다.";
    return;
  }

  target.className = "table-wrap";
  target.innerHTML = `
    <table>
      <thead><tr>${cols.map(col => `<th>${escapeHtml(col.label)}</th>`).join("")}</tr></thead>
      <tbody>
        ${rows.map(row => {
          const queryCol = cols.find(col => col.query);
          const queryValue = queryCol ? queryCol.value(row) : "";
          return `
          <tr${queryValue ? ` class="query-row" data-question="${escapeHtml(queryValue)}"` : ""}>
            ${cols.map(col => {
              const value = col.value(row);
              if (col.query) {
                return `<td><button class="query-link" data-question="${escapeHtml(value || "")}">${escapeHtml(formatTableValue(col, value))}</button></td>`;
              }
              return `<td>${escapeHtml(formatTableValue(col, value))}</td>`;
            }).join("")}
          </tr>
        `}).join("")}
      </tbody>
    </table>
  `;
  wireQueryLinks(target);
}

function renderTableInto(targetId, titleId, metaId, title, meta, rows, cols) {
  document.getElementById(titleId).textContent = title;
  document.getElementById(metaId).textContent = meta || "";
  const target = document.getElementById(targetId);
  if (!rows || !rows.length) {
    target.className = "table-empty";
    target.textContent = "표시할 데이터가 없습니다.";
    return;
  }
  target.className = "table-wrap";
  target.innerHTML = `
    <table>
      <thead><tr>${cols.map(col => `<th>${escapeHtml(col.label)}</th>`).join("")}</tr></thead>
      <tbody>
        ${rows.map(row => `
          <tr>
            ${cols.map(col => {
              const value = col.value(row);
              if (col.query) {
                return `<td><button class="query-link" data-question="${escapeHtml(value || "")}">${escapeHtml(formatTableValue(col, value))}</button></td>`;
              }
              return `<td>${escapeHtml(formatTableValue(col, value))}</td>`;
            }).join("")}
          </tr>
        `).join("")}
      </tbody>
    </table>
  `;
  wireQueryLinks(target);
}

function clearSecondary(title = "상세 목록", meta = "") {
  document.getElementById("secondaryTitle").textContent = title;
  document.getElementById("secondaryMeta").textContent = meta;
  const target = document.getElementById("secondaryGrid");
  target.className = "table-empty";
  target.textContent = "표시할 상세 목록이 없습니다.";
}

function wireQueryLinks(root = document) {
  root.querySelectorAll(".query-link").forEach(btn => {
    btn.addEventListener("click", event => {
      event.stopPropagation();
      executeQuestion(btn.dataset.question || btn.textContent || "");
    });
  });
  root.querySelectorAll(".query-row").forEach(row => {
    row.addEventListener("click", () => executeQuestion(row.dataset.question || ""));
  });
}

function executeQuestion(question) {
  const input = document.getElementById("question");
  input.value = question;
  resizeQuestionInput();
  input.focus();
  run("query");
}

function collapseConsecutiveQueries(rows) {
  const collapsed = [];
  rows.forEach(row => {
    const prev = collapsed[collapsed.length - 1];
    if (prev && (prev.question || "") === (row.question || "")) {
      prev.repeat_count = (prev.repeat_count || 1) + 1;
      prev.total_ms = Math.max(Number(prev.total_ms || 0), Number(row.total_ms || 0));
      prev.execution_ms = Math.max(Number(prev.execution_ms || 0), Number(row.execution_ms || 0));
      return;
    }
    collapsed.push({...row, repeat_count: 1});
  });
  return collapsed;
}

function countBy(rows, field) {
  const counts = new Map();
  rows.forEach(row => {
    const value = row[field] ?? "unknown";
    counts.set(value, (counts.get(value) || 0) + 1);
  });
  return [...counts.entries()]
    .map(([label, value]) => ({label, value}))
    .sort((a, b) => b.value - a.value);
}

function renderBarChart(title, meta, series, qualityRows = null) {
  document.getElementById("chartTitle").textContent = title;
  document.getElementById("chartMeta").textContent = meta || "";
  const target = document.getElementById("chart");
  if (!series || !series.length) {
    target.className = "chart-empty";
    target.textContent = "차트로 표시할 데이터가 없습니다.";
    return;
  }
  const top = series.slice(0, 10);
  const total = top.reduce((sum, item) => sum + Number(item.value || 0), 0) || 1;
  const colors = ["#f97316", "#c2410c", "#92400e", "#78716c", "#475569", "#334155", "#166534", "#0f766e", "#7c2d12", "#3f3f46"];
  const radius = 84;
  const circumference = 2 * Math.PI * radius;
  let offset = 0;
  const segments = top.map((item, index) => {
    const value = Number(item.value || 0);
    const length = Math.max(0, (value / total) * circumference - 2.5);
    const gap = circumference - length;
    const segment = {
      ...item,
      color: colors[index % colors.length],
      dasharray: `${length.toFixed(3)} ${gap.toFixed(3)}`,
      dashoffset: (-offset).toFixed(3),
      rate: (value / total) * 100
    };
    offset += (value / total) * circumference;
    return segment;
  });
  target.className = "chart-wrap";
  target.innerHTML = `
    <div class="pie-layout">
      <div class="donut-shell">
        <svg class="donut-chart" viewBox="0 0 220 220" role="img" aria-label="${escapeHtml(title)}">
          <defs>
            <filter id="donutShadow" x="-20%" y="-20%" width="140%" height="140%">
              <feDropShadow dx="0" dy="7" stdDeviation="5" flood-color="#000000" flood-opacity="0.28"/>
            </filter>
          </defs>
          <circle class="donut-backdrop" cx="110" cy="110" r="${radius}"></circle>
          ${segments.map(item => `
            <circle
              class="donut-segment"
              cx="110"
              cy="110"
              r="${radius}"
              stroke="${item.color}"
              stroke-dasharray="${item.dasharray}"
              stroke-dashoffset="${item.dashoffset}"
            >
              <title>${escapeHtml(item.label)} ${escapeHtml(formatValue(item.value))} (${item.rate.toFixed(1)}%)</title>
            </circle>
          `).join("")}
        </svg>
        <div class="donut-center">
          <span>total</span>
          <strong>${escapeHtml(formatValue(total))}</strong>
        </div>
      </div>
      <div class="pie-legend">
      ${segments.map(item => `
        <div class="legend-row">
          <span class="legend-dot" style="background:${item.color}"></span>
          <span class="legend-label" title="${escapeHtml(item.label)}">${escapeHtml(item.label)}</span>
          <span class="legend-value">${escapeHtml(formatValue(item.value))}</span>
          <span class="legend-rate">${escapeHtml(item.rate.toFixed(1))}%</span>
        </div>
      `).join("")}
      </div>
    </div>
    ${qualityRows ? renderQualityBoxes(qualityRows) : ""}
  `;
}

function renderLogView(rows) {
  rememberView("logs");
  hideSidePanels();
  const displayRows = collapseConsecutiveQueries(rows);
  renderBarChart("최근 질의 컬렉션 분포", `최근 ${rows.length}건 / 표시 ${displayRows.length}줄`, countBy(rows, "collection"), rows);
  renderDataTable("최근 자연어 질의", `shown=${displayRows.length} / raw=${rows.length}`, displayRows, "logs");
}

function renderCandidateView(candidates) {
  rememberView("candidates");
  hideSidePanels();
  document.getElementById("candidateMeta").textContent = `shown=${candidates.length}`;
  renderBarChart("개선 후보 상태", `후보 ${candidates.length}건`, countBy(candidates, "status"));
  renderFixedTable("개선 후보 목록", `shown=${candidates.length}`, candidates, [
    {label: "status", value: row => row.status || "proposed"},
    {label: "collection", value: row => row.collection || ""},
    {label: "failure", value: row => row.failure_reason || ""},
    {label: "fix_type", value: row => (row.candidate_fix || {}).type || ""},
    {label: "positive", value: row => row.positive_probe_count ?? 0},
    {label: "question", value: row => row.question || "", query: true}
  ]);
}

function renderAutotestView(data) {
  rememberView("autotest");
  hideSidePanels();
  mergeAutotestRows(data.recent || []);
  const rows = autotestRows;
  const state = data.state || (data.completed ? "completed" : "idle");
  renderBarChart(
    "자동 테스트 결과",
    `${state} / completed=${data.completed ?? rows.length}`,
    countBy(rows.map(row => ({...row, status: statusOf(row)})), "status"),
    rows
  );
  renderFixedTable("자동 테스트 항목", `rows=${rows.length}`, rows, [
    {label: "status", value: row => statusOf(row)},
    {label: "collection", value: row => row.collection || ""},
    {label: "expected", value: row => (row.expected_collections || []).join(", ")},
    {label: "rows", value: row => row.result_count ?? ""},
    {label: "time_ms", value: row => row.elapsed_ms ?? ""},
    {label: "category", value: row => row.category || ""},
    {label: "question", value: row => row.question || "", query: true}
  ]);
}

function mergeAutotestRows(rows) {
  rows.forEach(row => {
    if (!row || !row.question) return;
    const key = `${row.category || ""}:${row.question}`;
    const index = autotestRows.findIndex(existing => `${existing.category || ""}:${existing.question}` === key);
    if (index >= 0) autotestRows[index] = row;
    else autotestRows.push(row);
  });
}

function renderLogSide(rows) {
  const target = document.getElementById("logs");
  const displayRows = collapseConsecutiveQueries(rows);
  document.getElementById("logMeta").textContent = `rows=${displayRows.length} / raw=${rows.length}`;
  target.className = "stack";
  target.innerHTML = displayRows.map(row => `
    <article class="candidate">
      <button class="query-link candidate-title" data-question="${escapeHtml(row.question || "")}">${escapeHtml(row.question || "")}</button>
      <div class="badges">
        <span class="badge orange">${escapeHtml(row.ok ? "OK" : "FAIL")}</span>
        <span class="badge">${escapeHtml(formatLocalDateTime(row.created_at) || "-")}</span>
        <span class="badge">${escapeHtml(formatValue(row.repeat_count || 1))}회</span>
        <span class="badge">${escapeHtml(row.collection || row.backend || "")}</span>
        <span class="badge">${escapeHtml(formatValue(row.result_count ?? 0))} rows</span>
        <span class="badge">${escapeHtml(formatValue(row.total_ms ?? row.execution_ms ?? 0))}ms</span>
      </div>
    </article>
  `).join("");
  wireQueryLinks(target);
  document.getElementById("logPanel").classList.remove("hidden");
}

function renderCacheView(data) {
  activeView = "cache";
  hideSidePanels();
  document.getElementById("meta").textContent = `cache=${data.exact_size} hits=${data.hits} misses=${data.misses}`;
  renderBarChart("Cache 상태", data.path || "", [
    {label: "exact_size", value: data.exact_size || 0},
    {label: "hits", value: data.hits || 0},
    {label: "misses", value: data.misses || 0}
  ]);
  renderFixedTable("Cache 상세", "planner cache", [data], [
    {label: "exact_size", value: row => row.exact_size},
    {label: "hits", value: row => row.hits},
    {label: "misses", value: row => row.misses},
    {label: "path", value: row => row.path || ""}
  ]);
}

function renderQualityBoxes(rows) {
  const counts = {ok: 0, failed: 0, needs_review: 0, mismatch: 0};
  rows.forEach(row => {
    const status = statusOf(row);
    if (counts[status] === undefined) counts[status] = 0;
    counts[status] += 1;
  });
  return `
    <div class="quality-row">
      <div class="quality-box"><span>OK</span><strong class="status-ok">${counts.ok || 0}</strong></div>
      <div class="quality-box"><span>Mismatch</span><strong class="status-mismatch">${counts.mismatch || 0}</strong></div>
      <div class="quality-box"><span>Needs Review</span><strong class="status-needs_review">${counts.needs_review || 0}</strong></div>
      <div class="quality-box"><span>Failed</span><strong class="status-failed">${counts.failed || 0}</strong></div>
    </div>
  `;
}

function firstExisting(cols, patterns) {
  return patterns
    .map(pattern => cols.find(col => pattern.test(col)))
    .find(Boolean);
}

function isDistributionQuestion(question) {
  return /분포|비중|현황|종류별|버전별|제조사별|사용자별|요일별|일별|월별|그룹별|별로/.test(question || "");
}

function pickChartLabelColumn(cols, plan, question) {
  const text = `${question || ""} ${(plan?.group_by || []).join(" ")}`.toLowerCase();
  if (/백신|안티바이러스|antivirus|av\b|보안제품/.test(text) && /노드|장비|pc|컴퓨터|서버|별/.test(text)) {
    return firstExisting(cols, [/^node_label$/i, /^display_name$/i, /^node_user$/i, /^node_computer$/i, /^computer$/i, /^csname$/i, /^id$/i]);
  }
  if (/윈도우|windows| os |운영체제|빌드/.test(` ${text} `)) {
    return firstExisting(cols, [/^data_Caption$/i, /caption/i, /build/i, /os/i, /version/i]);
  }
  if (/메모리|memory|ram/.test(text) && /물리|크기|용량|사양/.test(text)) {
    return firstExisting(cols, [/computer/i, /csname/i, /username/i, /^id$/i]);
  }
  if (/삼성|samsung|제조사|벤더|maker|manufacturer/.test(text)) {
    return firstExisting(cols, [/manufacturer/i, /vendor/i, /company/i, /maker/i, /product/i, /computer/i, /csname/i]);
  }
  if (/프로세스|process/.test(text)) {
    return firstExisting(cols, [/process/i, /proc/i, /name/i, /description/i, /file/i]);
  }
  if (/사용자|소유자|담당자|user/.test(text)) {
    return firstExisting(cols, [/username/i, /user/i, /owner/i, /name/i]);
  }
  if (/장비|노드|pc|서버|컴퓨터|node|computer/.test(text)) {
    return firstExisting(cols, [/^node_label$/i, /^display_name$/i, /^node_user$/i, /^node_computer$/i, /computer/i, /csname/i, /hostname/i, /^id$/i]);
  }
  return firstExisting(cols, [/^node_label$/i, /^display_name$/i, /caption/i, /version/i, /product/i, /company/i, /computer/i, /username/i, /name/i, /desc/i, /file/i]) || cols[0];
}

function pickChartMetricColumn(cols, question) {
  const text = (question || "").toLowerCase();
  if (/메모리|memory|ram/.test(text)) {
    return firstExisting(cols, [/memory/i, /physical/i, /total.*mem/i, /mem.*total/i, /size/i]);
  }
  if (/cpu|프로세서|코어/.test(text)) {
    return firstExisting(cols, [/cpu/i, /core/i, /processor/i]);
  }
  if (/io|입출력|디스크/.test(text)) {
    return firstExisting(cols, [/io/i, /disk/i, /read/i, /write/i]);
  }
  if (/부하|pscore|load/.test(text)) {
    return firstExisting(cols, [/pscore/i, /load/i]);
  }
  return firstExisting(cols, [/count/i, /sum/i, /avg/i, /max/i, /rate/i, /cpu/i, /memory/i, /io/i, /pscore/i]);
}

function buildDistributionSeries(rows, labelCol, metricCol = null) {
  const counts = new Map();
  rows.forEach(row => {
    const label = formatValue(row[labelCol]) || "unknown";
    const value = metricCol && typeof row[metricCol] === "number" ? row[metricCol] : 1;
    counts.set(label, (counts.get(label) || 0) + value);
  });
  return [...counts.entries()]
    .map(([label, value]) => ({label, value}))
    .sort((a, b) => b.value - a.value);
}

function chartTitleFor(question, labelCol) {
  if (/백신|안티바이러스|antivirus|보안제품/i.test(question || "")) return "백신 제품/버전 노드별 현황";
  if (/윈도우|windows|운영체제|os/i.test(question || "")) return "윈도우 버전 분포";
  if (/삼성|samsung|제조사/i.test(question || "")) return "제조사 기준 장비 분포";
  if (/프로세스|process/i.test(question || "")) return "프로세스 기준 분포";
  if (/사용자|소유자|담당자/i.test(question || "")) return "사용자 기준 분포";
  return `${labelCol} 기준 차트`;
}

function chooseChartFromResult(rows, plan = {}, question = "") {
  if (!rows || !rows.length) return {title: "실행 결과 차트", meta: question, series: []};
  const cols = Object.keys(rows[0]);
  const numericCols = cols.filter(col => rows.some(row => typeof row[col] === "number"));
  const label = pickChartLabelColumn(cols, plan, question);
  const metric = pickChartMetricColumn(numericCols, question) || numericCols[0];
  if (!label) return {title: "실행 결과 차트", meta: question, series: []};

  if (isDistributionQuestion(question)) {
    return {
      title: chartTitleFor(question, label),
      meta: `axis=${label} / value=count`,
      series: buildDistributionSeries(rows, label)
    };
  }

  if (metric && rows.some(row => typeof row[metric] === "number")) {
    return {
      title: chartTitleFor(question, label),
      meta: `axis=${label} / value=${metric}`,
      series: rows
        .filter(row => typeof row[metric] === "number")
        .map(row => ({label: String(row[label] ?? "unknown"), value: row[metric]}))
        .sort((a, b) => b.value - a.value)
    };
  }

  return {
    title: chartTitleFor(question, label),
    meta: `axis=${label} / value=count`,
    series: buildDistributionSeries(rows, label)
  };
}

async function loadInitialLogs() {
  stopLiveExcept("logs");
  try {
    const data = await api("/api/llm-query/recent?limit=80", null, 15000);
    logRows = data.rows || [];
    setDefaultQuestionFromRows(logRows);
    renderLogView(logRows);
    startLogLive();
  } catch (err) {
    document.getElementById("chart").className = "chart-empty";
    document.getElementById("chart").textContent = err.message || String(err);
    document.getElementById("dataGrid").className = "table-empty";
    document.getElementById("dataGrid").textContent = "최근 로그를 불러오지 못했습니다.";
  }
}

async function run(mode) {
  const runId = Symbol(mode);
  activeRun = runId;
  const input = document.getElementById("question");
  const question = input.value.trim();
  if (!question) {
    input.focus();
    return;
  }
  const started = performance.now();
  rememberView(mode);
  setDefaultPanelTitles();
  stopLiveExcept(mode);
  hideSidePanels();
  setCliResult(mode === "query" ? "MongoDB 조회 중..." : "query_plan 생성 중...", "busy");
  document.getElementById("meta").textContent = mode === "query" ? "MongoDB 조회 중..." : "query_plan 생성 중...";
  document.getElementById("errors").textContent = "";
  const tick = setInterval(() => {
    if (activeRun === runId) {
      document.getElementById("meta").textContent = `${((performance.now() - started) / 1000).toFixed(1)}s`;
    }
  }, 500);

  try {
    const data = await api(mode === "query" ? "/api/mongo-query" : "/api/mongo-plan", {question}, mode === "query" ? 120000 : 70000);
    if (activeRun !== runId) return;
    document.getElementById("plan").textContent = pretty(data.plan);
  document.getElementById("aggregation").textContent = pretty(data.aggregation);
  setActiveStats(data);
    const rowCount = data.result_count ?? (data.result_preview ? data.result_preview.length : 0);
    document.getElementById("meta").textContent = `${data.ok ? "OK" : "FAIL"} / ${data.backend} / rows=${rowCount}`;
    setCliResult(`${data.ok ? "OK" : "FAIL"} / ${data.backend || ""} / rows=${rowCount} / ${formatValue(data.execution_ms ?? 0)}ms`, data.ok ? "ok" : "fail");
    const diagnostics = [];
    if (data.errors?.length) diagnostics.push(...data.errors);
    if (data.failure_reason) diagnostics.push(`failure_reason: ${data.failure_reason}`);
    if (data.candidate_fix) diagnostics.push(`candidate_fix: ${JSON.stringify(data.candidate_fix)}`);
    document.getElementById("errors").textContent = diagnostics.join("\n");
    if (mode === "query") {
      const rows = data.result_preview || [];
      const chart = chooseChartFromResult(rows, data.plan || {}, question);
      renderBarChart(chart.title, chart.meta || question, chart.series);
      renderDataTable("실행 결과 데이터", `rows=${rowCount}`, rows);
      clearSecondary("Query Plan", (data.plan || {}).collection || "");
      renderTableInto("secondaryGrid", "secondaryTitle", "secondaryMeta", "Query Plan", (data.plan || {}).collection || "", [data.plan || {}], [
        {label: "collection", value: row => row.collection || ""},
        {label: "group_by", value: row => (row.group_by || []).join(", ")},
        {label: "metrics", value: row => (row.metrics || []).map(item => `${item.name}:${item.op}(${item.field})`).join(", ")},
        {label: "filters", value: row => (row.filters || []).map(item => `${item.field} ${item.op} ${item.value}`).join(", ")}
      ]);
    } else {
      renderBarChart("Query Plan", (data.plan || {}).collection || "", [{label: (data.plan || {}).collection || "plan", value: 1}]);
      renderDataTable("Query Plan", "plan only", [data.plan || {}]);
      clearSecondary("Aggregation", "");
      renderTableInto("secondaryGrid", "secondaryTitle", "secondaryMeta", "Aggregation", "pipeline", (data.aggregation || []).map((stage, index) => ({index, stage})), [
        {label: "#", value: row => row.index + 1},
        {label: "stage", value: row => JSON.stringify(row.stage)}
      ]);
    }
  } catch (err) {
    document.getElementById("meta").textContent = "FAIL";
    document.getElementById("errors").textContent = err.message || String(err);
    setCliResult(`FAIL / ${err.message || String(err)}`, "fail");
  } finally {
    clearInterval(tick);
    input.focus();
    resizeQuestionInput();
  }
}

async function loadCandidates() {
  stopLiveExcept("candidates");
  document.getElementById("meta").textContent = "개선 후보 조회 중...";
  document.getElementById("errors").textContent = "";
  const target = document.getElementById("candidates");
  target.className = "stack-empty";
  target.textContent = "개선 후보 조회 중...";
  try {
    const [data, requestData] = await Promise.all([
      api("/api/improvement-candidates?limit=200", null, 15000),
      api("/api/improvement-requests?limit=50", null, 15000)
    ]);
    document.getElementById("meta").textContent = `candidates=${data.count}`;
    document.getElementById("candidateMeta").textContent = `shown=${data.count} / requests=${requestData.count || 0}`;
    renderImprovementRequests(requestData.requests || []);
    const candidates = data.candidates || [];
    renderCandidateView(candidates);
    if (!candidates.length) {
      target.className = "stack-empty";
      target.textContent = "개선 후보가 없습니다.";
      return;
    }
    target.className = "stack";
    target.innerHTML = candidates.map(item => {
      const fix = item.candidate_fix || {};
      return `
        <article class="candidate">
          <div class="candidate-head">
            <div>
              <div class="candidate-title">${escapeHtml(item.question || "")}</div>
              <div class="badges">
                <span class="badge orange">${escapeHtml(item.status || "proposed")}</span>
                <span class="badge">${escapeHtml(item.failure_reason || "")}</span>
                <span class="badge">${escapeHtml(fix.type || "")}</span>
              </div>
            </div>
            <div class="candidate-actions">
              <button data-action="accepted" data-id="${escapeHtml(item.id)}">승인</button>
              <button data-action="rejected" data-id="${escapeHtml(item.id)}">폐기</button>
              <button data-action="implemented" data-id="${escapeHtml(item.id)}">반영완료</button>
              <button data-request-id="${escapeHtml(item.id)}" data-question="${escapeHtml(item.question || "")}">요청</button>
            </div>
          </div>
          <div>${escapeHtml(fix.reason || "")}</div>
          <details><summary>상세</summary><pre>${escapeHtml(pretty(item))}</pre></details>
        </article>
      `;
    }).join("");
    target.querySelectorAll("button[data-action]").forEach(btn => {
      btn.addEventListener("click", async () => {
        await api(`/api/improvement-candidates/${btn.dataset.id}/status`, {status: btn.dataset.action}, 10000);
        await loadCandidates();
      });
    });
    target.querySelectorAll("button[data-request-id]").forEach(btn => {
      btn.addEventListener("click", () => {
        const input = document.getElementById("improvementInstruction");
        input.dataset.candidateId = btn.dataset.requestId || "";
        input.dataset.question = btn.dataset.question || "";
        input.focus();
      });
    });
    document.getElementById("candidatePanel").classList.remove("hidden");
  } catch (err) {
    target.textContent = err.message || String(err);
  }
}

function renderImprovementRequests(rows) {
  const target = document.getElementById("improvementRequests");
  if (!rows.length) {
    target.className = "stack-empty";
    target.textContent = "등록된 개선 요청이 없습니다.";
    return;
  }
  target.className = "request-list";
  target.innerHTML = rows.slice(0, 10).map(row => `
    <article class="request-item">
      <div class="badges">
        <span class="badge orange">${escapeHtml(row.status || "")}</span>
        <span class="badge">${escapeHtml(formatLocalDateTime(row.created_at) || "")}</span>
        ${row.processed_run_id ? `<span class="badge">${escapeHtml(row.processed_run_id)}</span>` : ""}
      </div>
      <div class="request-text">${escapeHtml(row.instruction || "")}</div>
      ${row.response ? `<details><summary>처리 응답</summary><pre>${escapeHtml(row.response)}</pre></details>` : ""}
    </article>
  `).join("");
}

async function submitImprovementRequest() {
  const input = document.getElementById("improvementInstruction");
  const instruction = input.value.trim();
  if (!instruction) {
    input.focus();
    return;
  }
  document.getElementById("meta").textContent = "개선 요청 등록 중...";
  await api("/api/improvement-requests", {
    instruction,
    candidate_id: input.dataset.candidateId || "",
    question: input.dataset.question || ""
  }, 15000);
  input.value = "";
  input.dataset.candidateId = "";
  input.dataset.question = "";
  await loadCandidates();
}

async function loadLogs() {
  stopLiveExcept("logs");
  document.getElementById("meta").textContent = "최근 로그 조회 중...";
  document.getElementById("errors").textContent = "";
  const target = document.getElementById("logs");
  target.className = "stack-empty";
  target.textContent = "최근 로그 조회 중...";
  try {
    const data = await api("/api/llm-query/recent?limit=100", null, 15000);
    logRows = data.rows || [];
    setDefaultQuestionFromRows(logRows);
    document.getElementById("meta").textContent = `logs=${logRows.length} / live`;
    renderLogView(logRows);
    renderLogSide(logRows);
    startLogLive();
  } catch (err) {
    target.textContent = err.message || String(err);
  }
}

async function loadAutotest() {
  stopLiveExcept("autotest");
  document.getElementById("meta").textContent = "자동 테스트 상태 조회 중...";
  document.getElementById("errors").textContent = "";
  try {
    const data = await api("/api/autotest/status", null, 15000);
    const state = data.state || "status";
    document.getElementById("meta").textContent = `${state} / completed=${data.completed ?? 0} / updated=${formatLocalDateTime(data.updated_at) || "-"}`;
    renderAutotestView(data);
    startAutotestLive();
  } catch (err) {
    document.getElementById("meta").textContent = "FAIL";
    document.getElementById("errors").textContent = err.message || String(err);
  }
}

async function loadLocalCompare() {
  stopLiveExcept("localCompare");
  rememberView("localCompare");
  document.getElementById("meta").textContent = "로컬 LM 비교 집계 조회 중...";
  document.getElementById("errors").textContent = "";
  try {
    const data = await api("/api/local-backend/compare?days=7&limit=120", null, 20000);
    const total = data.total || 0;
    const semanticBase = data.semantic_evaluated || total;
    const capability = data.capability || {};
    const training = data.training || {};
    const evaluation = data.evaluation || {};
    const evalRows = evaluation.rows || [];
    const trainerEvents = data.trainer_events || [];
    document.getElementById("leftPanelTitle").textContent = "Local LM Training";
    document.getElementById("question").placeholder = "평가 질의 또는 재검증할 자연어 질의";
    document.getElementById("meta").textContent = `${capability.title || "검증"} / latest eval=${evaluation.passed ?? "-"}/${evaluation.total ?? "-"} / examples=${training.examples || 0}`;
    const evalSummary = evaluation.total ? `훈련 후 평가 ${evaluation.passed}/${evaluation.total} 통과` : "훈련 후 평가 미실행";
    setCliResult(`${evalSummary}. ${capability.verdict || ""}`, evaluation.passed === evaluation.total && evaluation.total ? "ok" : "busy");
    document.getElementById("activeCollection").textContent = evaluation.local_model || "qwen3-coder:30b";
    document.getElementById("activeRows").textContent = formatValue(training.examples || 0);
    document.getElementById("activeQuality").textContent = evaluation.total ? `${evaluation.passed}/${evaluation.total}` : "-";
    document.getElementById("activeTime").textContent = evaluation.avg_elapsed_ms ? `${formatValue(evaluation.avg_elapsed_ms)}ms` : "-";
    document.getElementById("errors").textContent = localLmNextActions(evalRows);
    document.getElementById("plan").textContent = pretty({
      capability,
      training,
      latest_evaluation: {
        run_id: evaluation.run_id,
        created_at: evaluation.created_at,
        passed: evaluation.passed,
        total: evaluation.total,
        valid: evaluation.valid,
        avg_elapsed_ms: evaluation.avg_elapsed_ms
      }
    });
    document.getElementById("aggregation").textContent = pretty((training.by_collection || []).map(row => ({
      collection: row.collection,
      examples: row.count
    })));
    const issueSeries = data.issues?.length ? data.issues : [
      {label: "semantic_same", value: data.semantic_same || 0},
      {label: "semantic_gap", value: Math.max(0, total - (data.semantic_same || 0))}
    ];
    const chartSeries = (training.by_collection || []).length
      ? (training.by_collection || []).map(row => ({label: row.collection, value: row.count}))
      : issueSeries;
    renderBarChart("학습 예시 구성", `총 ${training.examples || 0}개 / hard open=${training.hard_cases_open || 0} / resolved=${training.hard_cases_resolved || 0}`, chartSeries);
    renderTableInto("dataGrid", "dataTitle", "dataMeta", "훈련 후 평가 세트", "평가 질의는 클릭하면 CLI에서 재실행됩니다.", evalRows, [
      {label: "상태", value: row => row.status},
      {label: "ref", value: row => row.reference_collection || ""},
      {label: "local", value: row => row.local_collection || ""},
      {label: "ms", value: row => row.elapsed_ms},
      {label: "질의", query: true, value: row => row.question || ""},
      {label: "해석", value: row => row.semantic_same ? "기준 plan과 의미 일치" : localLmNextActions([row])}
    ].map(col => col.label === "상태" ? {...col, value: row => row.semantic_same ? "OK" : "GAP"} : col));
    renderTableInto("secondaryGrid", "secondaryTitle", "secondaryMeta", "로컬 LM 데몬 작업", "최근 자동 질의/학습/평가 이벤트", trainerEvents.slice(0, 40), [
      {label: "시간", key: "created_at", format: "time", value: row => row.created_at},
      {label: "상태", value: row => row.status || ""},
      {label: "collection", value: row => row.collection || ""},
      {label: "local", value: row => row.local_collection || ""},
      {label: "rows", value: row => row.result_count ?? ""},
      {label: "수치", value: row => row.evaluation_total ? `${row.evaluation_passed}/${row.evaluation_total}` : (row.local_elapsed_ms ? `${formatValue(row.local_elapsed_ms)}ms` : (row.elapsed_ms ? `${formatValue(row.elapsed_ms)}ms` : ""))},
      {label: "질의/작업", query: true, value: row => row.question || row.message || ""}
    ]);
    return;
    if (evalRows.length) {
      const target = document.getElementById("dataGrid");
      target.insertAdjacentHTML("beforeend", `
        <div class="table-section-title">훈련 후 평가 세트</div>
        <table>
          <thead><tr><th>판정</th><th>ref</th><th>local</th><th>ms</th><th>질의</th></tr></thead>
          <tbody>
            ${evalRows.map(row => `
              <tr>
                <td>${escapeHtml(row.semantic_same ? "OK" : "GAP")}</td>
                <td>${escapeHtml(row.reference_collection || "")}</td>
                <td>${escapeHtml(row.local_collection || "")}</td>
                <td>${escapeHtml(formatValue(row.elapsed_ms))}</td>
                <td><button class="query-link" data-question="${escapeHtml(row.question || "")}">${escapeHtml(row.question || "")}</button></td>
              </tr>
            `).join("")}
          </tbody>
        </table>
      `);
      wireQueryLinks(target);
    }
    const recent = data.recent || [];
    if (recent.length) {
      const target = document.getElementById("dataGrid");
      target.insertAdjacentHTML("beforeend", `
        <div class="table-section-title">최근 채점 근거</div>
        <table>
          <thead><tr><th>시간</th><th>판정</th><th>ref</th><th>local</th><th>ms</th><th>문제</th><th>질의</th></tr></thead>
          <tbody>
            ${recent.map(row => {
              const comparison = row.comparison || {};
              const failed = ["collection_match", "group_by_match", "metric_semantic_match", "filter_match", "sort_match"].filter(key => comparison[key] === false);
              const issue = row.local_error || (row.local_errors || []).join(", ") || failed.join(", ");
              return `
                <tr>
                  <td>${escapeHtml(formatLocalDateTime(row.created_at))}</td>
                  <td>${escapeHtml(comparison.semantic_same ? "OK" : "GAP")}</td>
                  <td>${escapeHtml(row.reference_collection || "")}</td>
                  <td>${escapeHtml(row.local_collection || "")}</td>
                  <td>${escapeHtml(formatValue(row.local_elapsed_ms))}</td>
                  <td>${escapeHtml(issue)}</td>
                  <td><button class="query-link" data-question="${escapeHtml(row.question || "")}">${escapeHtml(row.question || "")}</button></td>
                </tr>
              `;
            }).join("")}
          </tbody>
        </table>
      `);
      wireQueryLinks(target);
    }
  } catch (err) {
    document.getElementById("meta").textContent = "FAIL";
    document.getElementById("errors").textContent = err.message || String(err);
    setCliResult(`FAIL / ${err.message || String(err)}`, "fail");
  }
}

async function refreshActiveView() {
  if (activeView === "candidates") return loadCandidates();
  if (activeView === "autotest") return loadAutotest();
  if (activeView === "localCompare") return loadLocalCompare();
  if (activeView === "cache") {
    stopLiveExcept("cache");
    const data = await api("/api/cache/stats", null, 10000);
    renderCacheView(data);
    return;
  }
  return loadLogs();
}

document.getElementById("refreshBtn").addEventListener("click", refreshActiveView);
document.getElementById("planBtn").addEventListener("click", () => run("plan"));
document.getElementById("queryBtn").addEventListener("click", () => run("query"));
document.getElementById("cacheBtn").addEventListener("click", async () => {
  rememberView("cache");
  document.getElementById("errors").textContent = "";
  const data = await api("/api/cache/stats", null, 10000);
  renderCacheView(data);
});
document.getElementById("candidatesBtn").addEventListener("click", loadCandidates);
document.getElementById("logsBtn").addEventListener("click", loadLogs);
document.getElementById("autotestBtn").addEventListener("click", loadAutotest);
document.getElementById("localCompareBtn").addEventListener("click", loadLocalCompare);
document.getElementById("improvementSubmitBtn").addEventListener("click", submitImprovementRequest);
document.getElementById("improvementInstruction").addEventListener("keydown", event => {
  if (event.key === "Enter" && (event.metaKey || event.ctrlKey)) {
    event.preventDefault();
    submitImprovementRequest();
  }
});
document.getElementById("question").addEventListener("keydown", event => {
  if (event.key === "Enter" && !event.shiftKey) {
    event.preventDefault();
    run("query");
  }
});
document.getElementById("question").addEventListener("input", () => {
  questionTouched = true;
  resizeQuestionInput();
});
document.querySelector(".cli-terminal").addEventListener("click", () => {
  document.getElementById("question").focus();
});

api("/health", null, 10000).then(data => {
  const runtime = data.runtime || data.model || {};
  const local = runtime.local || {};
  document.getElementById("appVersion").textContent = `v${data.version || "0.0.1.1"}`;
  document.getElementById("modelLibrary").textContent = `library=${runtime.library || "Codex CLI"}`;
  document.getElementById("status").textContent = `${data.service} ${data.host}:${data.port} ${data.schema_version} store=${data.store_db} backend=${runtime.backend || "-"} local=${local.model || "-"}(${local.backend || "-"})`;
  return api("/api/llm-query/status", null, 10000).then(status => {
    const total = status.stores?.llm_query?.count;
    if (typeof total === "number") {
      document.getElementById("status").textContent += ` queries=${total.toLocaleString()}`;
    }
  });
}).catch(err => {
  document.getElementById("status").textContent = `offline: ${err.message || err}`;
});

if ("serviceWorker" in navigator) {
  window.addEventListener("load", () => {
    navigator.serviceWorker.register("/sw.js", {scope: "/"}).catch(err => {
      console.warn("q2 service worker registration failed", err);
    });
  });
}

async function loadInitialView() {
  const view = rememberedView();
  if (view === "localCompare") return loadLocalCompare();
  if (view === "autotest") return loadAutotest();
  if (view === "candidates") return loadCandidates();
  if (view === "cache") {
    rememberView("cache");
    const data = await api("/api/cache/stats", null, 10000);
    renderCacheView(data);
    return;
  }
  return loadInitialLogs();
}

loadInitialView();
bootstrapLogLive();
resizeQuestionInput();
document.getElementById("question").focus();
