@echo off
rem Build BGAPI Monitor with Qt's own MinGW toolchain (installed by the Qt online installer).
rem Edit the two paths below to match your Qt installation, then run from this folder.

set QT_DIR=C:\Qt\6.12.0\mingw_64
set MINGW_DIR=C:\Qt\Tools\mingw1510_64

set PATH=%QT_DIR%\bin;%MINGW_DIR%\bin;C:\Qt\Tools\CMake_64\bin;C:\Qt\Tools\Ninja;%PATH%

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=%QT_DIR% || goto :err
cmake --build build || goto :err
ctest --test-dir build --output-on-failure || goto :err

rem Copy the Qt DLLs next to the exe so it runs on any PC
if not exist dist mkdir dist
copy /y build\bgmon.exe dist\ >nul
windeployqt --release --no-translations dist\bgmon.exe || goto :err
echo.
echo Done: dist\bgmon.exe
goto :eof

:err
echo.
echo BUILD FAILED
exit /b 1
