#include "ui/webui.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "chain/run.h"
#include "common/time_util.h"
#include "cost/pricing.h"
#include "graph/mermaid.h"
#include "store/events_reader.h"
#include "store/run_index.h"

namespace axr {
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

nlohmann::json stepToJson(const Step& s, double cost, bool costKnown) {
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
    j["cost"] = costKnown ? fmtCost(cost) : std::string("n/a");
    j["error"] = s.response.err;
    j["text"] = trimText(s.response.text, 1200);
    int toolDefs = static_cast<int>(s.call.tools.size());
    j["toolDefs"] = toolDefs;
    j["messages"] = static_cast<int>(s.call.messages.size());

    nlohmann::json tools = nlohmann::json::array();
    for (const auto& t : s.tools) {
        nlohmann::json jt;
        jt["name"] = t.name;
        jt["arguments"] = trimText(t.arguments, 600);
        jt["result"] = trimText(t.result, 600);
        jt["isError"] = t.isError;
        jt["durationMs"] = t.durationMs;
        tools.push_back(jt);
    }
    j["tools"] = tools;
    return j;
}

nlohmann::json runToJson(Run& run, const Config& cfg, bool live) {
    applyCost(run, cfg.pricing);
    const CostBreakdown cb = computeCost(run, cfg.pricing);

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
    j["cost"] = cb.complete ? fmtCost(cb.totalUsd) : std::string("n/a");
    j["costKnown"] = cb.complete;

    nlohmann::json byModel = nlohmann::json::object();
    for (const auto& kv : cb.byModel) byModel[kv.first] = fmtCost(kv.second);
    j["byModel"] = byModel;

    nlohmann::json models = nlohmann::json::array();
    for (const auto& s : run.steps) {
        if (s.call.model.empty()) continue;
        if (std::find(models.begin(), models.end(), s.call.model) == models.end()) models.push_back(s.call.model);
    }
    j["models"] = models;

    nlohmann::json steps = nlohmann::json::array();
    for (std::size_t i = 0; i < run.steps.size(); ++i) {
        const bool known = cb.stepKnown.size() > i && cb.stepKnown[i];
        const double usd = cb.stepUsd.size() > i ? cb.stepUsd[i] : 0.0;
        steps.push_back(stepToJson(run.steps[i], usd, known));
    }
    j["steps"] = steps;
    j["mermaid"] = toMermaid(run, mo);
    return j;
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
  .tag.fuzzy{border-color:var(--warn);color:#f0d795}
  .tag.live{border-color:var(--ok);color:#9ae6b4}
  details.step{margin-top:10px;background:var(--panel);border:1px solid var(--line);border-radius:8px}
  details.step>summary{cursor:pointer;padding:10px 14px;font-size:13px}
  details.step>div{padding:0 14px 14px}
  pre{background:#0b0f14;border:1px solid var(--line);border-radius:8px;padding:10px;overflow:auto;font-size:12px;max-height:320px;white-space:pre-wrap;word-break:break-word}
  .muted{color:var(--dim)}
  .empty{color:var(--dim);padding:40px 0;text-align:center}
</style>
</head>
<body>
<header>
  <h1>AgentXray</h1>
  <span class="hint">零改造 Agent 执行流程逆向 · 每 3 秒自动刷新</span>
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
let runs = [], selected = null, lastKey = '';

const esc = s => String(s == null ? '' : s).replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));

async function api(path) {
  const r = await fetch(path, { cache: 'no-store' });
  return r.json();
}

function renderList() {
  countEl.textContent = runs.length ? runs.length + ' 个' : '';
  if (!runs.length) { listEl.innerHTML = '<div class="empty">还没有 run</div>'; return; }
  listEl.innerHTML = runs.map(r => `
    <div class="run-item ${r.id === selected ? 'active' : ''}" data-id="${esc(r.id)}">
      <div class="id">${esc(r.id)}</div>
      <div class="meta">${esc(r.startedAt || '')} · ${r.steps} 步 · ${r.tools} 工具 · ${r.tokens} tok</div>
      <div class="meta">${esc(r.cost || '')} · ${esc(r.status)}${r.incomplete ? ' · 不完整' : ''}</div>
    </div>`).join('');
  listEl.querySelectorAll('.run-item').forEach(el => {
    el.onclick = () => { selected = el.dataset.id; lastKey = ''; renderList(); loadRun(); };
  });
}

async function loadRuns() {
  try {
    const d = await api('/__xray/api/runs');
    runs = Array.isArray(d) ? d : (d.runs || []);
    renderList();
    if (!selected && runs.length) { selected = runs[0].id; renderList(); loadRun(); }
  } catch (e) { /* 服务器刚启动时忽略 */ }
}

async function loadRun() {
  if (!selected) return;
  let d;
  try { d = await api('/__xray/api/run?id=' + encodeURIComponent(selected)); } catch (e) { return; }
  if (d.error) { mainEl.innerHTML = '<div class="banner bad">' + esc(d.error) + '</div>'; return; }
  const key = d.id + '|' + d.steps.length + '|' + d.endedAt + '|' + d.live;
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
    ['成本', d.cost],
    ['耗时', Math.round(d.durationMs / 1000) + 's'],
    ['模型', d.models.join(', ') || '-'],
    ['模糊边', d.fuzzyEdges],
  ];
  return '<div class="chips">' + items.map(([k, v]) => `<span class="chip">${k} <b>${esc(v)}</b></span>`).join('') + '</div>';
}

function flowSvg(d) {
  const steps = d.steps.slice();
  if (!steps.length) return '<div class="empty">没有步骤</div>';
  const STEPW = 250, STEPH = 74, GAPY = 46, TOOLW = 270, TOOLH = 56, TGAP = 10, X2 = 330;
  const rows = []; let y = 0;
  steps.forEach(s => {
    const th = s.tools.length ? s.tools.length * TOOLH + (s.tools.length - 1) * TGAP : 0;
    const h = Math.max(STEPH, th);
    rows.push({ s, y, h });
    y += h + GAPY;
  });
  const H = y - GAPY + 16, W = X2 + TOOLW + 20;
  let g = '';
  rows.forEach((row, i) => {
    const s = row.s, cx = STEPW / 2;
    if (i > 0) {
      const prev = rows[i - 1];
      const y1 = prev.y + STEPH, y2 = row.y;
      g += `<line x1="${cx}" y1="${y1}" x2="${cx}" y2="${y2}" stroke="#3b4656" stroke-width="1.5" marker-end="url(#arr)"/>`;
      if (s.fuzzy) g += `<text x="${cx + 8}" y="${(y1 + y2) / 2 + 4}" fill="#d29922" font-size="11">推测</text>`;
    }
    const border = s.fuzzy ? '#d29922' : (s.error ? '#f85149' : '#4c9aff');
    g += `<rect x="0" y="${row.y}" width="${STEPW}" height="${STEPH}" rx="10" fill="#151c26" stroke="${border}" stroke-width="1.5" ${s.fuzzy ? 'stroke-dasharray="6 4"' : ''}/>`;
    g += `<text x="12" y="${row.y + 22}" fill="#e6edf3" font-size="13" font-weight="600">Step ${i + 1} · ${esc(s.model || '-')}</text>`;
    g += `<text x="12" y="${row.y + 42}" fill="#8b98a9" font-size="11">${s.latencyMs}ms · ${s.tokens} tok · ${esc(s.cost)}${s.streamed ? ' · 流式' : ''}</text>`;
    g += `<text x="12" y="${row.y + 60}" fill="#8b98a9" font-size="11">${esc((s.finishReason || '') + ' · 消息 ' + s.messages + ' · 工具定义 ' + s.toolDefs)}</text>`;
    s.tools.forEach((t, j) => {
      const ty = row.y + j * (TOOLH + TGAP);
      g += `<line x1="${STEPW}" y1="${row.y + STEPH / 2}" x2="${X2}" y2="${ty + TOOLH / 2}" stroke="#3b4656" stroke-width="1.5" marker-end="url(#arr)"/>`;
      const c = t.isError ? '#f85149' : '#3fb950';
      g += `<rect x="${X2}" y="${ty}" width="${TOOLW}" height="${TOOLH}" rx="8" fill="#131a22" stroke="${c}" stroke-width="1.5"/>`;
      g += `<text x="${X2 + 12}" y="${ty + 22}" fill="#e6edf3" font-size="12" font-weight="600">${esc(t.name)}${t.isError ? ' · 失败' : ''}</text>`;
      g += `<text x="${X2 + 12}" y="${ty + 40}" fill="#8b98a9" font-size="11">${t.durationMs}ms · ${esc((t.arguments || '').slice(0, 34))}</text>`;
    });
  });
  return `<div class="flow"><svg width="${W}" height="${H}" viewBox="0 0 ${W} ${H}">
    <defs><marker id="arr" markerWidth="8" markerHeight="8" refX="7" refY="4" orient="auto">
      <path d="M0,0 L8,4 L0,8 z" fill="#3b4656"/></marker></defs>${g}</svg></div>`;
}

function stepsTable(d) {
  const rows = d.steps.map((s, i) => `
    <tr>
      <td class="mono">${i + 1}</td>
      <td class="mono">${esc(s.model || '-')}</td>
      <td class="mono">${s.latencyMs}ms</td>
      <td class="mono">${s.promptTokens} / ${s.completionTokens}${s.cachedTokens ? ' / 缓存 ' + s.cachedTokens : ''}</td>
      <td class="mono">${esc(s.cost)}</td>
      <td>${s.tools.map(t => `<span class="tag ${t.isError ? 'err' : ''}">${esc(t.name)} · ${t.durationMs}ms</span>`).join(' ') || '<span class="muted">-</span>'}</td>
      <td>${s.fuzzy ? '<span class="tag fuzzy">推测连接</span> ' : ''}${esc((s.text || '').replace(/\s+/g, ' ').slice(0, 90))}</td>
    </tr>`).join('');
  return `<table><thead><tr><th>#</th><th>模型</th><th>耗时</th><th>token（提示/补全）</th><th>成本</th><th>工具</th><th>回复摘要</th></tr></thead><tbody>${rows}</tbody></table>`;
}

function stepDetails(d) {
  return d.steps.map((s, i) => `
    <details class="step"><summary>Step ${i + 1} · ${esc(s.model || '-')} · ${esc((s.text || '').slice(0, 70).replace(/\s+/g, ' '))}</summary>
      <div>
        <div class="muted" style="margin-bottom:8px">${esc(s.endpoint)}</div>
        <pre>${esc(s.text || '(无文本)')}</pre>
        ${s.tools.map(t => `<div style="margin-top:10px"><div>${esc(t.name)} <span class="muted">${t.durationMs}ms${t.isError ? ' · 失败' : ''}</span></div>
          <pre>参数: ${esc(t.arguments || '-')}\n结果: ${esc(t.result || '-')}</pre></div>`).join('')}
      </div>
    </details>`).join('');
}

function renderRun(d) {
  let html = `<div class="muted" style="font-family:Consolas,monospace">${esc(d.id)} <span class="tag ${d.live ? 'live' : ''}">${d.live ? '采集中' : '已落盘'}</span></div>`;
  html += chips(d);
  if (d.incomplete || d.missing.length) {
    html += '<div class="banner">完整性校验未通过：' + (d.missing.map(m => esc(m)).join('；') || '最后一步没有后续请求') + '</div>';
  }
  html += '<h2>流程图</h2>' + flowSvg(d);
  html += '<h2>步骤明细</h2>' + stepsTable(d) + stepDetails(d);
  html += `<details class="step"><summary>Mermaid 源码（可粘贴到 mermaid.live）</summary><div><pre>${esc(d.mermaid)}</pre></div></details>`;
  mainEl.innerHTML = html;
}

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
        nlohmann::json j;
        j["id"] = m.dirName;
        j["startedAt"] = m.hasMeta ? m.startedAt : timeFromDirName(m.dirName);
        j["status"] = m.status;
        j["incomplete"] = m.incomplete;
        j["steps"] = m.stepCount;
        j["tools"] = m.toolCount;
        j["requests"] = m.requestCount;
        j["retries"] = m.retryCount;
        j["tokens"] = m.totalTokens;
        j["cost"] = m.costKnown ? fmtCost(m.costUsd) : std::string("n/a");
        j["models"] = m.models;
        arr.push_back(j);
    }
    r.body = arr.dump();
    return r;
}

