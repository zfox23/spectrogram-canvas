# Spectrogram Canvas

A VST3 effect that shows the spectrogram of the audio passing through it and lets you
**paint the time-frequency regions you want to keep**. Everything you don't paint is
attenuated to the **Background Gain** level.

Use it to pull a single instrument, voice or tone out of a recording, or (with **Invert**)
to remove one.

> Status: early development. Windows; intended for Reaper and Audacity.

This repository holds two front-ends for the same idea:

| Folder | What it is |
|---|---|
| [`vst/`](vst) | The VST3 plugin (JUCE / C++). Documented below. |
| [`web/`](web) | A serverless web app: open an audio file, paint, and save the result as WAV or MP3. All processing happens locally in the browser. See [Web app](#web-app). |

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
| **Middle-drag** | Scrub through time |
| **Alt + left-drag** | Pan time and frequency |
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

## Using it in Reaper

1. Put *Spectrogram Canvas* on the track (or as take FX on the item).
2. Open its window, press **Capture**, and play the section you want to work on. Capture stops
   when you stop playback.
3. Paint and play again, or loop the section and paint while it plays. Changes are heard immediately.
4. To make it permanent, use *Apply track/take FX to items*, or render.

Notes:

- Painting is tied to the project timeline. If you move the item after capturing, re-capture.
- Reaper keeps Ctrl+Z for its own undo unless the plugin window has keyboard focus. Click in the
  plugin window first, or use the **Undo/Redo** buttons. To give the plugin all key presses, enable
  *Send all keyboard input to plug-in* in the FX window's menu.
- Duplicating the track or copying the FX gives the copy its own independent painting and capture.
- REAPER's loop points are shown on the spectrogram and the overview: an amber bar along the top with
  edge lines, drawn dimmer when repeat is off. They update live, even while stopped. Other hosts show
  their loop range while playing, if they report one.
- With *anticipative FX processing* on, the playhead line in the plugin may run slightly ahead of Reaper's.
  This doesn't affect what you hear.
- The plugin adds latency equal to the FFT size, and Reaper compensates for it automatically.

## Using it in Audacity

**As a realtime effect** (recommended — you hear changes live):

1. Click **Effects** on the track's header and add *Spectrogram Canvas*.
2. Open its window, press **Capture**, and play the part of the track you want to work on.
3. Stop playback. The spectrogram appears and capture stops by itself.
4. Paint, then play again to hear the result. Keep painting and listening.
5. To make it permanent, export the audio or use Audacity's command to apply realtime effects to the track.

**As a destructive effect** (Effect menu):

1. Select the audio, open *Effect → Spectrogram Canvas*, press **Capture**, then **Apply**.
   The audio is passed through unchanged while being recorded.
2. Open the effect again (*Effect → Repeat* or the Effect menu). The captured spectrogram is shown.
3. Paint, then **Apply**.

Captured audio is cached in `%APPDATA%\SpectrogramCanvas\captures` so it survives closing the
plugin window. Cached files older than 30 days are deleted automatically.

## Building the plugin (Windows)

Requirements: Visual Studio 2026 or its Build Tools, with the *Desktop development with C++*
workload (includes CMake). JUCE 8 is downloaded automatically.

```powershell
.\vst\scripts\build.ps1       # configure, build Release, run tests
.\vst\scripts\build.ps1 -Install # ...and copy into %LOCALAPPDATA%\Programs\Common\VST3 (no admin needed)
```

Or manually, from a developer prompt:

```
cd vst
cmake --preset vs2026
cmake --build --preset release
ctest --preset release
```

The plugin ends up in `vst/build/SpectrogramCanvas_artefacts/Release/VST3/`. After installing,
rescan in your host (Reaper: *Options → Preferences → Plug-ins → VST → Re-scan*;
Audacity: *Effect → Plugin Manager → Rescan*).

`vst/build/ScSnapshot_artefacts/Release/ScSnapshot.exe out.png [width height] [invert,linear]`
renders the editor with synthetic audio to a PNG, which is handy for checking UI changes.

## Web app

Try it: <https://zfox23.github.io/spectrogram-canvas/>

`web/` is a static site (TypeScript + Vite, no backend). Open an audio file, paint the regions to
keep, listen to the filtered result, and save it as WAV (16/24-bit or 32-bit float) or MP3.
The audio never leaves your machine: decoding, the STFT filter and the encoders all run in the
browser (heavy work happens in a Web Worker).

Differences from the plugin: there is no capture step (the whole file is analysed on load), and the
filter is applied offline with no latency. The painting model, brush, controls and DSP are the same.

**Export PNG** saves a picture of the painted spectrogram, captioned with the file name and date,
with the whole session (painting, settings, view, loop) embedded in it. With **with audio** ticked it
also embeds the audio file, so that one PNG is enough to reopen the session: drop it on the app.
Sites that re-compress images (X, Instagram, most image hosts) strip this data; sending it as a file
(Discord, email, cloud drives) keeps it.

```
cd web
npm install
npm run dev        # local dev server
npm test           # unit tests (FFT, filter, mask, encoders)
npm run build      # static site in web/dist - host it anywhere (Netlify, S3, ...); pushes to `main` deploy it to GitHub Pages automatically
```

## Project layout

```
vst/src/dsp/        STFT mask filter (SpectralMaskProcessor)
vst/src/model/      Painted mask grid, frequency scales
vst/src/capture/    Lock-free capture buffer + WAV cache
vst/src/analysis/   Spectrogram analysis and rendering
vst/src/session/    Shared per-document state (mask + capture) across plugin instances
vst/src/ui/         Spectrogram view, overview, theme
vst/tests/          Unit tests (JUCE UnitTest)
vst/tools/          Editor snapshot tool
web/src/dsp/        FFT, spectrogram analysis, STFT mask filter, Web Worker
web/src/model/      Painted mask grid, frequency scales
web/src/ui/         Spectrogram canvas view
web/src/audio/      Decoding and playback
web/src/encode/     WAV and MP3 encoders
web/tests/          Unit tests (Vitest)
```

## License

AGPL-3.0, see [LICENSE](LICENSE). Built with [JUCE](https://juce.com).
