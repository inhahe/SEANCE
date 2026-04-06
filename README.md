# seance

A node-based digital audio workstation (DAW) designed to be intuitive for people without a music background.

## Building

### Prerequisites

- Windows 10/11, macOS, or Linux
- Visual Studio 2022 (Windows), Xcode (macOS), or GCC/Clang (Linux)
- CMake 3.22+
- [JUCE 8.0.12](https://juce.com/get-juce/download)
- Python 3.10+ (for scripting support)

### Setup

1. Install JUCE and note the path
2. Run the dependency setup script:

```
cd cpp
setup_dependencies.bat    (Windows)
```

3. Build:

```
cd cpp
cmake -B build -G "Visual Studio 17 2022" -A x64 -DJUCE_DIR=D:/JUCE-8.0.12
cmake --build build --config Release
```

On Linux:
```
cd cpp
cmake -B build -DJUCE_DIR=/path/to/JUCE
cmake --build build
```

### Optional Dependencies (downloaded by setup script)

- **Rubber Band** — pitch shifting / time stretching
- **libopenmpt** — MOD/S3M/IT/XM import
- **libopus + libogg** — Opus audio export
- **wasm3** — WASM script nodes

## Features

- Node-based signal routing with visible cables
- MIDI and audio timeline editing with piano roll
- VST3, AU (macOS), LV2, LADSPA (Linux) plugin hosting
- Built-in wavetable/terrain synthesizer with N-dimensional traversal
- Graintable synthesis
- MPE (MIDI Polyphonic Expression) support
- Python scripting for project manipulation
- WASM scripting for real-time audio DSP (C, Rust, Zig, AssemblyScript)
- Audio-rate and UI-rate signal connections
- Export to WAV, FLAC, OGG, Opus, M4A, WMA
- Pitch shifting / time stretching via Rubber Band
- MOD/S3M/IT/XM tracker file import
- Automation recording and playback
- MIDI CC learn and mapping
- ASIO support (Windows)
- Configurable project sample rate with internal resampling

See `CLAUDE.md` for detailed architecture documentation.
See `cpp/scripts/wasm_examples/README.md` for WASM scripting guide.
