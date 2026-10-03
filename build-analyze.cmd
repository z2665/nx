@echo off
rem nx /analyze 排雷构建（批次 6，roadmap §7.3：MSVC 静态分析低噪子集——非门，顺手排雷；
rem 对所有权环零检出能力，不作主线）。产物 build-analyze\nx.exe。
rem 低噪 = 压掉 SAL 密集依赖型与 SDK 头批注噪声（6011/6387 在无完整 SAL 标注的
rem 三方边界上误报高；28251/28286/6553 为 WinSDK 批注自身不一致）；新增警告逐条
rem 人工分诊，不接入 run_tests。注：此版 cl 不支持 /analyze:plugin- 等子选项。
setlocal
set "HTTP_PROXY=http://127.0.0.1:10808"
set "HTTPS_PROXY=http://127.0.0.1:10808"
set "VSROOT=C:\Program Files\Microsoft Visual Studio\18\Community"
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
  echo [nx] failed to call vcvars64.bat
  exit /b 1
)
set "PATH=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
cmake -S . -B build-analyze -G Ninja ^
  -DCMAKE_BUILD_TYPE=Debug ^
  "-DCMAKE_CXX_FLAGS=/analyze /utf-8 /W4 /permissive- /EHsc /DNOMINMAX /DWIN32_LEAN_AND_MEAN /wd6011 /wd6387 /wd28159 /wd26495 /wd26439 /wd26451 /wd26821 /wd28251 /wd28286 /wd6553" ^
  "-DCMAKE_TOOLCHAIN_FILE=%VSROOT%\VC\vcpkg\scripts\buildsystems\vcpkg.cmake" ^
  -DVCPKG_TARGET_TRIPLET=x64-windows-static ^
  "-DVCPKG_INSTALL_OPTIONS=--overlay-ports=E:/zproject/zunzip/ports-overlay" || exit /b 1
cmake --build build-analyze -- %* || exit /b 1
endlocal
