# Contributing

Thanks for looking. This is a single-maintainer project, so the safest contributions are ones that
fit the existing shape: reconstructions, fixes, recipes, docs.

## Scope

In scope: proxy correctness (headers, status codes, streaming), reconstruction quality, the local UI,
the artifacts (`events.jsonl` / `run.json` / `graph.mmd` / `report.md`), CLI ergonomics, install and
packaging, OTel export.

Out of scope, deliberately: evals, prompt management, datasets, cloud/multi-tenant hosting, billing,
and additional protocol families (Anthropic `/v1/messages`, Responses API, embeddings). Those belong
to tools like Langfuse; this project stays a local view of an uninstrumented agent.

If you are unsure, open a Discussion before writing code. It is cheaper for both of us.

## Build and test

Requirements: a C++17 compiler and CMake. On Ubuntu/Debian also `libcurl4-openssl-dev`.
Windows builds use MSVC and WinHTTP and need no libcurl.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/axr selftest
bash tests/e2e_proxy.sh          # Linux / macOS
powershell tests\e2e_proxy.ps1   # Windows
```

`selftest` does not run automatically after a build — run it yourself. Both suites are offline: they
start a mock upstream, so no API key and no network are needed.

If your change touches the UI or reconstruction, also run `axr demo` and look at the two bundled runs
(one that succeeds, one that edits the wrong file, fails the tests and loops).

## Guidelines

- Match the surrounding style. C++17, no exceptions in hot paths, no new third-party dependency without
  an issue first.
- Keep changes scoped to the problem. A bug fix does not need nearby cleanup.
- New command or flag: update the usage text in `src/main.cpp` and the README `Usage` section in the same PR.
- Every command that appears in the README must be runnable as written. If you change a command, change the docs.
- Commit messages: one short subject, body explaining why when it is not obvious.

## Recipes

New "Works with" recipes are the cheapest useful contribution: one short section with the start command,
the environment variable, and what you should see in the UI. Mark it as verified on which platform.

## License

Contributions are accepted under the project's MIT license.
