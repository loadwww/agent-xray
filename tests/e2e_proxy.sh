#!/usr/bin/env bash
# M2 离线验收（Linux/macOS）：mock 上游 -> axr run 录制 -> events/run.json/meta 检查
# 用法: bash tests/e2e_proxy.sh
# 环境覆盖：AXR_BUILD_DIR 指定构建目录（默认 <repo>/build），AXR_PYTHON 指定 python 解释器（默认 python3）
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${AXR_BUILD_DIR:-"$ROOT/build"}
AXR="$BUILD/axr"
MOCK="$BUILD/mock_openai"
PYTHON=${AXR_PYTHON:-python3}
CFG="$ROOT/tests/proxy_test.yaml"
AGENT="$ROOT/tests/fake_agent.py"
RUNS_ROOT="$BUILD/e2e-runs"

FAIL=0
check() {  # $1=0 通过，非 0 失败（放在 [ ... ] / grep 之后直接用 $? 传入） $2=描述
    if [ "$1" -eq 0 ]; then echo "  PASS  $2"; else echo "  FAIL  $2（码 $1）"; FAIL=$((FAIL+1)); fi
}
count_kind() {    # $1=events.jsonl $2=kind
    grep -o "\"kind\":\"$2\"" "$1" | wc -l | tr -d ' '
}
newest_run() {
    ls -1dt "$RUNS_ROOT"/runs/*/ 2>/dev/null | head -n 1
}
newest_run_under() {  # $1=root
    ls -1dt "$1"/runs/*/ 2>/dev/null | head -n 1
}

[ -x "$AXR" ] || { echo "缺少 $AXR，请先构建（cmake --build build -j）"; exit 2; }
[ -x "$MOCK" ] || { echo "缺少 $MOCK，请先构建"; exit 2; }

rm -rf "$RUNS_ROOT"

"$MOCK" --port 9099 >/dev/null 2>&1 &
MOCK_PID=$!
trap 'kill "$MOCK_PID" 2>/dev/null' EXIT

ready=0
for _ in $(seq 1 50); do
    if curl -s -m 1 "http://127.0.0.1:9099/__case/deep" >/dev/null 2>&1; then ready=1; break; fi
    sleep 0.1
done
[ "$ready" -eq 1 ]; check "$?" "mock_openai 就绪"

echo "[1] axr run：非流式，3 轮工具调用 + 最终回答"
RUN1_OUT=$("$AXR" run --config "$CFG" --root "$RUNS_ROOT" -- "$PYTHON" "$AGENT" 2>&1)
RUN1_RC=$?
echo "$RUN1_OUT"
check "$RUN1_RC" "axr run 退出码 0"
echo "$RUN1_OUT" | grep -q "ollama_host = (unset)"
check "$?" "P2-12 不再注入 OLLAMA_HOST"

RUN1=$(newest_run)
[ -n "$RUN1" ]; check "$?" "生成 run 目录"
RUN1=${RUN1%/}
EVENTS1="$RUN1/events.jsonl"
for f in events.jsonl run.json graph.mmd meta.json; do
    [ -f "$RUN1/$f" ]; check "$?" "$f 已落盘"
done
[ "$(count_kind "$EVENTS1" req)" -ge 4 ]; check "$?" "req 事件 >= 4（实际 $(count_kind "$EVENTS1" req)）"
[ "$(count_kind "$EVENTS1" resp)" -ge 4 ]; check "$?" "resp 事件 >= 4（实际 $(count_kind "$EVENTS1" resp)）"
[ "$(grep -c "sk-0123456789" "$EVENTS1" || true)" -eq 0 ]; check "$?" "body 中的 sk- 密钥已脱敏"
grep -Eq '"authorization":\s*"\*\*\*"' "$EVENTS1"
check "$?" "authorization 头已脱敏"

STEPS=$("$PYTHON" -c "import json,sys;print(len(json.load(open(sys.argv[1]))['steps']))" "$RUN1/run.json")
check "$?" "run.json 可解析"
[ "${STEPS:-0}" -ge 3 ]; check "$?" "还原步数 >= 3（实际 $STEPS）"
grep -q '"total"' "$RUN1/run.json"
check "$?" "token 统计存在"

