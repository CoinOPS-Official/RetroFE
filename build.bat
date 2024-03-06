@echo off
REM Change to the directory where the batch file is located
cd /d %~dp0

REM Execute the first cmake command
cmake -A x64 -B .\RetroFE\Build -D GSTREAMER_ROOT=C:\gstreamer\1.0\msvc_x86_64 -S .\RetroFE\Source

REM Check if the previous command was successful
if %errorlevel% neq 0 (
    echo Failed to configure the project, exiting...
    exit /b %errorlevel%
)

REM Execute the second cmake command
cmake --build .\RetroFE\Build --config Release
