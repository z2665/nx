@echo off
rem 编译 F* 验证 + KaRaMeL 抽取的闭包内核为 closure_check.exe
rem 依赖仅仓库内容：krml_out/（KaRaMeL 产物）+ krml_glue/（前置头）+
rem krml_runtime/（收编的 KaRaMeL 运行时，Apache-2.0）——CI 可完整复现。
rem clang-cl 定位：NX_VSROOT/VSROOT 环境变量 → 本机默认（VS2026）→ vswhere → 独立 LLVM。
setlocal
if not defined VSROOT set "VSROOT=C:\Program Files\Microsoft Visual Studio\18\Community"
if not exist "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" (
  for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
)
set "CLANG=%VSROOT%\VC\Tools\Llvm\x64\bin\clang-cl.exe"
if not exist "%CLANG%" set "CLANG=C:\Program Files\LLVM\bin\clang-cl.exe"
if not exist "%CLANG%" (echo [kernel] 找不到 clang-cl（VS Clang 组件或独立 LLVM） & exit /b 1)
cd /d "%~dp0proofs"
"%CLANG%" /nologo /O2 /W4 /Ikrml_glue /Ikrml_out /Ikrml_runtime\include\krml /Ikrml_runtime\karamel ^
   shim_main.c krml_out\Closure.c krml_runtime\c\prims.c krml_runtime\karamel\fstar_int32.c /Fe:closure_check.exe /link /nologo
if errorlevel 1 (echo [kernel] 编译失败 & exit /b 1)
echo [kernel] closure_check.exe 就绪（%CLANG%）
endlocal
