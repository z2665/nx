@echo off
setlocal
cd /d "%~dp0"
set "CLANG=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\Llvm\x64\bin\clang-cl.exe"
"%CLANG%" /nologo /O0 /fsanitize=address /Ikrml_glue /Ikrml_out /I..\fstar\include\krml /I..\fstar\lib\krml\dist\generic ^
  shim_main.c krml_out\Closure.c ..\fstar\lib\krml\c\prims.c ..\fstar\lib\krml\dist\generic\fstar_int32.c ^
  /Fe:closure_check_asan.exe /link /nologo
endlocal
