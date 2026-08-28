@echo off
rem nx build helper: VS dev prompt (vcvars64) + vcpkg + Ninja + cmake
setlocal
set "HTTP_PROXY=http://127.0.0.1:10808"
set "HTTPS_PROXY=http://127.0.0.1:10808"
set "http_proxy=http://127.0.0.1:10808"
set "https_proxy=http://127.0.0.1:10808"
set "VSROOT=C:\Program Files\Microsoft Visual Studio\18\Community"
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
  echo [nx] failed to call vcvars64.bat
  exit /b 1
)
rem vcvars 不一定带 CMake/Ninja，显式补 PATH
set "PATH=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
where cmake >nul 2>&1 || (
  echo [nx] cmake not found
  exit /b 1
)
where ninja >nul 2>&1 || (
  echo [nx] ninja not found
  exit /b 1
)
cmake -S . -B build -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release ^
  "-DCMAKE_TOOLCHAIN_FILE=%VSROOT%\VC\vcpkg\scripts\buildsystems\vcpkg.cmake" ^
  -DVCPKG_TARGET_TRIPLET=x64-windows-static ^
  "-DVCPKG_INSTALL_OPTIONS=--overlay-ports=E:/zproject/zunzip/ports-overlay" || exit /b 1
cmake --build build -- %* || exit /b 1
endlocal