META_OK=$("$PYTHON" - "$RUN1/meta.json" <<'PYEOF'
import json, sys
m = json.load(open(sys.argv[1]))
ok = m.get('status') == 'complete' and m.get('step_count', 0) >= 3 and 'deepseek-chat' in m.get('models', [])
print(1 if ok else 0)
PYEOF
)
[ "$META_OK" -eq 1 ]; check "$?" "meta：complete / step_count>=3 / 含 deepseek-chat"

echo "[2] axr run：流式（S2 边收边发 + 逐 chunk 录制）"
STREAM_OUT=$("$AXR" run --config "$CFG" --root "$RUNS_ROOT" -- "$PYTHON" "$AGENT" --stream 2>&1)
check "$?" "流式 axr run 退出码 0"
echo "$STREAM_OUT" | grep -q "transfer-encoding = chunked"
check "$?" "S2 流式响应为 chunked（不是缓冲后一次性返回）"
echo "$STREAM_OUT" | grep -Eq "sse bytes = [0-9]+"
check "$?" "客户端收到 SSE 数据"

RUN2=$(newest_run)
[ -n "$RUN2" ]; check "$?" "流式 run 已生成"
RUN2=${RUN2%/}
[ "$(count_kind "$RUN2/events.jsonl" sse_chunk)" -ge 1 ]; check "$?" "流式录制包含 sse_chunk"
[ "$(count_kind "$RUN2/events.jsonl" sse_end)" -ge 1 ]; check "$?" "流式录制包含 sse_end"
SSTEPS=$("$PYTHON" -c "import json,sys;print(len(json.load(open(sys.argv[1]))['steps']))" "$RUN2/run.json")
[ "${SSTEPS:-0}" -ge 1 ]; check "$?" "流式 run 还原出步骤（实际 $SSTEPS）"

echo "[3] P2-8 上游拒绝 stream_options 时自动降级重试"
curl -s -m 5 "http://127.0.0.1:9099/__mode/usage_reject" >/dev/null
DEGRADE_OUT=$("$AXR" run --config "$CFG" --root "$RUNS_ROOT" -- "$PYTHON" "$AGENT" --stream 2>&1)
check "$?" "降级重试后流式 axr run 退出码 0"
echo "$DEGRADE_OUT" | grep -Eq "sse bytes = [0-9]+"
check "$?" "降级重试后客户端仍收到 SSE 数据"
RUN3=$(newest_run)
[ -n "$RUN3" ]; check "$?" "降级重试 run 已生成"
RUN3=${RUN3%/}
DEGRADE_META=$("$PYTHON" - "$RUN3/meta.json" <<'PYEOF'
import json, sys
m = json.load(open(sys.argv[1]))
ok = m.get('usage_injected') is False and m.get('rewritten') is True
print(1 if ok else 0)
PYEOF
)
[ "$DEGRADE_META" -eq 1 ]; check "$?" "meta：usage_injected=false / rewritten=true"
curl -s -m 5 "http://127.0.0.1:9099/__mode/ok" >/dev/null

echo "[4] P2-12 本地 ollama 走 /ollama/v1 前缀仍能被采集"
OLLAMA_OUT=$(AXR_FAKE_PATH=/ollama/v1 "$AXR" run --config "$CFG" --root "$RUNS_ROOT" -- "$PYTHON" "$AGENT" 2>&1)
OLLAMA_RC=$?
echo "$OLLAMA_OUT"
check "$OLLAMA_RC" "ollama 前缀 axr run 退出码 0"
echo "$OLLAMA_OUT" | grep -q "/ollama/v1"
check "$?" "客户端请求发到 /ollama/v1"
RUN4=$(newest_run)
RUN4=${RUN4%/}
OLLAMA_STEPS=$("$PYTHON" -c "import json,sys;print(len(json.load(open(sys.argv[1]))['steps']))" "$RUN4/run.json" 2>/dev/null)
[ "${OLLAMA_STEPS:-0}" -ge 3 ]; check "$?" "ollama 前缀 run 还原步数 >= 3（实际 ${OLLAMA_STEPS:-0}）"

