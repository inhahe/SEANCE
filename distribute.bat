@echo off
echo ============================================
echo seance — Copy project for distribution
echo ============================================
echo.

set SRC=D:\visual studio projects\soundshop2
set DST=D:\github\seance

mkdir "%DST%"

:: ============================================
:: Root files
:: ============================================
copy "%SRC%\CLAUDE.md" "%DST%\" >nul
copy "%SRC%\main.py" "%DST%\" >nul

:: ============================================
:: cpp/ — source code
:: ============================================
mkdir "%DST%\cpp"
copy "%SRC%\cpp\CMakeLists.txt" "%DST%\cpp\" >nul
copy "%SRC%\cpp\setup_dependencies.bat" "%DST%\cpp\" >nul

:: cpp/src/
mkdir "%DST%\cpp\src"
xcopy "%SRC%\cpp\src\*.cpp" "%DST%\cpp\src\" /q >nul
xcopy "%SRC%\cpp\src\*.h" "%DST%\cpp\src\" /q >nul

:: cpp/include/
mkdir "%DST%\cpp\include"
xcopy "%SRC%\cpp\include\*" "%DST%\cpp\include\" /q >nul

:: cpp/scripts/
mkdir "%DST%\cpp\scripts"
copy "%SRC%\cpp\scripts\*.py" "%DST%\cpp\scripts\" >nul 2>nul
mkdir "%DST%\cpp\scripts\wasm_examples"
xcopy "%SRC%\cpp\scripts\wasm_examples\*" "%DST%\cpp\scripts\wasm_examples\" /q >nul

:: ============================================
:: Do NOT copy:
::   - cpp/build/          (build artifacts)
::   - cpp/third_party/    (downloaded by setup_dependencies.bat)
::   - cpp/src_imgui_backup/ (old code)
::   - cpp/external/       (if any vendored deps)
::   - __pycache__/
::   - *.ssp, *.cfg, *.dat (user project/config files)
::   - imgui.ini
::   - SoundShop.node_editor.json
::   - cservice-znc-module/
:: ============================================

:: ============================================
:: Create empty third_party dir with .gitkeep
:: ============================================
mkdir "%DST%\cpp\third_party"
echo. > "%DST%\cpp\third_party\.gitkeep"

:: ============================================
:: Test script
:: ============================================
if exist "%SRC%\test_all_features.py" (
    copy "%SRC%\test_all_features.py" "%DST%\" >nul
)

:: ============================================
:: Create README
:: ============================================
(
echo # seance
echo.
echo A node-based digital audio workstation ^(DAW^) designed to be intuitive for people without a music background.
echo.
echo ## Building
echo.
echo ### Prerequisites
echo.
echo - Windows 10/11, macOS, or Linux
echo - Visual Studio 2022 ^(Windows^), Xcode ^(macOS^), or GCC/Clang ^(Linux^)
echo - CMake 3.22+
echo - [JUCE 8.0.12](https://juce.com/get-juce/download^)
echo - Python 3.10+ ^(for scripting support^)
echo.
echo ### Setup
echo.
echo 1. Install JUCE and note the path
echo 2. Run the dependency setup script:
echo.
echo ```
echo cd cpp
echo setup_dependencies.bat    ^(Windows^)
echo ```
echo.
echo 3. Build:
echo.
echo ```
echo cd cpp
echo cmake -B build -G "Visual Studio 17 2022" -A x64 -DJUCE_DIR=D:/JUCE-8.0.12
echo cmake --build build --config Release
echo ```
echo.
echo On Linux:
echo ```
echo cd cpp
echo cmake -B build -DJUCE_DIR=/path/to/JUCE
echo cmake --build build
echo ```
echo.
echo ### Optional Dependencies ^(downloaded by setup script^)
echo.
echo - **Rubber Band** — pitch shifting / time stretching
echo - **libopenmpt** — MOD/S3M/IT/XM import
echo - **libopus + libogg** — Opus audio export
echo - **wasm3** — WASM script nodes
echo.
echo ## Features
echo.
echo - Node-based signal routing with visible cables
echo - MIDI and audio timeline editing with piano roll
echo - VST3, AU ^(macOS^), LV2, LADSPA ^(Linux^) plugin hosting
echo - Built-in wavetable/terrain synthesizer with N-dimensional traversal
echo - Graintable synthesis
echo - MPE ^(MIDI Polyphonic Expression^) support
echo - Python scripting for project manipulation
echo - WASM scripting for real-time audio DSP ^(C, Rust, Zig, AssemblyScript^)
echo - Audio-rate and UI-rate signal connections
echo - Export to WAV, FLAC, OGG, Opus, M4A, WMA
echo - Pitch shifting / time stretching via Rubber Band
echo - MOD/S3M/IT/XM tracker file import
echo - Automation recording and playback
echo - MIDI CC learn and mapping
echo - ASIO support ^(Windows^)
echo - Configurable project sample rate with internal resampling
echo.
echo See `CLAUDE.md` for detailed architecture documentation.
echo See `cpp/scripts/wasm_examples/README.md` for WASM scripting guide.
) > "%DST%\README.md"

:: ============================================
:: Create .gitignore
:: ============================================
(
echo # Build artifacts
echo cpp/build/
echo.
echo # Downloaded dependencies
echo cpp/third_party/*/
echo !cpp/third_party/.gitkeep
echo.
echo # User files
echo *.ssp
echo *.cfg
echo *.dat
echo *.xml
echo soundshop_cache/
echo soundshop_recent_*.txt
echo soundshop_window.xml
echo soundshop_prefs.xml
echo recordings/
echo.
echo # Python
echo __pycache__/
echo *.pyc
echo.
echo # IDE
echo .vs/
echo .vscode/
echo *.user
echo imgui.ini
) > "%DST%\.gitignore"

:: ============================================
:: Summary
:: ============================================
echo.
echo ============================================
echo Distribution copied to: %DST%
echo.
echo Contents:
dir /b "%DST%"
echo.
echo cpp/src/ files:
dir /b "%DST%\cpp\src\*.cpp" 2>nul | find /c /v ""
echo .cpp files
dir /b "%DST%\cpp\src\*.h" 2>nul | find /c /v ""
echo .h files
echo.
echo Remember to:
echo   1. cd %DST%
echo   2. git init ^&^& git add -A ^&^& git commit -m "Initial commit"
echo   3. git remote add origin https://github.com/yourusername/seance
echo   4. git push -u origin main
echo ============================================
pause
