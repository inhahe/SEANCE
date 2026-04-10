@echo off
echo ============================================
echo SoundShop2 Dependency Setup
echo ============================================
echo.
echo This script downloads dependencies that cannot be
echo redistributed with the source code.
echo.

set BASE=%~dp0

:: Create directories
if not exist "%BASE%third_party" mkdir "%BASE%third_party"

:: ============================================
:: ASIO SDK (Steinberg)
:: JUCE 8 bundles ASIO headers internally, so
:: this is only needed if JUCE_ASIO_USE_EXTERNAL_SDK=1.
:: With our setup (JUCE_ASIO=1, external SDK off),
:: no download is needed.
:: ============================================
echo [OK] ASIO: Using JUCE bundled headers (no download needed)
echo.

:: ============================================
:: libopenmpt (prebuilt DLL for MOD/S3M/IT/XM import)
:: ============================================
if exist "%BASE%third_party\libopenmpt_bin\lib\amd64\libopenmpt.lib" (
    echo [OK] libopenmpt: Already downloaded
) else (
    echo [..] Downloading libopenmpt...
    mkdir "%BASE%third_party\libopenmpt_bin" 2>nul
    curl -L -o "%BASE%third_party\libopenmpt_bin\libopenmpt-dev.zip" ^
        "https://lib.openmpt.org/files/libopenmpt/dev/libopenmpt-0.8.4+release.dev.windows.vs2022.zip"
    if errorlevel 1 (
        echo [FAIL] Download failed. Get it manually from https://lib.openmpt.org/libopenmpt/download/
    ) else (
        echo [..] Extracting...
        cd "%BASE%third_party\libopenmpt_bin"
        tar -xf libopenmpt-dev.zip
        echo [OK] libopenmpt: Downloaded and extracted
    )
)
echo.

:: ============================================
:: wasm3 (WASM interpreter for script nodes)
:: ============================================
if exist "%BASE%third_party\wasm3\source\wasm3.h" (
    echo [OK] wasm3: Already downloaded
) else (
    echo [..] Downloading wasm3...
    git clone --depth 1 https://github.com/nicholasgasior/wasm3 "%BASE%third_party\wasm3" 2>nul
    if errorlevel 1 (
        echo [..] Trying alternative URL...
        git clone --depth 1 https://github.com/nicholasgasior/nicholasgasior-wasm3 "%BASE%third_party\wasm3" 2>nul
    )
    if exist "%BASE%third_party\wasm3\source\wasm3.h" (
        echo [OK] wasm3: Downloaded
    ) else (
        echo [SKIP] wasm3: Download failed. WASM scripts will be disabled.
        echo        Clone manually: git clone https://github.com/nicholasgasior/nicholasgasior-wasm3 third_party/wasm3
    )
)
echo.

:: ============================================
:: Rubber Band (pitch shifting / time stretching)
:: ============================================
if exist "%BASE%third_party\rubberband\rubberband\RubberBandStretcher.h" (
    echo [OK] Rubber Band: Already downloaded
) else (
    echo [..] Downloading Rubber Band...
    git clone --depth 1 https://github.com/breakfastquay/rubberband.git "%BASE%third_party\rubberband" 2>nul
    if exist "%BASE%third_party\rubberband\rubberband\RubberBandStretcher.h" (
        :: Fix include path issue
        if not exist "%BASE%third_party\rubberband\src\common\system" mkdir "%BASE%third_party\rubberband\src\common\system"
        copy "%BASE%third_party\rubberband\src\common\sysutils.h" "%BASE%third_party\rubberband\src\common\system\sysutils.h" >nul
        echo [OK] Rubber Band: Downloaded
    ) else (
        echo [FAIL] Rubber Band: Download failed.
        echo        Clone manually: git clone https://github.com/breakfastquay/rubberband third_party/rubberband
    )
)
echo.

:: ============================================
:: libopus + libogg (for Opus export)
:: ============================================
if exist "%BASE%third_party\opus\include\opus.h" (
    echo [OK] libopus: Already downloaded
) else (
    echo [..] Downloading libopus...
    git clone --depth 1 https://github.com/xiph/opus.git "%BASE%third_party\opus" 2>nul
    if exist "%BASE%third_party\opus\include\opus.h" (
        echo [OK] libopus: Downloaded
    ) else (
        echo [FAIL] libopus: Download failed. Opus export will be disabled.
    )
)

if exist "%BASE%third_party\ogg\include\ogg\ogg.h" (
    echo [OK] libogg: Already downloaded
) else (
    echo [..] Downloading libogg...
    git clone --depth 1 https://github.com/xiph/ogg.git "%BASE%third_party\ogg" 2>nul
    if exist "%BASE%third_party\ogg\include\ogg\ogg.h" (
        :: Generate config_types.h if missing
        if not exist "%BASE%third_party\ogg\include\ogg\config_types.h" (
            echo #ifndef __CONFIG_TYPES_H__> "%BASE%third_party\ogg\include\ogg\config_types.h"
            echo #define __CONFIG_TYPES_H__>> "%BASE%third_party\ogg\include\ogg\config_types.h"
            echo #include ^<stdint.h^>>> "%BASE%third_party\ogg\include\ogg\config_types.h"
            echo typedef int16_t ogg_int16_t;>> "%BASE%third_party\ogg\include\ogg\config_types.h"
            echo typedef uint16_t ogg_uint16_t;>> "%BASE%third_party\ogg\include\ogg\config_types.h"
            echo typedef int32_t ogg_int32_t;>> "%BASE%third_party\ogg\include\ogg\config_types.h"
            echo typedef uint32_t ogg_uint32_t;>> "%BASE%third_party\ogg\include\ogg\config_types.h"
            echo typedef int64_t ogg_int64_t;>> "%BASE%third_party\ogg\include\ogg\config_types.h"
            echo typedef uint64_t ogg_uint64_t;>> "%BASE%third_party\ogg\include\ogg\config_types.h"
            echo #endif>> "%BASE%third_party\ogg\include\ogg\config_types.h"
        )
        echo [OK] libogg: Downloaded
    ) else (
        echo [FAIL] libogg: Download failed. Opus export will be disabled.
    )
)
echo.

:: ============================================
:: Summary
:: ============================================
echo ============================================
echo Setup complete. Now build with:
echo.
echo   cd cpp
echo   cmake -B build -G "Visual Studio 17 2022" -A x64
echo   cmake --build build --config Release
echo.
echo Optional dependencies that failed to download
echo will be skipped automatically (features disabled).
echo ============================================
pause
