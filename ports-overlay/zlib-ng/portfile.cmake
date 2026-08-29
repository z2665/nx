# overlay：基于 vcpkg 基线（e03dc9b）的 zlib-ng 2.3.3 端口，
# 将上游留给 triplet 的 ZLIB_COMPAT 开关改为 feature（zlib-ng[compat]）。
# compat 模式：安装 zlib.h/zconf.h、库名 zlibstatic（静态 MSVC）、
# CMake 配置目录 share/ZLIB → find_package(ZLIB) 即得 ZLIB::ZLIB，
# libarchive 与 nx 的直连 zlib 代码均零改动获得 SIMD inflate/CRC。
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO zlib-ng/zlib-ng
    REF "${VERSION}"
    SHA512 e2057c764f1d5aaee738edee7e977182c5b097e3c95489dcd8de813f237d92a05daaa86d68d44b331d9fec5d1802586a8f6cfb658ba849874aaa14e72a8107f5
    HEAD_REF develop
)

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        compat   ZLIB_COMPAT
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        "-DZLIB_FULL_VERSION=${ZLIB_FULL_VERSION}"
        -DZLIB_ENABLE_TESTS=OFF
        -DWITH_NEW_STRATEGIES=ON
        ${FEATURE_OPTIONS}
    OPTIONS_RELEASE
        -DWITH_OPTIM=ON
)

vcpkg_cmake_install()
vcpkg_copy_pdbs()

# 库名规则（静态 MSVC）：compat=zlibstatic / 原生=zlibstatic-ng —— 修 pkgconfig 引用
if(VCPKG_TARGET_IS_WINDOWS AND (NOT (VCPKG_LIBRARY_LINKAGE STREQUAL static AND VCPKG_TARGET_IS_MINGW)))
    set(_port_suffix)
    if(ZLIB_COMPAT)
        set(_port_suffix "")
    else()
        set(_port_suffix "-ng")
    endif()
    set(_port_output_name)
    if(VCPKG_LIBRARY_LINKAGE STREQUAL "dynamic")
        set(_port_output_name "zlib${_port_suffix}")
    else()
        set(_port_output_name "zlibstatic${_port_suffix}")
    endif()
    if(NOT DEFINED VCPKG_BUILD_TYPE OR VCPKG_BUILD_TYPE STREQUAL "release")
        vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/lib/pkgconfig/zlib${_port_suffix}.pc" " -lz${_port_suffix}" " -l${_port_output_name}")
    endif()
    if(NOT DEFINED VCPKG_BUILD_TYPE OR VCPKG_BUILD_TYPE STREQUAL "debug")
        vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/debug/lib/pkgconfig/zlib${_port_suffix}.pc" " -lz${_port_suffix}" " -l${_port_output_name}d")
    endif()
endif()

vcpkg_fixup_pkgconfig()

if(ZLIB_COMPAT)
    set(_cmake_dir "ZLIB")
else()
    set(_cmake_dir "zlib-ng")
endif()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/${_cmake_dir})

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share"
                    "${CURRENT_PACKAGES_DIR}/debug/include"
)

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE.md")
