# WXRST XTRACT

Sample extractor for WXRST: drop a sample (or record what plays through the insert), pick a region,
split it into stems (Demucs v4 hybrid transformer, running natively in C++), turn any stem into MIDI
(Spotify Basic Pitch for pitched parts, a 3-band onset detector for drums), then drag the WAV/MIDI
into FL Studio or send it to WXRST's CHORDS / MELODY / DRUMS inbox.

Built with JUCE 8. Formats: VST3 effect, AU effect, Standalone. macOS universal + Windows x64 (CI).

## Build

```
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
cmake --build build --config Release -j12
```

JUCE is taken from `./JUCE`, then `../VST1/JUCE`, then fetched. ONNX Runtime (prebuilt) is fetched by CMake.
The Demucs weights are NOT in the repo: the plugin downloads them on first use into the user's
app-data folder (`~/Library/Application Support/WXRST/models` / `%APPDATA%\WXRST\models`).

Console smoke test: `-DXTRACT_BUILD_TEST=ON` builds `XtractTest <audio file> [--6] [--drums] [out dir]`.

## Third party

- demucs.cpp (MIT, sevagh) - `third_party/demucs`
- basicpitch.cpp (MIT, sevagh) + Basic Pitch model (Apache 2.0, Spotify) - `third_party/basicpitch`
- Eigen (MPL2), libremidi (BSD), ONNX Runtime (MIT)
