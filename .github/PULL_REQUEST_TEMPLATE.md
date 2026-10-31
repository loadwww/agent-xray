## What this changes

<!-- One or two sentences. Link the issue if there is one. -->

## Why

<!-- What breaks or is missing today. -->

## Type

- [ ] Bug fix
- [ ] Feature (in scope — see CONTRIBUTING.md)
- [ ] Docs / recipe
- [ ] Build / packaging

## How it was verified

```bash
axr selftest
bash tests/e2e_proxy.sh          # Linux
powershell tests\e2e_proxy.ps1   # Windows
```

- [ ] Selftest passes on my platform
- [ ] E2E passes on my platform
- [ ] UI-affecting change: checked `axr demo` (offline, no API key)
- [ ] New command or flag: usage text and README updated
- [ ] No new third-party dependency (or discussed in an issue first)

## Notes for reviewers

<!-- Trade-offs, deliberate non-goals, screenshots. -->
