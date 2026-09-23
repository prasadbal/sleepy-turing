@echo off
REM Builds and runs both mmap designs' tests from this directory.
REM Requires MSVC (Visual Studio's x64 dev environment); adjust the
REM vcvarsall.bat path below for your install if it differs.
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1
set SP=%~dp0
REM cl.exe cannot write intermediate/output files to a UNC path (\\wsl$\...):
REM source is read from here (SP), but objects/exes are written locally.
set OUT=%TEMP%\mmap_writer_build
if not exist "%OUT%" mkdir "%OUT%"

echo === segmented_mmap.h (sliding window) ===
cl /nologo /std:c++latest /O2 /EHa /W4 /I "%SP%." "%SP%segmented_mmap_test.cpp" /Fe:"%OUT%\segmented_mmap_test.exe" /Fo:%OUT%\
if errorlevel 1 (echo BUILD FAILED & exit /b 1)
"%OUT%\segmented_mmap_test.exe"

echo.
echo === growable_mmap_win.h (remap-from-0, superseded but kept for reference) ===
cl /nologo /std:c++latest /O2 /EHsc /W4 /I "%SP%." "%SP%test_growable_mmap.cpp" /Fe:"%OUT%\test_growable_mmap.exe" /Fo:%OUT%\
if errorlevel 1 (echo BUILD FAILED & exit /b 1)
"%OUT%\test_growable_mmap.exe"
