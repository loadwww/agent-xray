cmake_minimum_required(VERSION 3.16)
project(agentxray CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# header-only 依赖
include_directories(${CMAKE_SOURCE_DIR}/src)
include_directories(${CMAKE_SOURCE_DIR}/third_party)
include_directories(${CMAKE_SOURCE_DIR}/third_party/cpp-httplib)

file(GLOB_RECURSE AXR_SRC ${CMAKE_SOURCE_DIR}/src/*.cpp)
add_executable(axr ${AXR_SRC})

# 测试台（M2 起用于真实转发验证；M1 仅保证可编译）
add_executable(mock_openai ${CMAKE_SOURCE_DIR}/tests/mock_openai.cpp)

# 转发层依赖 libcurl，M2 阶段启用（见设计文档 §2.2）
# find_package(CURL REQUIRED)
# target_link_libraries(axr PRIVATE CURL::libcurl)

if(MSVC)
    target_compile_options(axr PRIVATE /utf-8 /W3 /permissive-)
    target_compile_options(mock_openai PRIVATE /utf-8 /W3 /permissive-)
endif()
