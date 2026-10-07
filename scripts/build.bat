@echo off
setlocal

where cmake >nul 2>&1
if errorlevel 1 (
  echo CMake not found in PATH. Open a new terminal after install, or add CMake\bin to PATH.
  exit /b 1
)

cmake -S "%~dp0." -B "%~dp0build" -G "Visual Studio 17 2022" -A x64
if errorlevel 1 exit /b 1

cmake --build "%~dp0build" --config Release
if errorlevel 1 exit /b 1

echo.
echo Built: %~dp0build\Release\instant_replay.exe
endlocal
