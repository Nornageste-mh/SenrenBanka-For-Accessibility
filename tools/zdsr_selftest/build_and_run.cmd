@echo off
rem ===========================================================================
rem  Build + run the ZDSR backend self-test (32-bit MSVC).
rem
rem  Requires:
rem    VCVARS   full path to vcvars32.bat. Defaults to the VS2022 BuildTools
rem             location when that file exists.
rem
rem  Builds tools\zdsr_selftest\out\{zdsr_stub.dll,zdsr_stub_noGetState.dll,
rem  zdsr_stub_mismatch.dll,zdsr_selftest.exe}, prints the stub export table (the
rem  names must be undecorated, exactly like the real ZDSRAPI.dll, or the test
rem  proves nothing), runs the assertions and writes zdsr_selftest_report.txt in
rem  the repo root.
rem
rem  zdsr_stub_mismatch.dll is the "InitTTS always returns 1 (version mismatch)"
rem  variant: the auto-skip-a-stale-copy path must hold independently of any
rem  environment variable, so it gets a stub of its own.
rem
rem  NOTE: keep this file ASCII-only and CRLF-terminated (same reason as build.cmd:
rem  cmd.exe parses it in the OEM codepage and mis-handles UTF-8 / lone LF).
rem ===========================================================================
setlocal
set ROOT=%~dp0..\..
set OUT=%~dp0out
set TRACE_FLAG=
if /i "%ZDSR_TRACE%"=="1" set TRACE_FLAG=/DZDSR_TRACE

if "%VCVARS%"=="" set VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat
if not exist "%VCVARS%" (
  echo [x] vcvars32.bat not found: "%VCVARS%" - set VCVARS
  exit /b 1
)

call "%VCVARS%" >nul || exit /b 1
if /i not "%VSCMD_ARG_TGT_ARCH%"=="x86" (
  echo [x] wrong target arch: VSCMD_ARG_TGT_ARCH="%VSCMD_ARG_TGT_ARCH%" - use vcvars32.bat
  exit /b 1
)

if not exist "%OUT%" mkdir "%OUT%"

echo === building the stub DLLs ===
cl /nologo /c /utf-8 /O2 /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /Fo"%OUT%\zdsr_stub.obj" "%~dp0zdsr_stub.c" || exit /b 1
link /nologo /DLL /MACHINE:X86 /DEF:"%~dp0zdsr_stub.def" /OUT:"%OUT%\zdsr_stub.dll" "%OUT%\zdsr_stub.obj" || exit /b 1
cl /nologo /c /utf-8 /O2 /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /DZDSR_STUB_NO_GETSTATE /Fo"%OUT%\zdsr_stub_noGetState.obj" "%~dp0zdsr_stub.c" || exit /b 1
link /nologo /DLL /MACHINE:X86 /DEF:"%~dp0zdsr_stub_noGetState.def" /OUT:"%OUT%\zdsr_stub_noGetState.dll" "%OUT%\zdsr_stub_noGetState.obj" || exit /b 1
cl /nologo /c /utf-8 /O2 /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /Fo"%OUT%\zdsr_stub_mismatch.obj" "%~dp0zdsr_stub_mismatch.c" || exit /b 1
link /nologo /DLL /MACHINE:X86 /DEF:"%~dp0zdsr_stub_mismatch.def" /OUT:"%OUT%\zdsr_stub_mismatch.dll" "%OUT%\zdsr_stub_mismatch.obj" || exit /b 1

echo === building the self-test exe (32-bit) ===
cd /d "%OUT%"
rem advapi32.lib is what the registry auto-discovery needs; winreg.h does not
rem carry a "#pragma comment(lib,...)" (checked), so the linker needs it named.
cl /nologo /EHsc /MT /O1 /utf-8 /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE %TRACE_FLAG% /I"%ROOT%\src" /Fe"zdsr_selftest.exe" "%~dp0zdsr_selftest.cpp" advapi32.lib || exit /b 1

echo === stub export table (names must be undecorated) ===
dumpbin /exports "%OUT%\zdsr_stub.dll" | findstr /R /C:" InitTTS" /C:" Speak" /C:" GetSpeakState" /C:" StopSpeak" /C:" Braille"
dumpbin /exports "%OUT%\zdsr_stub_noGetState.dll" | findstr /R /C:" InitTTS" /C:" Speak" /C:" GetSpeakState" /C:" StopSpeak" /C:" Braille"
dumpbin /exports "%OUT%\zdsr_stub_mismatch.dll" | findstr /R /C:" InitTTS" /C:" Speak" /C:" GetSpeakState" /C:" StopSpeak" /C:" Braille"

echo === running the self-test ===
"%OUT%\zdsr_selftest.exe" "%ROOT%\zdsr_selftest_report.txt"
echo selftest_exit=%errorlevel%
echo report=%ROOT%\zdsr_selftest_report.txt