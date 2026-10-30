@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat" >nul
if errorlevel 1 goto :eof

set NASM=%TEMP%\opencode\nasm\nasm-2.16.03\nasm.exe
if defined NASM_SET goto have-nasm
if exist "%TEMP%\opencode\nasm\nasm-2.16.03\nasm.exe" goto have-nasm
where nasm >nul 2>&1 && for /f "delims=" %%i in ('where nasm') do set NASM=%%i
:have-nasm

"%NASM%" -f win32 --prefix _ -o x86_helpers.obj x86_helpers.asm
if errorlevel 1 goto :eof

cl /nologo /O2 /W3 /c fury.c
if errorlevel 1 goto :eof

cl /nologo /O2 /W3 /c sound.c
if errorlevel 1 goto :eof

cl /nologo /O2 /W3 /EHsc /c dbopl.cpp
if errorlevel 1 goto :eof

cl /nologo /O2 /W3 /c opl3.c
if errorlevel 1 goto :eof

cl /nologo /O2 /W3 /c kmdplayer.c
if errorlevel 1 goto :eof

link /nologo /SUBSYSTEM:CONSOLE /BASE:0x10000000 /DYNAMICBASE:NO /MAP /OUT:fury.exe ^
     fury.obj sound.obj opl3.obj kmdplayer.obj dbopl.obj x86_helpers.obj kernel32.lib user32.lib winmm.lib
if errorlevel 1 goto :eof

echo BUILD OK
