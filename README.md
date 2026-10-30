# AgentXray

零改造 Agent 执行流程逆向与可视化系统。CLI 名：`axr`。实现语言：C++17，采集模式：Mode A（反向代理）。

> 完整施工规格见 [docs/agent-xray-design.md](docs/agent-xray-design.md)。

## 构建（Windows / MSVC）

环境隔离在 `D:\env\axr`，代码区不存放任何工具链。

```powershell
. D:\env\axr\env.ps1          # 初始化 MSVC + WinSDK + CMake + Ninja
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

产物：`build\axr.exe`

一键回归（环境初始化 + 构建 + 自检）：

```powershell
powershell -ExecutionPolicy Bypass -File tests\run_tests.ps1
```

## 已实现命令

```powershell
build\axr.exe selftest                                  # 50 条断言：hash / model / SSE / 链 / fuzzy
build\axr.exe replay tests\fixtures\deepseek_tool_2 --format mermaid --out build\graph.mmd
build\axr.exe replay tests\fixtures\t2_single_tool --format json
```

## 状态

- [x] 阶段 0：环境搭建（MSVC 14.41 / WinSDK 10.0.22621 / CMake 3.29.5 / Ninja 1.11）
- [x] 阶段 1（M1）：离线闭环 — mock fixture → parse → chain → mermaid
- [ ] 阶段 2（M2）：真流量接入（反向代理 + 录制）
- [ ] 阶段 3（M3）：分析质量（superstep / fuzzy / validate / cost / report）
- [ ] 阶段 4（M4）：工程化（diff / prune / list / export / 性能）
