# PROMPT — Editroom LX (the Linux port of Editroom)

This is the prompt for building and maintaining **Editroom LX**: the Linux version of the Mac video editor in
`../editroom/` (Swift, SwiftUI/AppKit, AVFoundation, Core Image on Metal). Work in `editroomLX/` only.
Never change `../editroom/`: it is the reference. The step-by-step plan is in [IMPLEMENTATION.md](IMPLEMENTATION.md).

Mac baseline for the first port: `editroom@7b7b865` (version 1.0.3, build 4, 2026-11-25).

---

## Goal 1: 1-to-1 parity with the Mac version, as far as Linux and Qt allow

Match the Mac app in:

- **Features.** Everything in `../editroom/README.md` and in the code, including the details only the code
  shows: ripple trim (Q/W), sticky edges, ducking, speed ramps, zoom moves, reversed clips that play straight
  from the original file, HDR/HLG/PQ/Apple Log to Rec.709, captions read from transcripts, linked media
  folders, the export queue, loudness targets, reframed export shapes, Magic (Claude / ChatGPT / Local), and
  the MCP server and its 60-plus tools.
- **UI layout.** Media panel on the left (252 px, resizable 190–440), viewer in the middle, inspector on the
  right (320 px), timeline below (360 px, resizable). The window opens at 1480×940 with a minimum of 1080×680,
  and fills the screen on first launch. Use the same five themes (Auto, Dark, Grey, Amber, Light) with the exact
  palette hex values from `UI/Theme.swift`, and the same 13 px / 11.5 px type scale, pill buttons, hairlines and
  splitters. Match `docs/screenshot.webp` and the mocks in `design/`.
- **Wording.** Copy every string exactly, including menu titles, ellipses ("Open…"), en/em dashes, curly
  quotes, the "−∞ dB" minus sign, alerts, empty states, tooltips, the cursor tags ("placing boat_wide") and
  error messages ("The timeline is empty."). Collect the strings in one table so a sync can diff them.
- **Menus.** The same menus, items, order, separators and enabled/disabled rules (File, Edit, Clip, Mark,
  View). The Mac app menu has no Linux equivalent, so: About Editroom and Check for Updates… go under Help,
  Settings… goes under Edit (Ctrl+,), Quit goes under File (Ctrl+Q). Like the Mac, there is no Close or
  Ctrl+W (one editor window).
- **Shortcuts.** The same `ShortcutAction` list and defaults, mapped ⌘→Ctrl, ⌥→Alt, ⇧→Shift, ⌃→Meta. ⌫ is
  Backspace and ⌦ is Delete. They stay editable in Settings › Shortcuts. Keep the Mac's rules: plain-key
  shortcuts are typed as text inside a text field, Escape always cancels, and a combo given to one action is
  taken from any other. Store them in the same JSON shape as the Mac (`{"split":[{"key":"k","command":true}]}`)
  so the two stay comparable. Top-level menus get no `&` mnemonics, so Alt+F (Freeze Frame) and Alt+R
  (Reverse) work.
- **Defaults.** The same values for every setting and model default: `ColorGrade()`, `Track.duckDB = -12`,
  `TextStyle.size = 110`, transition 0.5 s, autosave on, timecode display, theme Dark, the shortcut defaults,
  the export defaults (H.264, High, sequence size, audio on), MCP port 47920, and so on. Read settings and
  write them under the **same key names** (`appearance`, `autosave`, `timeStyle`, `snapping`/`stickyEdges`/…,
  `magicTab`, `shortcuts`…), using QSettings.
- **Undo.** The Mac's undo works on project snapshots: up to 200 steps, live drags and sliders merge into one
  step (0.8 s auto-commit), `amendLastStep` adds to the step before, and the library and Magic history are
  never undone (except that items a step deleted come back). The menu items are plain "Undo" / "Redo" with
  no action names. Copy this exactly. Don't add named undo the Mac doesn't have.
- **File format.** `.room` files are JSON `SavedDocument {version: 1, project}`, written pretty-printed with
  sorted keys. LX must **read every Mac project and write files the Mac reads**, and round-trip what it
  doesn't use: `bookmark` (macOS bookmark data, base64), `playablePath`, and unknown keys. Encode the way
  Swift's `JSONEncoder` does: UUIDs as upper-case strings, `Data` as base64, a `[UUID: T]` dictionary as a
  flat `[key, value, key, value]` array (`captionFixes`), optionals left out when nil, and doubles in their
  shortest round-trip form. Keep the same caches and sidecars: transcripts, scene tags, media notes, analysis
  and reversed audio, each keyed by path, size and date.
