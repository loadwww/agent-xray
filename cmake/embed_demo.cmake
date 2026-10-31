# 把 demo/ 下的示例文件转成一个 C++ 源文件，内嵌进 axr 二进制（axr demo 用）。
# 这样安装后不需要额外 assets，也不依赖工作目录。
#
# 用法：cmake -DDEMO_DIR=<demo 目录> -DOUT_FILE=<生成路径> -P embed_demo.cmake
#
# 注意：MSVC 对单个字符串字面量有长度上限（约 16K），因此按换行切成多段，
# 相邻字面量在 C++ 里会自动拼接；只在新行处切分，既是 ASCII 边界也不会切坏 UTF-8。

if(NOT DEFINED DEMO_DIR OR NOT DEFINED OUT_FILE)
    message(FATAL_ERROR "需要 -DDEMO_DIR=<dir> -DOUT_FILE=<file>")
endif()

set(CHUNK_LIMIT 8000)

file(GLOB_RECURSE demo_files RELATIVE "${DEMO_DIR}" "${DEMO_DIR}/*")
list(SORT demo_files)

file(WRITE "${OUT_FILE}" "// 由 cmake/embed_demo.cmake 自动生成，请勿手改\n#include \"cli/demo_assets.h\"\n\nnamespace axr {\nnamespace demo {\n\nconst DemoFile kDemoFiles[] = {\n")
foreach(f ${demo_files})
    file(READ "${DEMO_DIR}/${f}" content)
    string(LENGTH "${content}" total)
    set(pieces "")
    set(pos 0)
    while(pos LESS total)
        math(EXPR remain "${total} - ${pos}")
        set(len ${remain})
        if(remain GREATER CHUNK_LIMIT)
            set(len ${CHUNK_LIMIT})
            string(SUBSTRING "${content}" ${pos} ${len} head)
            string(FIND "${head}" "\n" nl REVERSE)
            if(nl GREATER -1)
                math(EXPR len "${nl} + 1")
            endif()
        endif()
        string(SUBSTRING "${content}" ${pos} ${len} chunk)
        string(APPEND pieces "R\"AXR_DEMO(${chunk})AXR_DEMO\"\n    ")
        math(EXPR pos "${pos} + ${len}")
    endwhile()
    file(APPEND "${OUT_FILE}" "    {\"${f}\",\n    ${pieces}},\n")
endforeach()
file(APPEND "${OUT_FILE}" "};\n\nconst int kDemoFileCount = static_cast<int>(sizeof(kDemoFiles) / sizeof(kDemoFiles[0]));\n\n} // namespace demo\n} // namespace axr\n")
