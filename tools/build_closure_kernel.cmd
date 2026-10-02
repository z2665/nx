@echo off
rem 编译 F* 验证 + KaRaMeL 抽取的闭包内核为 closure_check.exe（批次 5）
rem 用 clang-cl（krml 头的 quoted-include 语义更可靠）；产物供 audit_ownership.py 调用
setlocal
set "VSROOT=C:\Program Files\Microsoft Visual Studio\18\Community"
set "CLANG=%VSROOT%\VC\Tools\Llvm\x64\bin\clang-cl.exe"
cd /d "%~dp0proofs"
"%CLANG%" /nologo /O2 /W4 /Ikrml_glue /Ikrml_out /I..\fstar\include\krml /I..\fstar\lib\krml\dist\generic ^
   shim_main.c krml_out\Closure.c ..\fstar\lib\krml\c\prims.c ..\fstar\lib\krml\dist\generic\fstar_int32.c /Fe:closure_check.exe /link /nologo
if errorlevel 1 (echo [kernel] 编译失败 & exit /b 1)
echo [kernel] closure_check.exe 就绪
endlocal
