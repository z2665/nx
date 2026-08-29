cmake_minimum_required(VERSION 3.20)
project(nx LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(MSVC)
  add_compile_options(/utf-8 /W4 /permissive-)
  add_compile_definitions(_WIN32_WINNT=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN CRT_SECURE_NO_WARNINGS)
endif()

find_package(LibArchive REQUIRED)
find_package(ZLIB REQUIRED)
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
# COM DLL 由资源管理器进程内加载：仅链接、不启用 PCH/统一UTF8 也无妨
set_target_properties(nxshell PROPERTIES OUTPUT_NAME "nxshell")

add_executable(nx
  src/main.cpp
  src/util.cpp
  src/bytesource.cpp
  src/pipes.cpp
  src/spool.cpp
  src/detect.cpp
  src/password.cpp
  src/volumeset.cpp
  src/filter.cpp
  src/engines.cpp
  src/szcom.cpp
  src/sink.cpp
  src/walker.cpp
  src/gui.cpp
  src/log.cpp
  src/menu.cpp
)

# 双模式：资源管理器启动不闪黑框；终端/管道行为不变（继承句柄）
target_link_options(nx PRIVATE "/SUBSYSTEM:WINDOWS" "/ENTRY:mainCRTStartup")

target_include_directories(nx PRIVATE src)
target_link_libraries(nx PRIVATE
  LibArchive::LibArchive
  ZLIB::ZLIB
  BZip2::BZip2
  LibLZMA::LibLZMA
  ${NX_ZSTD}
  ${NX_LZ4}
  comctl32
)
