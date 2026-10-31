cmake_minimum_required(VERSION 3.16)
project(agentxray CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    # 单文件分发（§10 M4）：静态 CRT，避免额外 vcruntime140 / msvcp140 DLL 依赖
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# header-only 依赖
include_directories(${CMAKE_SOURCE_DIR}/src)
include_directories(${CMAKE_SOURCE_DIR}/third_party)
include_directories(${CMAKE_SOURCE_DIR}/third_party/cpp-httplib)

file(GLOB_RECURSE AXR_SRC ${CMAKE_SOURCE_DIR}/src/*.cpp)
add_executable(axr ${AXR_SRC})

# A2：把 demo/ 下的示例 run 内嵌进二进制（axr demo 离线可用，安装后不依赖 assets）
set(DEMO_DIR ${CMAKE_SOURCE_DIR}/demo)
set(DEMO_GEN_CPP ${CMAKE_BINARY_DIR}/generated/demo_assets.cpp)
file(GLOB_RECURSE DEMO_ASSET_DEPS ${DEMO_DIR}/*)
add_custom_command(
    OUTPUT ${DEMO_GEN_CPP}
    COMMAND ${CMAKE_COMMAND} -DDEMO_DIR=${DEMO_DIR} -DOUT_FILE=${DEMO_GEN_CPP}
            -P ${CMAKE_SOURCE_DIR}/cmake/embed_demo.cmake
    DEPENDS ${DEMO_ASSET_DEPS} ${CMAKE_SOURCE_DIR}/cmake/embed_demo.cmake
    COMMENT "生成 demo 内嵌资源"
)
target_sources(axr PRIVATE ${DEMO_GEN_CPP})

# 测试台（M2 起用于真实转发验证；M1 仅保证可编译）
add_executable(mock_openai ${CMAKE_SOURCE_DIR}/tests/mock_openai.cpp)

# 转发层与子进程的平台实现（P1-4）：Windows 用内置 WinHTTP；其他平台用 libcurl
if(WIN32)
    target_link_libraries(axr PRIVATE winhttp)
else()
    find_package(CURL REQUIRED)
    target_link_libraries(axr PRIVATE CURL::libcurl)
endif()

if(MSVC)
    target_compile_options(axr PRIVATE /utf-8 /W3 /permissive-)
    target_compile_options(mock_openai PRIVATE /utf-8 /W3 /permissive-)
endif()

# POSIX：httplib 的线程池需要 pthread（CI 的 linux job 依赖此行）
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(axr PRIVATE Threads::Threads)
    target_link_libraries(mock_openai PRIVATE Threads::Threads)
endif()
