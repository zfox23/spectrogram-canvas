# Spectrogram Draw Filter

A VST3 effect that shows the spectrogram of the audio passing through it and lets you
**paint the time-frequency regions you want to keep**. Everything you don't paint is
attenuated to the **Background Gain** level.

Use it to pull a single instrument, voice or tone out of a recording, or (with **Invert**)
to remove one.

> Status: early development. Tested on Windows with Audacity 3.5.

## How it works

A plugin only ever sees audio as the host plays or renders it, so the workflow has two passes:

1. **Capture.** Press **Capture**, then play the audio (or Apply the effect). The plugin records
   what passes through it and draws its spectrogram. While capturing, audio passes through
   unchanged.
2. **Paint, then play or Apply.** Paint over what you want to keep. On the next playback or Apply,
   only the painted regions come through.

The painting is tied to positions on the host's timeline, so it lines up with the audio it
was captured from.

## Controls

| Control | What it does |
|---|---|
| **Left-drag** | Paint (keep these frequencies) |
| **Right-drag** | Erase |
| **Mouse wheel** | Brush size |
| **Ctrl + wheel** | Zoom time around the cursor |
| **Ctrl + Shift + wheel** | Zoom frequency around the cursor |
| **Shift + wheel** | Scroll in time |
| **Middle-drag** or **Alt + left-drag** | Pan |
| **Ctrl+Z / Ctrl+Y** | Undo / redo |
| Overview strip | Drag the box to move through time, drag its edges to zoom, click to jump, wheel to zoom |
| **Capture** | Record the audio passing through (stops automatically when playback stops) |
| **Clear Audio** | Forget the captured audio (painting is kept) |
| **Clear Paint** | Erase all painting (undoable) |
| **Invert** | Play everything *except* what you painted |
| **Background Gain** | How much unpainted audio leaks through (−144 dB = silent, up to +12 dB) |
| **FFT Size** | Frequency vs. time precision. Larger values (8192, 16384) separate close frequencies better but smear fast events; smaller values (512, 1024) keep transients sharp but blur pitch. 4096 is a good start. Latency equals the FFT size. |
| **Hardness** | 0 = soft, feathered brush; 100 = hard edge |
| **Freq Scale** | Log (default), Linear or Mel display. Your painting is kept when you switch. |
| **i** | Host diagnostics (timeline position, sample rate, latency, capture range) |

## Using it in Audacity

**As a realtime effect** (recommended — you hear changes live):

1. Click **Effects** on the track's header and add *Spectrogram Draw Filter*.
2. Open its window, press **Capture**, and play the part of the track you want to work on.
3. Stop playback. The spectrogram appears and capture stops by itself.
4. Paint, then play again to hear the result. Keep painting and listening.
5. To make it permanent, export the audio or use Audacity's command to apply realtime effects to the track.

**As a destructive effect** (Effect menu):

1. Select the audio, open *Effect → Spectrogram Draw Filter*, press **Capture**, then **Apply**.
   The audio is passed through unchanged while being recorded.
2. Open the effect again (*Effect → Repeat* or the Effect menu). The captured spectrogram is shown.
3. Paint, then **Apply**.

Captured audio is cached in `%APPDATA%\SpectrogramDrawFilter\captures` so it survives closing the
plugin window. Cached files older than 30 days are deleted automatically.

## Building (Windows)

Requirements: Visual Studio 2026 or its Build Tools, with the *Desktop development with C++*
workload (includes CMake). JUCE 8 is downloaded automatically.

```powershell
.\scripts\build.ps1            # configure, build Release, run tests
.\scripts\build.ps1 -Install   # ...and copy into C:\Program Files\Common Files\VST3 (run as administrator)
```

Or manually, from a developer prompt:

```
cmake --preset vs2026
cmake --build --preset release
ctest --preset release
```

The plugin ends up in `build/SpectrogramDrawFilter_artefacts/Release/VST3/`. After installing,
rescan in your host (Audacity: *Effect → Plugin Manager → Rescan*).

`build/SdfSnapshot_artefacts/Release/SdfSnapshot.exe out.png [width height] [invert,linear]`
renders the editor with synthetic audio to a PNG, which is handy for checking UI changes.

## Project layout

```
src/dsp/        STFT mask filter (SpectralMaskProcessor)
src/model/      Painted mask grid, frequency scales
src/capture/    Lock-free capture buffer + WAV cache
src/analysis/   Spectrogram analysis and rendering
src/session/    Shared per-document state (mask + capture) across plugin instances
src/ui/         Spectrogram view, overview, theme
tests/          Unit tests (JUCE UnitTest)
tools/          Editor snapshot tool
```

## License

AGPL-3.0, see [LICENSE](LICENSE). Built with [JUCE](https://juce.com).
