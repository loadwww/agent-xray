#pragma once
// A2：`axr demo` 用的内置示例（由 cmake/embed_demo.cmake 从 demo/ 目录生成）
namespace axr {
namespace demo {

struct DemoFile {
    const char* path;    // 相对 demo 根目录，如 runs/<session>/run.json
    const char* content; // UTF-8 文本
};

extern const DemoFile kDemoFiles[];
extern const int kDemoFileCount;

} // namespace demo
} // namespace axr
