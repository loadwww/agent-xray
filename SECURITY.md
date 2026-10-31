# Security policy

AgentXray is a local proxy. It sees the traffic your agent sends to an LLM provider, which includes
headers such as `Authorization`. That makes its redaction, binding and file handling part of the
security surface, not an afterthought.

## Reporting a vulnerability

Please **do not** open a public issue. Use GitHub's private reporting flow:

<https://github.com/loadwww/agent-xray/security/advisories/new>

Include: affected version (`axr version`), platform, the command you ran, and the smallest
reproduction you can manage (a config plus a mock upstream is usually enough — no real API key needed).

## What counts as a security issue

- A secret (API key, bearer token) written to `events.jsonl`, `run.json`, `report.md`, the Web UI, or a terminal log, when redaction is on (the default).
- Data leaving the machine that you did not configure: an unexpected outbound request, telemetry, an update check.
- A bind-policy bypass: listening beyond the loopback interface without `--allow-remote`.
- Path handling that lets a crafted run directory or run id write outside the configured root.
- Anything that lets one local process read another user's runs (run directories are created `0700`).

Not a security issue: the proxy stores request and response bodies by design (`proxy.capture: full`);
that is the product. Set `capture: summary` if you need lengths and hashes only.

## Out of scope by design

- There is no authentication. Exposing the port to a network is a `--allow-remote` decision that puts
  the network boundary on you.
- Upstream TLS verification follows the system trust store; a MITM setup is not supported.

## Supported versions

Fixes land on the latest release. Only the current minor version is supported — this is a single-maintainer
project, so there is no backport branch. There is no bug bounty and no SLA; reports are handled as time allows,
with a fix in the next release where the report is actionable.
