#include "ui/webui.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "chain/run.h"
#include "common/time_util.h"
#include "graph/mermaid.h"
#include "store/events_reader.h"
#include "store/run_index.h"

namespace axr {

int countFailedTools(const Run& run) {
    int n = 0;
    for (const auto& s : run.steps) {
        for (const auto& t : s.tools) {
            if (t.errorKind != "none" && !t.errorKind.empty()) ++n;
        }
    }
    return n;
}

namespace {

const std::string kHtmlContentType = "text/html; charset=utf-8";

std::string readAll(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::string();
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string trimText(const std::string& s, std::size_t maxLen) {
    if (s.size() <= maxLen) return s;
    return s.substr(0, maxLen) + "…";
}

bool validRunId(const std::string& id) {
    if (id.empty() || id.size() > 64) return false;
    for (char c : id) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        c == '-' || c == '_';
        if (!ok) return false;
    }
    return true;
}

nlohmann::json stepToJson(const Step& s) {
    nlohmann::json j;
    j["index"] = s.index;
    j["kind"] = s.kind;
    j["model"] = s.call.model;
    j["endpoint"] = s.call.endpoint;
    j["fuzzy"] = s.fuzzyIn;
    j["streamed"] = s.response.streamed;
    j["latencyMs"] = s.response.latencyMs;
    j["finishReason"] = s.response.finishReason;
    j["promptTokens"] = s.response.usage.promptTokens;
    j["completionTokens"] = s.response.usage.completionTokens;
    j["cachedTokens"] = s.response.usage.cachedTokens;
    j["tokens"] = s.tokens;
    j["error"] = s.response.err;
    j["text"] = trimText(s.response.text, 1200);
    int toolDefs = static_cast<int>(s.call.tools.size());
    j["toolDefs"] = toolDefs;
    j["messages"] = static_cast<int>(s.call.messages.size());

    nlohmann::json tools = nlohmann::json::array();
    int failCount = 0, failConfirmed = 0, failInferred = 0;
    for (const auto& t : s.tools) {
        const bool failed = t.errorKind != "none" && !t.errorKind.empty();
        const bool confirmed = failed && t.errorKind == "confirmed";
        if (failed) ++failCount;
        if (confirmed) ++failConfirmed;
        else if (failed) ++failInferred;
        nlohmann::json jt;
        jt["name"] = t.name;
        jt["arguments"] = trimText(t.arguments, 600);
        jt["result"] = trimText(t.result, 600);
        jt["errorKind"] = failed ? t.errorKind : "none";
        jt["isError"] = failed;                    // 兼容旧前端字段
        jt["errorInferred"] = failed && !confirmed; // 兼容旧前端字段
        jt["errorConfirmed"] = confirmed;
        jt["approx"] = t.approx;
        jt["hasResult"] = !t.result.empty();
        jt["durationMs"] = t.durationMs;
        tools.push_back(jt);
    }
    j["tools"] = tools;
    j["failCount"] = failCount;
    j["failConfirmed"] = failConfirmed;
    j["failInferred"] = failInferred;
    j["hasError"] = failCount > 0 || !s.response.err.empty();
    return j;
}

nlohmann::json runToJson(Run& run, const Config& cfg, bool live) {
    MermaidOptions mo;
    mo.collapseThreshold = cfg.chain.collapseThreshold;

    nlohmann::json j;
    j["id"] = run.id;
    j["live"] = live;
    j["startedAt"] = run.startedAt;
    j["endedAt"] = run.endedAt;
    j["incomplete"] = run.incomplete;
    j["missing"] = run.missing;
    j["requestCount"] = run.requestCount;
    j["retryCount"] = run.retryCount;
    j["fuzzyEdges"] = run.fuzzyEdges;

    long long durationMs = 0;
    const auto t0 = parseRfc3339(run.startedAt);
    const auto t1 = parseRfc3339(run.endedAt);
    if (t0.ok() && t1.ok()) durationMs = diffMillis(t1.value, t0.value);
    j["durationMs"] = durationMs;

    j["total"] = {{"prompt", run.total.promptTokens},
                  {"completion", run.total.completionTokens},
                  {"cached", run.total.cachedTokens},
                  {"tokens", run.total.promptTokens + run.total.completionTokens}};

    nlohmann::json models = nlohmann::json::array();
    for (const auto& s : run.steps) {
        if (s.call.model.empty()) continue;
        if (std::find(models.begin(), models.end(), s.call.model) == models.end()) models.push_back(s.call.model);
    }
    j["models"] = models;

    nlohmann::json steps = nlohmann::json::array();
    for (const auto& s : run.steps) steps.push_back(stepToJson(s));
    j["steps"] = steps;
    j["mermaid"] = toMermaid(run, mo);
    j["collapseThreshold"] = cfg.chain.collapseThreshold;
    j["failTools"] = countFailedTools(run);
    return j;
}

// run 摘要（侧栏折叠组 / meta.runs 同款字段）
nlohmann::json runBriefUiJson(const Run& r) {
    int tools = 0;
    for (const auto& s : r.steps) tools += static_cast<int>(s.tools.size());
    return nlohmann::json{{"id", r.id},
                          {"startedAt", r.startedAt},
                          {"steps", static_cast<int>(r.steps.size())},
                          {"tools", tools},
                          {"tokens", r.total.promptTokens + r.total.completionTokens},
                          {"incomplete", r.incomplete},
                          {"requests", r.requestCount},
                          {"retries", r.retryCount},
                          {"fuzzyEdges", r.fuzzyEdges}};
}

// 未收尾的 run 还没有 meta.json，列表里现算一份摘要（否则侧栏只会显示 0 步 / unknown）
nlohmann::json liveSummary(const std::string& runDir, const Config& cfg) {
    nlohmann::json j;
    auto chain = buildChainFromEvents(runDir, cfg.chainOptions());
    if (!chain.ok()) return j;
    const Run* p = primaryRun(chain.value);
    if (p == nullptr) return j;

    Run copy = *p;

    int tools = 0;
    for (const auto& s : copy.steps) tools += static_cast<int>(s.tools.size());
    j["steps"] = static_cast<int>(copy.steps.size());
    j["tools"] = tools;
    j["failTools"] = countFailedTools(copy);
    j["tokens"] = copy.total.promptTokens + copy.total.completionTokens;
    j["requests"] = copy.requestCount;
    j["retries"] = copy.retryCount;
    j["incomplete"] = copy.incomplete;
    nlohmann::json models = nlohmann::json::array();
    for (const auto& s : copy.steps) {
        if (s.call.model.empty()) continue;
        if (std::find(models.begin(), models.end(), s.call.model) == models.end()) models.push_back(s.call.model);
    }
    j["models"] = models;
    // 实时模式：多 run 直接平铺（不落盘 run-<i>.json）
    j["primary"] = p->id;
    j["runCount"] = static_cast<int>(chain.value.runs.size());
    nlohmann::json runs = nlohmann::json::array();
    for (const auto& r : chain.value.runs) runs.push_back(runBriefUiJson(r));
    j["runs"] = std::move(runs);
    return j;
}

// 已收尾 session：把 runId 映射到落盘文件（主 run=run.json，第 i 条非主 run=run-<i>.json，
// 顺序与 finalize 写盘一致——都按 runs 数组顺序跳过主 run）。找不到返回空串。
std::string resolveRunFile(const std::string& runDir, const std::string& runId) {
    if (runId.empty()) return "run.json"; // 缺省主 run，旧目录可能没有 meta.json
    const std::string raw = readAll((std::filesystem::path(runDir) / "meta.json").string());
    if (raw.empty()) return "";
    const nlohmann::json meta = nlohmann::json::parse(raw, nullptr, false);
    if (meta.is_discarded()) return "";
    const std::string primary = meta.value("primary", "");
    if (runId == primary) return "run.json";
    if (!meta.contains("runs") || !meta["runs"].is_array()) return "";
    int idx = 0;
    for (const auto& x : meta["runs"]) {
        const std::string rid = x.value("id", "");
        if (rid.empty() || rid == primary) continue;
        if (rid == runId) return "run-" + std::to_string(idx) + ".json";
        ++idx;
    }
    return "";
}

// 解析降级计数（非 chat/completions 隔离 / 解析失败 / 截断）：>0 才写入，前端据此出顶部提示
void addDegradedCounts(nlohmann::json& j, int unsupported, int parseFailed, int truncated) {
    if (unsupported > 0) j["unsupportedRequests"] = unsupported;
    if (parseFailed > 0) j["parseFailed"] = parseFailed;
    if (truncated > 0) j["truncatedEvents"] = truncated;
}

} // namespace

WebResponse webUiPage() {
    WebResponse r;
    r.contentType = kHtmlContentType;
    r.body = R"HTML(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>AgentXray · 执行流程</title>
<style>
  :root{
    --bg:#0e1116; --panel:#151a21; --panel2:#1b222c; --line:#2a3340;
    --fg:#e6edf3; --dim:#8b98a9; --accent:#4c9aff; --ok:#3fb950; --warn:#d29922; --bad:#f85149;
  }
  *{box-sizing:border-box}
  body{margin:0;background:var(--bg);color:var(--fg);font:14px/1.5 "Segoe UI",system-ui,sans-serif}
  header{display:flex;align-items:center;gap:12px;padding:12px 18px;border-bottom:1px solid var(--line);background:var(--panel)}
  header h1{font-size:16px;margin:0;font-weight:600}
  header .hint{color:var(--dim);font-size:12px}
  .wrap{display:grid;grid-template-columns:300px 1fr;height:calc(100vh - 53px)}
  aside{border-right:1px solid var(--line);overflow:auto;background:var(--panel)}
  .aside-head{position:sticky;top:0;background:var(--panel);padding:10px 14px;border-bottom:1px solid var(--line);display:flex;justify-content:space-between;align-items:center;font-size:12px;color:var(--dim)}
  .run-item{padding:10px 14px;border-bottom:1px solid var(--line);cursor:pointer}
  .run-item:hover{background:var(--panel2)}
  .run-item.active{background:#1d2836;border-left:3px solid var(--accent);padding-left:11px}
  .run-item .id{font-family:Consolas,monospace;font-size:12px}
  .run-item .meta{font-size:11px;color:var(--dim);margin-top:2px}
  .run-item.sub{padding:7px 14px 7px 26px}
  .run-group{padding:3px 14px;font-size:11px;color:var(--dim);cursor:pointer;user-select:none}
  .run-group:hover{color:var(--fg)}
  main{padding:18px 22px;overflow:auto}
  .chips{display:flex;flex-wrap:wrap;gap:8px;margin:10px 0 16px}
  .chip{background:var(--panel2);border:1px solid var(--line);border-radius:999px;padding:4px 10px;font-size:12px;color:var(--dim)}
  .chip b{color:var(--fg);font-weight:600}
  .banner{border:1px solid var(--warn);background:#2a2210;color:#f0d795;border-radius:8px;padding:10px 14px;margin-bottom:14px;font-size:13px}
  .banner.bad{border-color:var(--bad);background:#2a1416;color:#ffb3b0}
  h2{font-size:14px;font-weight:600;margin:22px 0 10px;color:#c9d5e3}
  .flow{background:var(--panel);border:1px solid var(--line);border-radius:10px;padding:14px;overflow:auto}
  svg text{font-family:"Segoe UI",system-ui,sans-serif}
  table{border-collapse:collapse;width:100%;font-size:13px;background:var(--panel);border:1px solid var(--line);border-radius:8px;overflow:hidden}
  th,td{padding:8px 10px;border-bottom:1px solid var(--line);text-align:left;vertical-align:top}
  th{background:var(--panel2);color:var(--dim);font-weight:500;font-size:12px}
  td.mono{font-family:Consolas,monospace;font-size:12px}
  .tag{display:inline-block;padding:1px 7px;border-radius:999px;font-size:11px;border:1px solid var(--line);color:var(--dim)}
  .tag.err{border-color:var(--bad);color:#ff9d99}
  .tag.warn{border-color:var(--warn);color:#f0d795}
  .tag.fuzzy{border-color:var(--warn);color:#f0d795}
  .tag.live{border-color:var(--ok);color:#9ae6b4}
  .bad{color:#ff9d99;font-weight:600}
  .btn{background:var(--panel2);color:var(--fg);border:1px solid var(--line);border-radius:6px;padding:5px 12px;font-size:12px;cursor:pointer}
  .btn:hover{border-color:var(--accent);color:#cfe2ff}
  header .spacer{flex:1}
  .gantt-head{display:flex;gap:14px;align-items:baseline;flex-wrap:wrap;margin-bottom:8px;font-size:12px}
  .gantt-head span{font-size:11px}
  .legend{display:flex;gap:16px;margin-top:8px;font-size:11px;color:var(--dim)}
  .legend i{display:inline-block;width:10px;height:10px;border-radius:2px;margin-right:6px;vertical-align:middle}
  .nowrap{white-space:nowrap}
  details.step{margin-top:10px;background:var(--panel);border:1px solid var(--line);border-radius:8px}
  details.step>summary{cursor:pointer;padding:10px 14px;font-size:13px}
  details.step>div{padding:0 14px 14px}
  pre{background:#0b0f14;border:1px solid var(--line);border-radius:8px;padding:10px;overflow:auto;font-size:12px;max-height:320px;white-space:pre-wrap;word-break:break-word}
  .muted{color:var(--dim)}
  .empty{color:var(--dim);padding:40px 0;text-align:center}
</style>
)HTML" R"HTML(</head>
<body>
<header>
  <h1>AgentXray</h1>
  <span class="hint">零改造 Agent 执行流程逆向 · 每 3 秒自动刷新</span>
  <span class="spacer"></span>
  <button class="btn" id="toggle">折叠工具调用</button>
</header>
<div class="wrap">
  <aside>
    <div class="aside-head"><span>Run 列表</span><span id="count"></span></div>
    <div id="list"></div>
  </aside>
  <main>
    <div id="main"><div class="empty">选择一个 run 查看流程图</div></div>
  </main>
</div>
<script>
const listEl = document.getElementById('list');
const mainEl = document.getElementById('main');
const countEl = document.getElementById('count');
const toggleEl = document.getElementById('toggle');
const wantId = new URLSearchParams(location.search).get('id');
const wantCollapse = new URLSearchParams(location.search).get('collapse') === '1';
let runs = [], selected = null, selectedRun = '', lastKey = '', lastData = null, collapsed = wantCollapse;
const expanded = {}; // sessionId → 多 run 折叠组是否展开
if (collapsed) toggleEl.textContent = '展开工具调用';

const esc = s => String(s == null ? '' : s).replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));

async function api(path) {
  const r = await fetch(path, { cache: 'no-store' });
  return r.json();
}

function renderList() {
  countEl.textContent = runs.length ? runs.length + ' 个' : '';
  if (!runs.length) { listEl.innerHTML = '<div class="empty">还没有 run</div>'; return; }
  listEl.innerHTML = runs.map(r => {
    let html = `
    <div class="run-item ${r.id === selected && !selectedRun ? 'active' : ''}" data-id="${esc(r.id)}">
      <div class="id">${esc(r.id)}${r.live ? ' <span class="tag live">' + esc(r.status) + '</span>' : ''}</div>
      <div class="meta">${esc(r.startedAt || '')} · ${r.steps} 步 · ${r.tools} 工具 · ${r.tokens} tok</div>
      <div class="meta">${esc(r.status)}${r.incomplete ? ' · 不完整' : ''}${r.failTools ? ' · <span class="bad">' + r.failTools + ' 失败</span>' : ''}</div>
    </div>`;
    if (r.runCount > 1 && r.runs) {
      html += `<div class="run-group" data-gid="${esc(r.id)}">${expanded[r.id] ? '▾' : '▸'} 共 ${r.runCount} 个 run</div>`;
      if (expanded[r.id]) {
        html += r.runs.map(sr => `
        <div class="run-item sub ${r.id === selected && selectedRun === sr.id ? 'active' : ''}" data-id="${esc(r.id)}" data-run="${esc(sr.id)}">
          <div class="id">${esc(sr.id)}${sr.id === r.primary ? ' <span class="tag">主</span>' : ''}</div>
          <div class="meta">${sr.steps} 步 · ${sr.tools} 工具 · ${sr.tokens} tok${sr.incomplete ? ' · 不完整' : ''}</div>
        </div>`).join('');
      }
    }
    return html;
  }).join('');
  listEl.querySelectorAll('.run-item').forEach(el => {
    el.onclick = () => { selected = el.dataset.id; selectedRun = el.dataset.run || ''; lastKey = ''; renderList(); loadRun(); };
  });
  listEl.querySelectorAll('.run-group').forEach(el => {
    el.onclick = () => { expanded[el.dataset.gid] = !expanded[el.dataset.gid]; renderList(); };
  });
}

async function loadRuns() {
  try {
    const d = await api('/__xray/api/runs');
    runs = Array.isArray(d) ? d : (d.runs || []);
    renderList();
    if (!selected && runs.length) {
      selected = wantId && runs.some(r => r.id === wantId) ? wantId : runs[0].id;
      renderList();
      loadRun();
    }
  } catch (e) { /* 服务器刚启动时忽略 */ }
}

async function loadRun() {
  if (!selected) return;
  let d;
  try { d = await api('/__xray/api/run?id=' + encodeURIComponent(selected) + (selectedRun ? '&run=' + encodeURIComponent(selectedRun) : '')); } catch (e) { return; }
  if (d.error) { mainEl.innerHTML = '<div class="banner bad">' + esc(d.error) + '</div>'; return; }
  const key = d.id + '|' + d.steps.length + '|' + d.endedAt + '|' + d.live + '|' + d.total.tokens + '|' + d.failTools;
  if (key === lastKey) return;
  lastKey = key;
  renderRun(d);
}

function chips(d) {
  const t = d.total.tokens;
  const items = [
    ['状态', d.incomplete ? '不完整' : '完整'],
    ['Step', d.steps.length],
    ['请求', d.requestCount + '（重试 ' + d.retryCount + '）'],
    ['token', t + '（提示 ' + d.total.prompt + ' / 补全 ' + d.total.completion + '）'],
    ['耗时', Math.round(d.durationMs / 1000) + 's'],
    ['模型', d.models.join(', ') || '-'],
    ['模糊边', d.fuzzyEdges],
    ['失败工具', d.failTools || 0],
  ];
  return '<div class="chips">' + items.map(([k, v], i) =>
    `<span class="chip${k === '失败工具' && v ? ' bad' : ''}">${k} <b>${esc(v)}</b></span>`).join('') + '</div>';
}

)HTML" R"HTML(function flowSvg(d) {
  const steps = d.steps.slice();
  if (!steps.length) return '<div class="empty">没有步骤</div>';
  // 模型名只在顶部 chips 出现一次；节点上仅在"与主模型不同"（中途切模型 / fallback）时才标注
  const mainModel = steps[0].model || '';
  const STEPW = 250, STEPH = 74, GAPY = 46, TOOLW = 280, TOOLH = 56, TGAP = 10, X2 = 330;
  const rows = []; let y = 0;
  steps.forEach(s => {
    const n = collapsed ? 0 : s.tools.length;
    const th = n ? n * TOOLH + (n - 1) * TGAP : 0;
    const h = Math.max(STEPH, th);
    rows.push({ s, y, h });
    y += h + (collapsed ? 26 : GAPY);
  });
  const last = rows[rows.length - 1];
  const H = last.y + last.h + 16, W = X2 + TOOLW + 20;
  let g = '';
  rows.forEach((row, i) => {
    const s = row.s, cx = STEPW / 2;
    if (i > 0) {
      const prev = rows[i - 1];
      const y1 = prev.y + STEPH, y2 = row.y;
      g += `<line x1="${cx}" y1="${y1}" x2="${cx}" y2="${y2}" stroke="#3b4656" stroke-width="1.5" marker-end="url(#arr)"/>`;
      if (s.fuzzy) g += `<text x="${cx + 8}" y="${(y1 + y2) / 2 + 4}" fill="#d29922" font-size="11">推测</text>`;
    }
    // 明确失败（confirmed）红，仅按文本推断失败（inferred）琥珀
    const confirmed = s.failConfirmed > 0 || !!s.error;
    const inferred = !confirmed && s.failInferred > 0;
    const bad = confirmed || inferred;
    const border = confirmed ? '#f85149' : ((inferred || s.fuzzy) ? '#d29922' : '#4c9aff');
    g += `<rect x="0" y="${row.y}" width="${STEPW}" height="${STEPH}" rx="10" fill="${confirmed ? '#20151a' : (inferred ? '#221a0e' : '#151c26')}" stroke="${border}" stroke-width="${bad ? 2 : 1.5}" ${s.fuzzy && !bad ? 'stroke-dasharray="6 4"' : ''}/>`;
    g += `<text x="12" y="${row.y + 22}" fill="#e6edf3" font-size="13" font-weight="600">Step ${i + 1}${confirmed ? ' · 受阻' : (inferred ? ' · 疑似受阻' : '')}</text>`;
    if (s.model && s.model !== mainModel) {
      g += `<text x="${STEPW - 10}" y="${row.y + 22}" text-anchor="end" fill="#d29922" font-size="11">${esc(s.model)}</text>`;
    }
    g += `<text x="12" y="${row.y + 42}" fill="#8b98a9" font-size="11">${s.latencyMs}ms · ${s.tokens} tok${s.streamed ? ' · 流式' : ''}</text>`;
    g += `<text x="12" y="${row.y + 60}" fill="#8b98a9" font-size="11">${esc((s.finishReason || '') + ' · 消息 ' + s.messages + ' · 工具定义 ' + s.toolDefs)}</text>`;
    if (collapsed && s.tools.length) {
      g += `<text x="${STEPW - 10}" y="${row.y + 42}" text-anchor="end" fill="${s.failConfirmed ? '#ff9d99' : (s.failInferred ? '#f0d795' : '#3fb950')}" font-size="11">${s.tools.length} 工具${s.failCount ? ' · ' + s.failCount + ' 失败' : ''}</text>`;
    }
    if (collapsed) return;
    s.tools.forEach((t, j) => {
      const ty = row.y + j * (TOOLH + TGAP);
      g += `<line x1="${STEPW}" y1="${row.y + STEPH / 2}" x2="${X2}" y2="${ty + TOOLH / 2}" stroke="#3b4656" stroke-width="1.5" marker-end="url(#arr)"/>`;
      const confirmed = t.errorKind === 'confirmed', inferred = t.errorKind === 'inferred';
      const c = confirmed ? '#f85149' : (inferred ? '#d29922' : (t.hasResult ? '#3fb950' : '#55637a'));
      g += `<rect x="${X2}" y="${ty}" width="${TOOLW}" height="${TOOLH}" rx="8" fill="${confirmed ? '#20151a' : (inferred ? '#221a0e' : '#131a22')}" stroke="${c}" stroke-width="1.5"><title>${esc(t.result || t.arguments || '')}</title></rect>`;
      g += `<text x="${X2 + 12}" y="${ty + 22}" fill="${confirmed ? '#ffb3b0' : (inferred ? '#f0d795' : '#e6edf3')}" font-size="12" font-weight="600">${esc(t.name)}${confirmed ? ' · 失败' : (inferred ? ' · 疑似失败' : (t.hasResult ? '' : ' · 结果未观测'))}</text>`;
      g += `<text x="${X2 + 12}" y="${ty + 40}" fill="#8b98a9" font-size="11">${t.durationMs}ms · ${esc((t.arguments || '').slice(0, 40))}</text>`;
    });
  });
  return `<div class="flow"><svg width="${W}" height="${H}" viewBox="0 0 ${W} ${H}">
    <defs><marker id="arr" markerWidth="8" markerHeight="8" refX="7" refY="4" orient="auto">
      <path d="M0,0 L8,4 L0,8 z" fill="#3b4656"/></marker></defs>${g}</svg></div>`;
}

function fmtMs(ms) {
  if (!ms || ms <= 0) return '0';
  if (ms < 1000) return Math.round(ms) + 'ms';
  if (ms < 60000) return (ms / 1000).toFixed(1) + 's';
  return Math.floor(ms / 60000) + 'm' + Math.round((ms % 60000) / 1000) + 's';
}

// 合并同名工具时保留更严重的失败等级
function worseKind(a, b) {
  if (a === 'confirmed' || b === 'confirmed') return 'confirmed';
  if (a === 'inferred' || b === 'inferred') return 'inferred';
  return 'none';
}

// 甘特图：模型调用按响应延迟铺；同一步的工具从同一时刻起跑（即并行），宽度为推断耗时
function gantt(d) {
  const steps = d.steps;
  if (!steps.length) return '';
  const MAXROWS = 8;
  const rows = []; let cur = 0;
  steps.forEach((s, i) => {
    const llm = Math.max(0, s.latencyMs || 0);
    rows.push({ label: 'S' + (i + 1) + ' · LLM', start: cur, dur: llm, kind: 'llm', err: false,
      sub: false, tip: 'Step ' + (i + 1) + ' 模型调用 ' + fmtMs(llm) });
    cur += llm;
    const tmax = s.tools.reduce((a, t) => Math.max(a, t.durationMs || 0), 0);
    // 同名工具（并行扇出）合并成一行，避免 8 行同名 http_get 把图撑爆
    const groups = [];
    s.tools.slice(0, MAXROWS * 2).forEach(t => {
      const g = groups.find(x => x.name === t.name);
      const ek = t.errorKind || 'none';
      if (g) {
        g.n++; g.dur = Math.max(g.dur, t.durationMs || 0);
        g.ek = worseKind(g.ek, ek); g.nr = g.nr || !t.hasResult;
      } else {
        groups.push({ name: t.name, n: 1, dur: t.durationMs || 0, ek: ek, nr: !t.hasResult });
      }
    });
    groups.slice(0, MAXROWS).forEach(g => {
      rows.push({ label: g.name + (g.n > 1 ? ' ×' + g.n : ''), start: cur,
        dur: Math.max(g.dur, 2), kind: 'tool', ek: g.ek, sub: true,
        tip: g.name + (g.n > 1 ? ' ×' + g.n + ' 并行' : '') + ' ' + fmtMs(g.dur) +
          (g.ek === 'confirmed' ? '（有失败）' : (g.ek === 'inferred' ? '（疑似失败）' : '')) +
          (g.nr ? '（部分结果未观测）' : '') });
    });
    cur += tmax;
  });
  const measured = cur;
  const total = Math.max(d.durationMs || 0, measured, 1);
  const GUT = 132, W = 940, CH = W - GUT - 16, rowH = 20, top = 26;
  const H = top + rows.length * rowH + 10;
  const px = ms => GUT + (ms / total) * CH;
  const pw = ms => Math.max(2, (ms / total) * CH);
  let g = '';
  for (let k = 0; k <= 4; k++) {
    const ms = total * k / 4, x = px(ms);
    g += `<line x1="${x}" y1="${top - 6}" x2="${x}" y2="${top + rows.length * rowH}" stroke="#1f2733"/>`;
    g += `<text x="${x}" y="${top - 10}" fill="#6b7787" font-size="10" text-anchor="${k === 0 ? 'start' : (k === 4 ? 'end' : 'middle')}">${fmtMs(ms)}</text>`;
  }
  rows.forEach((r, i) => {
    const y = top + i * rowH;
    const color = r.ek === 'confirmed' ? '#f85149' : (r.ek === 'inferred' ? '#d29922' : (r.kind === 'llm' ? '#4c9aff' : '#3fb950'));
    g += `<text x="0" y="${y + 14}" fill="${r.sub ? '#8b98a9' : '#e6edf3'}" font-size="11">${esc((r.sub ? '   ' : '') + r.label)}</text>`;
    g += `<rect x="${px(r.start)}" y="${y + 4}" width="${pw(r.dur)}" height="${rowH - 9}" rx="3" fill="${color}" opacity="${r.kind === 'llm' ? 0.85 : 0.75}"><title>${esc(r.tip)}</title></rect>`;
  });
  if (total - measured > 2) {
    g += `<rect x="${px(measured)}" y="${top}" width="${pw(total - measured)}" height="${rows.length * rowH - 4}" rx="3" fill="#2a3340" opacity="0.5"><title>未计入的间隙（网络 / 调度 / 客户端处理）</title></rect>`;
    g += `<text x="${px(measured) + 6}" y="${top + 16}" fill="#8b98a9" font-size="10">间隙 ${fmtMs(total - measured)}</text>`;
  }
  return `<div class="flow"><div class="gantt-head"><b>总耗时 ${fmtMs(total)}</b><span class="muted">模型 = 响应延迟；工具 = 上一条响应结束 → 下一条请求开始（推断）；同一 Step 内的工具同时起跑即并行</span></div>
    <svg viewBox="0 0 ${W} ${H}" style="width:100%;height:auto">${g}</svg>
    <div class="legend"><span><i style="background:#4c9aff"></i>模型调用</span><span><i style="background:#3fb950"></i>工具</span><span><i style="background:#f85149"></i>明确失败</span><span><i style="background:#d29922"></i>疑似失败（按结果文本推断）</span><span><i style="background:#2a3340"></i>未计入间隙</span></div></div>`;
}

function stepsTable(d) {
  // 单模型 run 不设「模型」列（chips 里已经写过一次）；中途切模型时才显示每步模型
  const multi = (d.models || []).length > 1;
  const rows = d.steps.map((s, i) => `
    <tr>
      <td class="mono">${i + 1}</td>
      ${multi ? `<td class="mono">${esc(s.model || '-')}</td>` : ''}
      <td class="mono">${s.latencyMs}ms</td>
      <td class="mono">${s.promptTokens} / ${s.completionTokens}${s.cachedTokens ? ' / 缓存 ' + s.cachedTokens : ''}</td>
      <td>${s.tools.map(t => `<span class="tag ${t.errorKind === 'confirmed' ? 'err' : (t.errorKind === 'inferred' ? 'warn' : '')}" title="${esc(t.errorKind === 'confirmed' ? '明确失败（协议标记或结果 JSON 报错）' : (t.errorKind === 'inferred' ? '按结果文本推断为失败' : ''))}">${esc(t.name)} · ${t.durationMs}ms${t.errorKind === 'confirmed' ? ' · 失败' : (t.errorKind === 'inferred' ? ' · 疑似失败' : '')}</span>`).join(' ') || '<span class="muted">-</span>'}</td>
      <td>${s.fuzzy ? '<span class="tag fuzzy">推测连接</span> ' : ''}${esc((s.text || '').replace(/\s+/g, ' ').slice(0, 90))}</td>
    </tr>`).join('');
  return `<table><thead><tr><th>#</th>${multi ? '<th>模型</th>' : ''}<th>耗时</th><th>token（提示/补全）</th><th>工具</th><th>回复摘要</th></tr></thead><tbody>${rows}</tbody></table>`;
}

function stepDetails(d) {
  const multi = (d.models || []).length > 1;
  return d.steps.map((s, i) => `
    <details class="step"><summary>Step ${i + 1}${multi ? ' · ' + esc(s.model || '-') : ''} · ${esc((s.text || '').slice(0, 70).replace(/\s+/g, ' '))}</summary>
      <div>
        <div class="muted" style="margin-bottom:8px">${esc(s.endpoint)}</div>
        <pre>${esc(s.text || '(无文本)')}</pre>
        ${s.tools.map(t => `<div style="margin-top:10px"><div>${esc(t.name)} <span class="muted">${t.durationMs}ms${t.errorKind === 'confirmed' ? ' · 失败' : (t.errorKind === 'inferred' ? ' · 疑似失败（按结果文本推断，上游未标 is_error）' : '')}</span></div>
          <pre>参数: ${esc(t.arguments || '-')}\n结果: ${esc(t.result || '-')}</pre></div>`).join('')}
      </div>
    </details>`).join('');
}

)HTML" R"HTML(function renderRun(d) {
  lastData = d;
  let html = `<div class="muted" style="font-family:Consolas,monospace">${esc(d.id)} <span class="tag ${d.live ? 'live' : ''}">${d.live ? '采集中（每 3 秒刷新）' : '已落盘'}</span></div>`;
  html += chips(d);
  if (d.unsupportedRequests) {
    html += '<div class="banner">有 ' + d.unsupportedRequests +
      ' 条非 chat/completions 请求未还原，原始流量已保存在 events.jsonl</div>';
  }
  if (d.parseFailed || d.truncatedEvents) {
    const parts = [];
    if (d.parseFailed) parts.push(d.parseFailed + ' 条请求体解析失败');
    if (d.truncatedEvents) parts.push(d.truncatedEvents + ' 条事件被截断');
    html += '<div class="banner">' + parts.join('，') + '，相关步骤可能不完整</div>';
  }
  if (d.incomplete || d.missing.length) {
    html += '<div class="banner">完整性校验未通过：' + (d.missing.map(m => esc(m)).join('；') || '最后一步没有后续请求') + '</div>';
  }
  if (d.failTools) {
    const first = d.steps.findIndex(s => s.failCount);
    html += '<div class="banner bad">有 ' + d.failTools + ' 个工具调用明确失败或疑似失败，' +
      (first >= 0 ? '最早出现在 Step ' + (first + 1) + '：' : '') +
      '红色为协议/JSON 明确报错，琥珀色为按结果文本推断，可展开该 Step 看原始结果</div>';
  }
  html += '<h2>时间线</h2>' + gantt(d);
  html += '<h2>流程图' + (collapsed ? ' <span class="muted" style="font-size:11px;font-weight:400">（工具分支已折叠，右上角按钮可展开）</span>' : '') + '</h2>' + flowSvg(d);
  html += '<h2>步骤明细</h2>' + stepsTable(d) + stepDetails(d);
  html += `<details class="step"><summary>Mermaid 源码（可粘贴到 mermaid.live）</summary><div><pre>${esc(d.mermaid)}</pre></div></details>`;
  mainEl.innerHTML = html;
}

toggleEl.onclick = () => {
  collapsed = !collapsed;
  toggleEl.textContent = collapsed ? '展开工具调用' : '折叠工具调用';
  if (lastData) renderRun(lastData);
};

loadRuns();
setInterval(() => { loadRuns(); loadRun(); }, 3000);
</script>
</body>
</html>)HTML";
    return r;
}

WebResponse webUiRuns(const Config& cfg) {
    WebResponse r;
    auto runs = listRuns(cfg.storage.root);
    if (!runs.ok()) {
        r.status = 200;
        r.body = nlohmann::json{{"error", runs.err}, {"runs", nlohmann::json::array()}}.dump();
        return r;
    }
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& m : runs.value) {
        if (m.hasMeta && m.status == "empty") continue; // 没捕获到任何流量的会话，列表里不占位置
        nlohmann::json j;
        j["id"] = m.dirName;
        j["startedAt"] = m.hasMeta ? m.startedAt : timeFromDirName(m.dirName);
        j["status"] = m.status;
        j["incomplete"] = m.incomplete;
        j["steps"] = m.stepCount;
        j["tools"] = m.toolCount;
        j["failTools"] = m.failTools;
        j["requests"] = m.requestCount;
        j["retries"] = m.retryCount;
        j["tokens"] = m.totalTokens;
        j["models"] = m.models;
        // 多 run：侧栏折叠组用；旧 meta（无 runs 字段）按单 run 处理
        j["primary"] = m.primary;
        j["runCount"] = m.runs.empty() ? 1 : static_cast<int>(m.runs.size());
        if (!m.runs.empty()) {
            nlohmann::json briefs = nlohmann::json::array();
            for (const auto& b : m.runs) {
                briefs.push_back({{"id", b.id},
                                  {"startedAt", b.startedAt},
                                  {"steps", b.steps},
                                  {"tools", b.tools},
                                  {"tokens", b.tokens},
                                  {"incomplete", b.incomplete},
                                  {"requests", b.requests},
                                  {"retries", b.retries},
                                  {"fuzzyEdges", b.fuzzyEdges}});
            }
            j["runs"] = std::move(briefs);
        }
        if (!m.hasMeta) {
            // 还在采集中（或 axr 被强杀、没来得及收尾）：现算，让侧栏有真实数值
            const auto s = liveSummary(m.path, cfg);
            if (s.empty() || s["steps"].get<int>() <= 0) continue; // 还没捕获到调用，先不占列表
            j["live"] = true;
            j["status"] = m.ageDays * 86400.0 > 600 ? "未收尾" : "采集中";
            for (auto it = s.begin(); it != s.end(); ++it) j[it.key()] = it.value();
        } else {
            j["live"] = false;
        }
        arr.push_back(j);
    }
    r.body = arr.dump();
    return r;
}

WebResponse webUiRun(const Config& cfg, const std::string& id, const std::string& runId) {
    WebResponse r;
    auto fail = [&](int status, const std::string& msg) {
        r.status = status;
        r.body = nlohmann::json{{"error", msg}}.dump();
        return r;
    };
    if (!validRunId(id)) return fail(400, "非法 run id");
    if (!runId.empty() && !validRunId(runId)) return fail(400, "非法 run id");

    const std::string runDir = (std::filesystem::path(cfg.storage.root) / "runs" / id).string();
    if (!std::filesystem::is_directory(runDir)) return fail(404, "找不到 run: " + id);

    // 已落盘：直接读 run.json / run-<i>.json（字段最全）
    const std::string rawPrimary = readAll((std::filesystem::path(runDir) / "run.json").string());
    if (!rawPrimary.empty()) {
        const std::string file = resolveRunFile(runDir, runId);
        if (file.empty()) return fail(404, "找不到 run: " + runId);
        const std::string raw =
            file == "run.json" ? rawPrimary : readAll((std::filesystem::path(runDir) / file).string());
        Run run;
        try {
            run = nlohmann::json::parse(raw).get<Run>();
            nlohmann::json j = runToJson(run, cfg, false);
            // 已落盘：降级计数在 meta.json 里（run.json 不含）
            const std::string rawMeta = readAll((std::filesystem::path(runDir) / "meta.json").string());
            const nlohmann::json meta = nlohmann::json::parse(rawMeta, nullptr, false);
            if (!meta.is_discarded()) {
                addDegradedCounts(j, meta.value("unsupported_requests", 0),
                                  meta.value("parse_failed", 0),
                                  meta.value("truncated_events", 0));
            }
            r.body = j.dump();
            return r;
        } catch (...) {
            // run.json 损坏时退回实时建链
        }
    }

    // 采集中：直接从 events.jsonl 实时还原
    auto chain = buildChainFromEvents(runDir, cfg.chainOptions());
    if (!chain.ok()) return fail(500, "建链失败: " + chain.err);
    const Run* sel = runId.empty() ? primaryRun(chain.value) : findRunById(chain.value, runId);
    if (sel == nullptr) {
        if (!runId.empty()) return fail(404, "找不到 run: " + runId);
        return fail(200, "尚未捕获到完整的 LLM 调用");
    }
    Run copy = *sel;
    nlohmann::json j = runToJson(copy, cfg, true);
    addDegradedCounts(j, chain.value.unsupportedRequests, chain.value.parseFailed,
                      chain.value.truncatedEvents);
    r.body = j.dump();
    return r;
}

bool webUiWantsHtml(const std::string& path, const std::string& accept) {
    if (path != "/" && path != "") return false;
    return accept.find("text/html") != std::string::npos;
}

} // namespace axr
