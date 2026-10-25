@echo off
rem ============================================================
rem  Funky Stars RMX (ROR keygen version) - 构建脚本
rem  需要：llvm-mingw (i686, msvcrt) + UPX，均在 E:\project\test\tools
rem ============================================================
setlocal
set TOOLS=E:\project\test\tools
set PATH=%TOOLS%\llvm-mingw-20261006-msvcrt-i686\bin;%PATH%
set UPX=%TOOLS%\upx\upx.exe

echo [1/2] 编译 PE32 (i386, 无 CRT, -Oz -flto) ...
i686-w64-mingw32-clang -Oz -flto -nostdlib -ffreestanding -fno-builtin -fno-ident -fomit-frame-pointer ^
  -Wl,--entry=__start -Wl,-s -Wl,--no-insert-timestamp ^
  -o FunkyStars.exe src\main.c -lwinmm -lkernel32 -lmsvcrt
if errorlevel 1 goto fail

echo [2/2] UPX 加壳 (--best --lzma) ...
"%UPX%" --best --lzma -q -f -o FunkyStars_upx.exe FunkyStars.exe
if errorlevel 1 goto fail

for %%f in (FunkyStars.exe FunkyStars_upx.exe) do echo       %%~zf  %%~nxf
echo.
echo 完成。
goto :eof

:fail
echo 构建失败。
exit /b 1