- **Pixel output.** The compositor must give the same pixels as `TimelineCompositor` + Core Image: fit scale,
  orientation, the HDR/Log 64³ cube, transform, wipe, the 32³ grade cube (trilinear, as `CIColorCube`), opacity
  through the alpha vector, premultiplied source-over, captions in output space, and the reframe crop. It is
  unmanaged (Rec.709 values in, Rec.709 values out) and tagged BT.709. Test it against reference frames
  rendered on the Mac (see "Proof" below).

For each **Mac-only technology**, research the closest Linux equivalent and copy its behaviour. Starting
points (IMPLEMENTATION.md §3 has the full table):

| Mac | Linux |
|---|---|
| Metal + Core Image (compositor, colour cubes, scaling) | Vulkan compute through the shared renderer (`src/rendering/GpuRenderer`, `GpuVulkan.cpp`, `shaders/rendering.comp`, `GpuGraph`), with an OpenMP CPU fallback |
| AVFoundation / VideoToolbox decode and encode | FFmpeg libav* with VA-API / NVDEC / Vulkan Video hardware decode and VA-API / NVENC / Vulkan encode, plus a software fallback; DMA-BUF zero-copy into Vulkan |
| AVPlayer, AVSampleBufferAudioRenderer, synchronizer | Our own player: decode threads, the audio clock as master, PipeWire output (through miniaudio, which falls back to PulseAudio or ALSA) |
| AVAudioMix, `.spectral` time-pitch | Our own mixer with the same gain ramps; a phase-vocoder time-stretch (Signalsmith Stretch, MIT) |
| Vision (classify, faces, aesthetics) | ONNX Runtime (CLIP-family zero-shot over Vision's label vocabulary, YuNet faces, a CLIP aesthetic head), on a GPU execution provider with CPU fallback |
| Speech (`SpeechTranscriber`) | whisper.cpp with its Vulkan backend, giving word timestamps |
| Foundation Models (on-device LLM) | llama.cpp with its Vulkan backend: a small GGUF model, JSON-schema / grammar-constrained output, downloaded only on request |
| Accelerate / vDSP | pocketfft / KissFFT on the CPU; VkFFT where it runs on the GPU |
| Sparkle | The same signed `appcast.xml` feed (EdDSA), self-update for the AppImage, and "open release page" for distro and Flatpak installs |
| AppKit / SwiftUI | Qt 6 Widgets with a QSS + QPainter style that matches the Mac exactly, and a custom `QProxyStyle` |
| NSWorkspace / bundle IDs / `.mcpb` | XDG (`.desktop`, MIME type `application/x-editroom`), `claude mcp add`, `~/.codex/config.toml`; Claude Desktop only if it is installed on Linux |
| UserDefaults, Application Support, Caches | QSettings (`~/.config/Editroom`), `$XDG_DATA_HOME/Editroom`, `$XDG_CACHE_HOME/Editroom` |

**Only skip something when Linux really has no equivalent, and say why** in `docs/PARITY.md`, with what
LX does instead. Known cases: macOS bookmark resolution (LX keeps the data and finds files through
`relativePath` and the folder), the Cut and Play settings migration (there was never a Linux Cut and Play),
`MenuCleanup` (Qt adds no system menu items), the SF Pro font (it may not be redistributed; LX ships Inter),
and Apple's aesthetics model (LX uses the nearest open model, labelled as an approximation).

---

## Goal 2, just as important: use the person's hardware as well as it can, starting with the GPU

- **Where the Mac does something on the GPU (Metal, Core Image), LX does it on the GPU too**, through the
  shared Vulkan renderer: `src/rendering/GpuRenderer`, `src/rendering/GpuVulkan.cpp` and
  `shaders/rendering.comp`. Don't leave a ported feature CPU-only when the Mac runs it on the GPU. That covers
  the timeline compositor, YUV→RGB, the HDR/Log conversion, scaling and orientation, transforms and zoom moves,
  wipes and dissolves, grade cubes, opacity, caption and text overlay, reframe, RGB→YUV for the encoder,
  thumbnails and filmstrip frames from HDR files (`ColorFrameGrabber`), and frame scaling (`VTPixelTransferSession`).
  Where the Mac uses the media engine or the Neural Engine (VideoToolbox, Vision, Speech, Foundation Models),
  LX uses the GPU or hardware video blocks too: VA-API / NVDEC / NVENC / Vulkan Video, an ONNX Runtime GPU
  execution provider, and ggml-vulkan.
- **Keep a whole effect on the GPU as one chain of passes (`GpuGraph`)**: upload once, read back once. For
  video, a frame's whole composite (every layer, the grade, the transition, captions, the reframe and the YUV
  pack) is one `GpuGraph` submit. The preview never reads back: the result goes straight to the viewer's
  Vulkan swapchain. Export reads back once, as NV12/P010 for the encoder, or not at all when the encoder can
  take a DMA-BUF. Decoded frames come in as DMA-BUF imports where the driver allows it, and as a single upload
  otherwise.
