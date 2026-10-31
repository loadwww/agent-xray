#define _CRT_SECURE_NO_WARNINGS // std::getenv 在 Windows 上会触发 C4996
#include "config/config.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>

#include "common/log.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace axr {
namespace {

std::string trim(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string stripComment(const std::string& s) {
    bool inQuote = false;
    char quote = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (inQuote) {
            if (c == quote) inQuote = false;
        } else if (c == '"' || c == '\'') {
            inQuote = true;
            quote = c;
        } else if (c == '#') {
            return s.substr(0, i);
        }
    }
    return s;
}

std::string unquote(const std::string& s) {
    const std::string t = trim(s);
    if (t.size() >= 2 && ((t.front() == '"' && t.back() == '"') || (t.front() == '\'' && t.back() == '\'')))
        return t.substr(1, t.size() - 2);
    return t;
}

bool startsWith(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

int toInt(const std::string& v, int def) {
    try {
        return std::stoi(unquote(v));
    } catch (...) {
        return def;
    }
}

long long toLL(const std::string& v, long long def) {
    try {
        return std::stoll(unquote(v));
    } catch (...) {
        return def;
    }
}

double toDouble(const std::string& v, double def) {
    try {
        return std::stod(unquote(v));
    } catch (...) {
        return def;
    }
}

bool toBool(const std::string& v, bool def) {
    const std::string t = unquote(v);
    if (t == "true" || t == "yes" || t == "1") return true;
    if (t == "false" || t == "no" || t == "0") return false;
    return def;
}

std::vector<std::string> parseInlineList(const std::string& v) {
    std::string t = trim(v);
    if (!(t.size() >= 2 && t.front() == '[' && t.back() == ']')) return {};
    t = t.substr(1, t.size() - 2);
    std::vector<std::string> out;
    std::string cur;
    for (char c : t) {
        if (c == ',') {
            if (!trim(cur).empty()) out.push_back(unquote(cur));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!trim(cur).empty()) out.push_back(unquote(cur));
    return out;
}

// 把 value 写到对应配置字段；不识别的键静默忽略（配置向前兼容）
void applyScalar(Config& cfg, const std::string& section, const std::string& key, const std::string& rawValue) {
    const std::string v = unquote(rawValue);
    if (section == "proxy") {
        if (key == "listen") cfg.proxy.listen = toInt(v, cfg.proxy.listen);
        else if (key == "bind") cfg.proxy.bind = v;
        else if (key == "max_body_bytes") cfg.proxy.maxBodyBytes = toLL(v, cfg.proxy.maxBodyBytes);
        else if (key == "max_run_bytes") cfg.proxy.maxRunBytes = toLL(v, cfg.proxy.maxRunBytes);
        else if (key == "max_queue") cfg.proxy.maxQueue = toInt(v, cfg.proxy.maxQueue);
        else if (key == "inject_usage") cfg.proxy.injectUsage = toBool(v, cfg.proxy.injectUsage);
        else if (key == "capture") {
            if (v == "full" || v == "summary") cfg.proxy.capture = v;
            else logWarn("config", "proxy.capture 只支持 full|summary，已忽略: " + v);
        }
        else if (key == "tool_error_patterns") cfg.proxy.toolErrorPatterns = parseInlineList(v);
    } else if (section == "chain") {
        if (key == "run_gap_minutes") cfg.chain.runGapMinutes = toInt(v, cfg.chain.runGapMinutes);
        else if (key == "fuzzy_threshold") cfg.chain.fuzzyThreshold = toDouble(v, cfg.chain.fuzzyThreshold);
        else if (key == "fuzzy_window") cfg.chain.fuzzyWindow = toInt(v, cfg.chain.fuzzyWindow);
        else if (key == "collapse_threshold") cfg.chain.collapseThreshold = toInt(v, cfg.chain.collapseThreshold);
    } else if (section == "redact") {
        if (key == "headers") cfg.redact.headers = parseInlineList(v);
        else if (key == "patterns") cfg.redact.patterns = parseInlineList(v);
        else if (key == "pii") cfg.redact.pii = toBool(v, cfg.redact.pii);
    } else if (section == "storage") {
        if (key == "root") cfg.storage.root = v;
        else if (key == "retention_days") cfg.storage.retentionDays = toInt(v, cfg.storage.retentionDays);
        else if (key == "idle_finalize_minutes")
            cfg.storage.idleFinalizeMinutes = toInt(v, cfg.storage.idleFinalizeMinutes);
    }
}

} // namespace

std::string defaultStorageRoot() {
#ifdef _WIN32
    char buf[MAX_PATH] = {0};
    DWORD n = GetEnvironmentVariableA("USERPROFILE", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return std::string(buf) + "\\.xray";
#endif
    const char* home = std::getenv("HOME");
    if (home && *home) return std::string(home) + "/.xray";
    return ".xray";
}

std::string expandHome(const std::string& path) {
    if (path.empty()) return defaultStorageRoot();
    if (path[0] == '~') {
        std::string rest = path.substr(1);
        if (!rest.empty() && (rest[0] == '/' || rest[0] == '\\')) rest = rest.substr(1);
        const std::string root = defaultStorageRoot();
        // defaultStorageRoot 形如 <home>/.xray，这里去掉末尾 "/.xray" 得到 home
        std::string base = root;
        const std::string suffix = ".xray";
        if (base.size() > suffix.size() && base.compare(base.size() - suffix.size(), suffix.size(), suffix) == 0) {
            base.resize(base.size() - suffix.size());
            if (!base.empty() && (base.back() == '/' || base.back() == '\\')) base.pop_back();
        }
        return rest.empty() ? base : base + "/" + rest;
    }
    return path;
}

Config defaultConfig() {
    Config cfg;
    cfg.proxy.upstreams = {
        UpstreamDef{"/v1", "https://api.deepseek.com/v1", "deepseek"},
        UpstreamDef{"/ollama", "http://127.0.0.1:11434", "ollama"},
    };
    cfg.storage.root = defaultStorageRoot();
    return cfg;
}

ChainOptions Config::chainOptions() const {
    ChainOptions o;
    o.runGapMinutes = chain.runGapMinutes;
    o.fuzzyThreshold = chain.fuzzyThreshold;
    o.fuzzyWindow = chain.fuzzyWindow;
    o.toolErrorPatterns = proxy.toolErrorPatterns;
    return o;
}

Result<Config> parseConfigText(const std::string& text, const Config& base) {
    Config cfg = base;

    std::string section;  // proxy / chain / redact / storage
    std::string subKey;   // upstreams / model_map / headers / patterns
    int modelMapIndent = 0;
    int upstreamsIndent = 0; // upstreams: 的缩进；更深缩进才属于某个 upstream 字段
    bool upsCleared = false, hdrCleared = false, patCleared = false, tepCleared = false;

    std::size_t pos = 0;
    while (pos <= text.size()) {
        const std::size_t nl = text.find('\n', pos);
        std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = (nl == std::string::npos) ? text.size() + 1 : nl + 1;

        if (!line.empty() && line.back() == '\r') line.pop_back();
        line = stripComment(line);
        if (trim(line).empty()) continue;

        int indent = 0;
        while (indent < static_cast<int>(line.size()) && line[indent] == ' ') ++indent;
        std::string body = trim(line);

        if (startsWith(body, "- ")) {
            const std::string item = trim(body.substr(2));
            if (section == "proxy" && (subKey == "upstreams" || subKey == "model_map")) {
                const std::size_t colon = item.find(':');
                if (colon == std::string::npos) {
                    logWarn("config", "忽略无法识别的 upstreams 列表项: " + item);
                    continue;
                }
                UpstreamDef up;
                const std::string k = trim(item.substr(0, colon));
                const std::string v = unquote(item.substr(colon + 1));
                if (k == "match") up.match = v;
                else if (k == "url") up.url = v;
                else if (k == "name") up.name = v;
                cfg.proxy.upstreams.push_back(up);
                subKey = "upstreams"; // 新列表项重新归属 upstreams（model_map 可能刚切换过子键）
            } else if (section == "proxy" && subKey == "tool_error_patterns") {
                cfg.proxy.toolErrorPatterns.push_back(unquote(item));
            } else if (section == "redact" && (subKey == "headers" || subKey == "patterns")) {
                if (subKey == "headers") cfg.redact.headers.push_back(unquote(item));
                else cfg.redact.patterns.push_back(unquote(item));
            }
            continue;
        }

        const std::size_t colon = body.find(':');
        if (colon == std::string::npos) continue; // 不是 key: value
        const std::string key = trim(body.substr(0, colon));
        const std::string value = trim(body.substr(colon + 1));

        if (value.empty()) {
            // 无值键：顶层切换 section，缩进层记录子键（upstreams / headers / patterns 等列表起点）
            if (indent == 0) {
                section = key;
                subKey.clear();
            } else {
                subKey = key;
                if (key == "model_map") modelMapIndent = indent;
                if (key == "upstreams") upstreamsIndent = indent;
            }
            // 配置文件里的列表覆盖默认值（只清一次，允许列表跨多行）
            if (indent > 0 && section == "proxy" && subKey == "upstreams" && !upsCleared) {
                cfg.proxy.upstreams.clear();
                upsCleared = true;
            }
            if (indent > 0 && section == "redact" && subKey == "headers" && !hdrCleared) {
                cfg.redact.headers.clear();
                hdrCleared = true;
            }
            if (indent > 0 && section == "redact" && subKey == "patterns" && !patCleared) {
                cfg.redact.patterns.clear();
                patCleared = true;
            }
            if (indent > 0 && section == "proxy" && subKey == "tool_error_patterns" && !tepCleared) {
                cfg.proxy.toolErrorPatterns.clear();
                tepCleared = true;
            }
            continue;
        }

        // model_map 内的 "客户端模型名: 上游模型名" 映射
        if (section == "proxy" && subKey == "model_map" && indent > modelMapIndent && !cfg.proxy.upstreams.empty()) {
            cfg.proxy.upstreams.back().modelMap[unquote(key)] = unquote(value);
            continue;
        }
        if (section == "proxy" && subKey == "model_map" && indent <= modelMapIndent) {
            subKey = "upstreams"; // 缩进退回 upstream 字段层级，后续键仍按 upstream 解析
        }

        if (section == "proxy" && subKey == "upstreams" && indent > upstreamsIndent &&
            !cfg.proxy.upstreams.empty()) {
            UpstreamDef& up = cfg.proxy.upstreams.back();
            if (key == "match") up.match = unquote(value);
            else if (key == "url") up.url = unquote(value);
            else if (key == "name") up.name = unquote(value);
            continue;
        }

        applyScalar(cfg, section, key, value);
    }

    if (cfg.proxy.upstreams.empty()) return Result<Config>::Fail("config: proxy.upstreams 不能为空");
    for (const auto& up : cfg.proxy.upstreams) {
        if (up.match.empty() || up.url.empty())
            return Result<Config>::Fail("config: upstream 缺少 match 或 url（" + up.name + "）");
        if (up.url.find("://") == std::string::npos)
            return Result<Config>::Fail("config: upstream url 必须以 http:// 或 https:// 开头: " + up.url);
    }
    cfg.storage.root = expandHome(cfg.storage.root);
    return Result<Config>::Ok(cfg);
}

Result<Config> loadConfig(const std::string& path, const Config& base) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return Result<Config>::Fail("无法打开配置文件: " + path);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto r = parseConfigText(text, base);
    if (!r.ok()) return Result<Config>::Fail(r.err + "（文件: " + path + "）");
    return r;
}

} // namespace axr
