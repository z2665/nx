cmake_minimum_required(VERSION 3.20)
project(nx VERSION 0.1.0 LANGUAGES CXX)   # 唯一版本源：--version/发版对齐改这里

set(CMAKE_CXX_STANDARD 23)   # 批次 1：C++20→23（std::expected/Result 别名承接；ABI 无影响）
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(MSVC)
  add_compile_options(/utf-8 /W4 /permissive-)
  add_compile_definitions(_WIN32_WINNT=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN CRT_SECURE_NO_WARNINGS)
endif()

find_package(LibArchive REQUIRED)
find_package(ZLIB REQUIRED)   # zlib-ng[compat]（overlay 端口）：compat 模式导出标准 ZLIB 配置
find_package(BZip2 REQUIRED)
find_package(LibLZMA REQUIRED)
find_package(zstd CONFIG REQUIRED)
find_package(lz4 CONFIG REQUIRED)

# 不同 vcpkg 版本导出的目标名不同，做静态/共享目标回退
if(TARGET zstd::libzstd_static)
  set(NX_ZSTD zstd::libzstd_static)
elseif(TARGET zstd::libzstd)
  set(NX_ZSTD zstd::libzstd)
else()
  message(FATAL_ERROR "zstd target not found")
endif()

if(TARGET lz4::lz4)
  set(NX_LZ4 lz4::lz4)
elseif(TARGET LZ4::lz4_static)
  set(NX_LZ4 LZ4::lz4_static)
elseif(TARGET LZ4::lz4_shared)
  set(NX_LZ4 LZ4::lz4_shared)
elseif(TARGET LZ4::lz4)
  set(NX_LZ4 LZ4::lz4)
else()
  message(FATAL_ERROR "lz4 target not found")
endif()

add_library(nxshell SHARED src/nxshell.cpp)
target_include_directories(nxshell PRIVATE src)   # res/ RAII 头（P2 圈禁，批次 5）
# COM DLL 由资源管理器进程内加载：仅链接、不启用 PCH/统一UTF8 也无妨
set_target_properties(nxshell PROPERTIES OUTPUT_NAME "nxshell")

# 除入口外的全部管线源（nx 主程序与 fuzz 目标共用）
set(NX_CORE_SOURCES
  src/util.cpp
  src/bytesource.cpp
  src/pushback.cpp
  src/pipes.cpp
  src/spool.cpp
  src/diag.cpp
  src/detect.cpp
  src/stego.cpp
  src/password.cpp
  src/volumeset.cpp
  src/filter.cpp
  src/namecodec.cpp
  src/laseq.cpp
  src/open.cpp
  src/szcom.cpp
  src/views.cpp
  src/sink.cpp
  src/walker.cpp
  src/gui.cpp
  src/log.cpp
  src/menu.cpp
  src/report.cpp
)

add_executable(nx
  src/main.cpp
  ${NX_CORE_SOURCES}
)

# 双模式：资源管理器启动不闪黑框；终端/管道行为不变（继承句柄）
target_link_options(nx PRIVATE "/SUBSYSTEM:WINDOWS" "/ENTRY:mainCRTStartup")

# Release 全速：LTO（跨模块内联，尤其 sink/spool/engine 回调链）；/GS 等安全检查保留
if(MSVC AND CMAKE_BUILD_TYPE STREQUAL "Release")
  target_compile_options(nx PRIVATE /GL /Gy /Oi)
  target_link_options(nx PRIVATE /LTCG)
endif()

target_include_directories(nx PRIVATE src)
# 版本经编译期宏注入（源=project VERSION）——不用生成头：clang-tidy/audit 等
# 旁路工具链自建编译参数，生成头的构建目录 include 会漏（main.cpp 兜底宏兜住）
target_compile_definitions(nx PRIVATE NX_VERSION="${PROJECT_VERSION}")
target_link_libraries(nx PRIVATE
  LibArchive::LibArchive
  ZLIB::ZLIB
  BZip2::BZip2
  LibLZMA::LibLZMA
  ${NX_ZSTD}
  ${NX_LZ4}
  comctl32
)

# 泄漏哨兵 S1-S5（roadmap §7.2）对主程序开放（诊断构建用；fuzz 目标始终开启）
option(NX_DIAG_LEAKS_MAIN "nx 主程序编译泄漏哨兵（诊断用）" OFF)
if(NX_DIAG_LEAKS_MAIN)
  target_compile_definitions(nx PRIVATE NX_DIAG_LEAKS)
endif()

# ---- 纯核心单元测试目标（批次 1 起；run_tests.py 自动调用，非 EXCLUDE 保持随主构建产出）----
add_executable(nxunit
  tests/unit_main.cpp
  ${NX_CORE_SOURCES}
)
target_include_directories(nxunit PRIVATE src)
target_link_libraries(nxunit PRIVATE
  LibArchive::LibArchive
  ZLIB::ZLIB
  BZip2::BZip2
  LibLZMA::LibLZMA
  ${NX_ZSTD}
  ${NX_LZ4}
  comctl32
)

# ---- libFuzzer 全管线目标（tests/fuzz_run.py 自动配置构建；见 README「Fuzz 安全护城河」）----
# vcpkg 依赖为非插桩静态库：仅本仓代码带 ASan/覆盖率，依赖库内崩溃仍可被捕获。
option(NX_FUZZ "构建 libFuzzer+ASan 目标 nxfuzz（需 VS 的 AddressSanitizer 组件）" OFF)
if(NX_FUZZ)
  add_executable(nxfuzz EXCLUDE_FROM_ALL
    ${NX_CORE_SOURCES}
    src/fuzz_main.cpp
  )
  target_include_directories(nxfuzz PRIVATE src)
  target_link_libraries(nxfuzz PRIVATE
    LibArchive::LibArchive
    ZLIB::ZLIB
    BZip2::BZip2
    LibLZMA::LibLZMA
    ${NX_ZSTD}
    ${NX_LZ4}
    comctl32
  )
  # 注意：不能开 /GL（与 sanitizer 不兼容）；MSVC libFuzzer 同时提供驱动 main
  target_compile_options(nxfuzz PRIVATE /fsanitize=address /fsanitize=fuzzer)
  target_link_options(nxfuzz PRIVATE /fsanitize=address /fsanitize=fuzzer)
  # 泄漏哨兵常开（S1-S5，src/diag.hpp）：泄漏回归 = abort = fuzz 崩溃
  target_compile_definitions(nxfuzz PRIVATE NX_DIAG_LEAKS)
  # 动态 ASan 运行时随构建复制到 exe 旁——否则脱离 VS 环境启动报 0xC0000141
  get_filename_component(NX_CXX_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
  add_custom_command(TARGET nxfuzz POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${NX_CXX_DIR}/clang_rt.asan_dynamic-x86_64.dll"
            $<TARGET_FILE_DIR:nxfuzz>)
endif()