- **Always keep a CPU fallback (OpenMP) that gives the same result**, for machines without Vulkan and for
  the offscreen tests. The CPU path and the shader share one written-down maths (`src/rendering/ColorMath.h`),
  so they can't drift apart. Add tests that compare the GPU and CPU results **within a stated tolerance**
  (8-bit output: max |Δ| ≤ 1 code value and PSNR ≥ 50 dB unless a test documents something else). The tests
  **skip cleanly** (`QSKIP`, exit code 0, the reason logged) when there is no Vulkan device. CI also runs them
  on lavapipe (Mesa's CPU Vulkan), so the GPU path is always exercised.
- **Use multithreading** (OpenMP, QtConcurrent, a bounded `BlockingWork` pool like the Mac's three slots) for
  CPU work that is still heavy: the audio mix, waveform peaks, loudness, beat detection, reversed audio and
  cache building. Keep the UI responsive: every long job shows progress and can be cancelled (export queue,
  import/convert, transcription, tagging, analysis, model download, Magic), using the Mac's own progress UI
  and wording.
- **Measure before and after** on large media and report the timings. Don't claim a speed-up you didn't
  measure. The standard set is in IMPLEMENTATION.md §9: compositing 3840×2160 and 6016×3384 (6K) frames, a
  6000×4000 photo layer, 4K60 playback with dropped frames counted, scrub latency, a 60 s 4K export, reversing,
  transcription, tagging and loudness. Run each through `editroom-lx --bench` (the same name and output lines
  as the Mac's `--bench`), and record hardware, driver, the backend chosen (Vulkan device / CPU), the median
  of 5 runs, and the spread.
- **In each sync, also look for CPU-only code paths that could move to the GPU**, and list them in that
  sync's document with a priority (P1/P2/P3), the expected gain and what would need measuring, even if they
  aren't done in that release.

---

## Keeping in sync with the Mac

The Mac app moves fast (374 commits so far). LX follows it in **syncs**:

1. Find what changed since the last synced Mac commit (`git -C ../editroom log <last>..HEAD`).
2. Port each change, or list it as deferred or skipped with a reason.
3. Diff the string table, the shortcut defaults, the menus, the `Project` schema and the MCP tool list
   against the Mac's, automatically (`scripts/parity-diff.py`).
4. Re-run the golden-frame, GPU/CPU, format round-trip and benchmark tests.
5. Write `docs/sync/SYNC-<yyyy-mm-dd>-<macsha>.md` with: the Mac range, what was ported, the parity gaps,
   the timings before and after, and **the list of GPU candidates with priorities**.

---

## Proof (definition of done for any feature)

- **Golden frames.** A Mac-side script (`--render-frames <project.room> <times> <outDir>`, to be added to
  the Mac app, or run there from `scripts/mac/`) writes reference PNGs. LX renders the same frames on the GPU
  and on the CPU. Both must be within tolerance of the Mac's (ΔE2000 mean ≤ 1.0 and max ≤ 3.0 on picture
  content; text and captions are measured separately, since the fonts differ).
- **Format round-trip.** Every `.room` in the test corpus opens, saves and re-opens unchanged
  (semantically equal JSON). A file saved by LX opens on the Mac.
- **Behaviour.** Port the Mac's `SelfTest.swift` checks (the timeline geometry, edits, the export settings
  maths, captions layout, speed-ramp maths, Timecode parse/format, Local Magic parsing) as Qt Test cases,
  with the same inputs and the same expected outputs.
- **Hardware.** GPU-vs-CPU tests pass or skip cleanly; benchmark numbers are recorded.
- **Honesty.** Anything not at parity is in `docs/PARITY.md`, with the reason.

## Rules

- C++20, Qt 6.8+ Widgets (the dev box has 6.10.2), CMake + Ninja. No QML (to keep the Mac's pixel-exact,
  QPainter-drawn chrome). Licences: MIT app; LGPL FFmpeg, dynamically linked; no GPL code linked into the
  shipped binary unless the owner agrees (see IMPLEMENTATION.md §11, open questions).
- Port the Mac's maths **literally** (grade recipes, LookMath, SpeedRamp, SourceColor, BS.1770, beat
  tracking, the caption layout, the AutoPlanner scores), with the same constants and the same order of
  operations, so results match. Keep the Mac's comments, translated as needed: they explain why.
- Nothing is sent off the machine unless the person asks (Magic with Claude/ChatGPT, a model download, an
  update check). Local Magic and every analysis run on-device.
