# Works with — copy-paste recipes

Every recipe is the same trick: your agent keeps talking OpenAI-compatible `chat/completions`,
you only change **where** it sends the requests. AgentXray reconstructs the run from the traffic.

Verification status is honest: **verified** means someone ran it against a real upstream and checked
the run in the Web UI; **unverified** means the mechanism is correct by inspection but nobody has
sent a real run yet — the linked issue is where that gets fixed.

Start the proxy first (once, in its own terminal):

```bash
axr serve --listen 8080
# Web UI: http://127.0.0.1:8080/__xray
```

---

## OpenAI SDK / LangChain — custom agent — verified

```bash
export OPENAI_BASE_URL="http://127.0.0.1:8080/v1"
export OPENAI_API_KEY="<your real key>"
python your_agent.py
```

Expect: one run per task; each LLM call becomes a step; `axr show <runID>` prints the summary.
PowerShell: `$env:OPENAI_BASE_URL = "http://127.0.0.1:8080/v1"`.

Verified with the `openai` Python package (2.x) against DeepSeek, on Windows, v0.4.2.

## LiteLLM — verified

LiteLLM sits *behind* AgentXray: agent → AgentXray → LiteLLM → provider.

```yaml
# config.yaml
proxy:
  upstreams:
    - match: "/v1"
      url: "http://127.0.0.1:4000"
      name: "litellm"
```

```bash
pip install "litellm[proxy]"   # the proxy extra is required (websockets, backoff, ...)
litellm --model deepseek/deepseek-chat --port 4000
export OPENAI_BASE_URL="http://127.0.0.1:8080/v1"
export OPENAI_API_KEY="anything"   # LiteLLM checks its own key env upstream
```

Newer LiteLLM versions refuse to start without a master key. For local testing
either set `LITELLM_MASTER_KEY` (and send it as the bearer token), or export
`LITELLM_DANGEROUSLY_PERMIT_WEAK_OR_UNSET_MASTER_KEY=true` — never use the
latter outside local development.

Expect: the model LiteLLM was started with appears in the run; usage is present
(LiteLLM normalizes it across providers).

Verified with `litellm` 1.x proxy mode against DeepSeek, on Windows, v0.4.2.

## curl / any raw HTTP client — verified

```bash
export OPENAI_API_KEY="<your real key>"
axr run --listen 8081 -- curl -s http://127.0.0.1:8081/v1/chat/completions ^
  -H "Authorization: Bearer %OPENAI_API_KEY%" -H "Content-Type: application/json" ^
  -d "{\"model\":\"deepseek-chat\",\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}"
```

Expect: a single-step run — useful for "is the proxy working at all" checks before
pointing a whole agent at it.

## Aider — unverified ([#3](https://github.com/loadwww/agent-xray/issues/3))

```bash
axr serve --listen 8080
export OPENAI_API_BASE="http://127.0.0.1:8080/v1"   # note: API_BASE, not BASE_URL
export OPENAI_API_KEY="<your real key>"
aider --model gpt-4o-mini
```

If your upstream is DeepSeek, map the model name (see `model_map` in the README config example) —
Aider only knows OpenAI model names, and `model_map` rewrites them before forwarding.

Gotcha: Aider also reads `ANTHROPIC_*` and `OLLAMA_*`; those providers are not proxied.

## Cline / Continue — unverified ([#4](https://github.com/loadwww/agent-xray/issues/4))

Both editors have an "OpenAI Compatible" provider:

- **Cline**: VS Code settings → API Provider `OpenAI Compatible` → Base URL `http://127.0.0.1:8080/v1`,
  API key = your real key, model = whatever the upstream accepts.
- **Continue**: `config.json` → provider block with `"apiBase": "http://127.0.0.1:8080/v1"`.

Expect: one run per task. The editors may also call `/v1/models` — that endpoint is forwarded
but not reconstructed, so it won't appear as a step. That is expected.

## Ollama — unverified ([#5](https://github.com/loadwww/agent-xray/issues/5))

```yaml
# config.yaml
proxy:
  upstreams:
    - match: "/ollama"
      url: "http://127.0.0.1:11434"
      name: "ollama"
```

```bash
axr serve --listen 8080
export OPENAI_BASE_URL="http://127.0.0.1:8080/ollama/v1"
export OPENAI_API_KEY="ollama"   # any non-empty string
```

No API key and no cloud — the cheapest way to try AgentXray for real.
Note: if the server omits `usage`, the UI shows `—` instead of `0`. That is the honest state,
not a bug.

## vLLM / LM Studio — unverified

Same pattern as Ollama, different upstream URL:

```yaml
proxy:
  upstreams:
    - match: "/local"
      url: "http://127.0.0.1:8000"   # vLLM default; LM Studio uses 1234
      name: "local"
```

`OPENAI_BASE_URL=http://127.0.0.1:8080/local/v1`.

---

## Not supported (forwarded but not reconstructed)

Responses API, Anthropic `/v1/messages`, embeddings, `/v1/models`. Traffic passes through
byte-for-byte, but no steps are built from it. This is deliberate — see the README roadmap.
