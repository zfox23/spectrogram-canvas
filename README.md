# Spectrogram Draw Filter

A VST3 effect that shows the spectrogram of the audio passing through it and lets you
paint the time-frequency regions you want to keep. Everything you don't paint is
attenuated to the **Background Gain** level.

> Status: early development.

## Building (Windows)

Requirements: Visual Studio 2026 (or Build Tools) with the *Desktop development with C++*
workload, which includes CMake and Ninja.

From a *x64 Native Tools* / developer prompt:

```
cmake --preset vs2026
cmake --build --preset release
ctest --preset release
```

The plugin is written to `build/SpectrogramDrawFilter_artefacts/Release/VST3/Spectrogram Draw Filter.vst3`.
Copy that folder to `C:\Program Files\Common Files\VST3`, then rescan plugins in your host
(in Audacity: *Effect → Plugin Manager → Rescan*).

JUCE 8 is fetched automatically at configure time.

## License

AGPL-3.0 — see [LICENSE](LICENSE). Built with [JUCE](https://juce.com).