echo "[5] WP4 流式 sse_chunk 落盘脱敏（客户端仍收原文）"
curl -s -m 5 "http://127.0.0.1:9099/__case/secret" >/dev/null
SECRET_OUT=$("$AXR" run --config "$CFG" --root "$RUNS_ROOT" -- "$PYTHON" "$AGENT" --stream 2>&1)
check "$?" "secret 流式 axr run 退出码 0"
echo "$SECRET_OUT" | grep -q "sse has secret = True"
check "$?" "客户端收到的 SSE 仍是原文（含密钥）"
RUN5=$(newest_run)
RUN5=${RUN5%/}
[ "$(grep -c "sk-SECRETKEY0123456789ABCDEFGHIJ" "$RUN5/events.jsonl" || true)" -eq 0 ]
check "$?" "full 模式 sse_chunk 已脱敏"

echo "[6] WP5/WP6 serve 优雅退出与懒建会话"
wait_port() {  # $1=端口
    for _ in $(seq 1 60); do
        if (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null; then return 0; fi
        sleep 0.1
    done
    return 1
}

SERVE_ROOT="$BUILD/e2e-serve-shutdown"
rm -rf "$SERVE_ROOT"

# 6.1 无流量时 SIGINT：懒建会话，不应留下任何 run 目录
"$AXR" serve --config "$CFG" --root "$SERVE_ROOT" --listen 8083 > "$BUILD/e2e-serve-quiet.log" 2>&1 &
SERVE_PID=$!
wait_port 8083
check "$?" "serve 开始监听 8083（无流量）"
kill -INT "$SERVE_PID"
wait "$SERVE_PID"
check "$?" "无流量 SIGINT 退出码 0"
DIRS=0
if [ -d "$SERVE_ROOT/runs" ]; then DIRS=$(ls -1 "$SERVE_ROOT/runs" | wc -l | tr -d ' '); fi
[ "$DIRS" -eq 0 ]; check "$?" "无流量退出后未新增 run 目录（实际 $DIRS）"

# 6.2 有流量时 SIGINT：必须完成 finalize，meta.json 落盘
"$AXR" serve --config "$CFG" --root "$SERVE_ROOT" --listen 8084 > "$BUILD/e2e-serve-live.log" 2>&1 &
SERVE2_PID=$!
wait_port 8084
check "$?" "serve 开始监听 8084"
curl -s -m 10 -X POST "http://127.0.0.1:8084/v1/chat/completions" \
    -H 'Content-Type: application/json' \
    -d '{"model":"deepseek-chat","messages":[{"role":"user","content":"hi"}]}' >/dev/null
sleep 1
kill -INT "$SERVE2_PID"
wait "$SERVE2_PID"
check "$?" "有流量 SIGINT 退出码 0"
LIVE_RUN=$(newest_run_under "$SERVE_ROOT")
LIVE_RUN=${LIVE_RUN%/}
[ -n "$LIVE_RUN" ]; check "$?" "有流量退出后生成 run 目录"
if [ -n "$LIVE_RUN" ]; then
    [ -f "$LIVE_RUN/meta.json" ] && [ -f "$LIVE_RUN/run.json" ]
    check "$?" "SIGINT 收尾后 meta.json / run.json 已生成"
fi
ALL_DIRS=$(ls -1 "$SERVE_ROOT/runs" 2>/dev/null | wc -l | tr -d ' ')
[ "$ALL_DIRS" -eq 1 ]; check "$?" "两轮 serve 只留下有流量的 1 个会话（实际 $ALL_DIRS）"

if [ "$FAIL" -gt 0 ]; then
    echo
    echo "结果: $FAIL 项失败"
    exit 1
fi
echo
echo "结果: 全部通过"
exit 0