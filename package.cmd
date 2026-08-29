@echo off
rem package.cmd - M3: bundle all resources into one portable folder (no install needed)
rem output: dist\nx\  (nx.exe + 7z.dll + README + LICENSE.txt)
rem usage: run build.cmd first; then package.cmd; register menu via dist\nx\nx.exe menu install
setlocal
set "OUT=%~dp0dist\nx"
if not exist "build\nx.exe" (
  echo [package] build\nx.exe missing - run build.cmd first
  exit /b 1
)
if exist "%~dp0dist" rmdir /s /q "%~dp0dist"
mkdir "%OUT%" || exit /b 1

copy /y "build\nx.exe" "%OUT%\nx.exe" >nul || exit /b 1
echo [package] nx.exe

if exist "C:\Program Files\7-Zip\7z.dll" (
  copy /y "C:\Program Files\7-Zip\7z.dll" "%OUT%\7z.dll" >nul
  echo [package] 7z.dll
) else (
  echo [package] WARN: 7z.dll not found - 7z/rar fall back to libarchive, rar multivolume unavailable
)

copy /y "README.md" "%OUT%\README.md" >nul
copy /y "LICENSE-distro.txt" "%OUT%\LICENSE.txt" >nul
echo [package] README.md + LICENSE.txt

echo.
echo [package] done: dist\nx\
echo [package] context menu: run  nx.exe menu install  inside dist\nx
endlocal
