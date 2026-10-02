@echo off
rem 拉取 TLC（TLA+ 模型检查器）——run_tests 的 ownership_tla 用例需要。
rem 已配置代理时可直接跑；否则手动下载后放入 tools\tla2tools.jar
rem   https://github.com/tlaplus/tlaplus/releases（tla2tools.jar，v1.8.0 验证过）
setlocal
set "PROXY=http://127.0.0.1:10808"
if exist "%~dp0tla2tools.jar" (
  echo [tla] tools\tla2tools.jar 已存在
  exit /b 0
)
curl -sS -x %PROXY% -L -o "%~dp0tla2tools.jar" ^
  https://github.com/tlaplus/tlaplus/releases/download/v1.8.0/tla2tools.jar
if errorlevel 1 (
  echo [tla] 下载失败：请手动下载 tla2tools.jar 放入 tools\
  exit /b 1
)
java -jar "%~dp0tla2tools.jar" -h >nul 2>&1 || (echo [tla] jar 校验失败 & exit /b 1)
echo [tla] 就绪
endlocal
