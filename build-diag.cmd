@echo off
rem nx diag build helper: 泄漏哨兵开启的诊断构建（roadmap §7.2 S1-S5）
rem 产物 build-diag\nx.exe——spool/读取器泄漏、try_open 失败出口、析构纪律违规即 abort。
rem 与 build.cmd 同参（代理/VSROOT 硬编码为本机），仅多 -DNX_DIAG_LEAKS_MAIN=ON。
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
cmake -S . -B build-diag -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DNX_DIAG_LEAKS_MAIN=ON ^
  "-DCMAKE_TOOLCHAIN_FILE=%VSROOT%\VC\vcpkg\scripts\buildsystems\vcpkg.cmake" ^
  -DVCPKG_TARGET_TRIPLET=x64-windows-static ^
  "-DVCPKG_INSTALL_OPTIONS=--overlay-ports=E:/zproject/zunzip/ports-overlay" || exit /b 1
cmake --build build-diag -- %* || exit /b 1
endlocal