WebResponse webUiRun(const Config& cfg, const std::string& id) {
    WebResponse r;
    auto fail = [&](int status, const std::string& msg) {
        r.status = status;
        r.body = nlohmann::json{{"error", msg}}.dump();
        return r;
    };
    if (!validRunId(id)) return fail(400, "非法 run id");

    const std::string runDir = (std::filesystem::path(cfg.storage.root) / "runs" / id).string();
    if (!std::filesystem::is_directory(runDir)) return fail(404, "找不到 run: " + id);

    // 已落盘：直接读 run.json（成本已写回，字段最全）
    const std::string runJsonPath = (std::filesystem::path(runDir) / "run.json").string();
    const std::string raw = readAll(runJsonPath);
    if (!raw.empty()) {
        Run run;
        try {
            run = nlohmann::json::parse(raw).get<Run>();
            r.body = runToJson(run, cfg, false).dump();
            return r;
        } catch (...) {
            // run.json 损坏时退回实时建链
        }
    }

    // 采集中：直接从 events.jsonl 实时还原
    auto chain = buildChainFromEvents(runDir, cfg.chainOptions());
    if (!chain.ok()) return fail(500, "建链失败: " + chain.err);
    const Run* primary = primaryRun(chain.value);
    if (primary == nullptr) return fail(200, "尚未捕获到完整的 LLM 调用");
    Run copy = *primary;
    r.body = runToJson(copy, cfg, true).dump();
    return r;
}

bool webUiWantsHtml(const std::string& path, const std::string& accept) {
    if (path != "/" && path != "") return false;
    return accept.find("text/html") != std::string::npos;
}

} // namespace axr
