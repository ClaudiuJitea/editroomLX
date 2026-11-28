# IMPLEMENTATION — Editroom LX

The plan for porting `../editroom` (Mac, Swift, ~33.9k lines) to Linux (C++20 / Qt 6 / Vulkan), following
[PROMPT.md](PROMPT.md). Mac baseline: `editroom@7b7b865`, version 1.0.3 (build 4); synced since to `editroom@5f31e48`, version 1.1.2 (build 16).

Contents:
1. Inventory of the Mac app
2. Architecture
3. The Mac → Linux technology mapping
4. Repository layout
5. Rendering: GpuRenderer, GpuGraph, CPU fallback
6. Media, playback and audio
7. Export
8. UI, themes, menus and shortcuts
9. Analysis, Magic and MCP
10. Files, settings and compatibility
11. Tests
12. Benchmarks
13. Packaging and updates
14. Phases and exit criteria
15. The sync process
16. GPU candidates (first list)
17. Risks, open questions and things to decide
18. Setting up the dev machine

---

## 1. Inventory of the Mac app

| Area | Mac files (lines) | What it does | LX module |
|---|---|---|---|
| App shell | `App/EditroomApp.swift` (318), `Shortcuts.swift` (315), `Updates.swift`, `UpdateDriver.swift` (230) | Window, menus, file-open delegate, editable shortcuts, Sparkle | `src/app/` |
| Test hooks | `App/SelfTest.swift` (2177), `FormatCheck.swift` (165) | Headless self-tests, `--bench`, `--format-check`, ~60 `--flags` | `src/app/SelfTest*`, `tests/` |
| Model | `Model/Project.swift` (987), `EditorModel.swift` (2582), `Workspace.swift` (165, the tabs), `ColorGrade.swift` (332), `SpeedRamp.swift` (186), `ZoomMove.swift` (105), `Captions.swift` (279), `MediaFolders.swift` (202), `SourcePreview.swift` (101) | Data model, edits, undo, grade maths, ramp maths, captions layout, linked folders | `src/model/` (pure C++, no Qt GUI) |
| Engine | `Engine/CompositionBuilder.swift` (440), `TimelineCompositor.swift` (269), `SourceColor.swift` (255), `PlaybackEngine.swift` (666), `AudioStream.swift` (355), `AudioScrubber.swift` (163), `Exporter.swift` (449), `Loudness.swift` (162), `Meters.swift` (111), `MediaStore.swift` (737), `Reverser.swift` (307), `ScrubCache.swift` (205), `ScrubStrips.swift` (121), `CaptionRenderer.swift` (124), `ReadGuard.swift` (60) | Timeline → render plan, compositor, HDR/Log, player, audio streaming and scrubbing, export, loudness, meters, media import and caches, reversing | `src/engine/`, `src/rendering/`, `src/media/`, `src/audio/` |
| On-device analysis | `Engine/SceneTags.swift` (400), `Transcripts.swift` (314), `Magic/MediaAnalyzer.swift` (464) | Vision tags, speech transcripts, shot quality, beat tracking | `src/analysis/` |
| Magic | `Magic/*` (20 files, ~6.3k) | Claude/ChatGPT agent runs on a draft, Local planner (LocalBrain, AutoPlanner, LocalImprover…), the swarm/thinking show | `src/magic/` |
| MCP | `MCP/*` (6 files, ~2.4k) | Streamable-HTTP MCP server on 127.0.0.1:47920, stdio bridge (`--mcp`), 60-plus tools, agent connect, downloads, contact sheets | `src/mcp/` |
| UI | `UI/*` (16 files, ~11.3k) | ContentView (and WindowRoot), TabStrip, Library, Preview, Inspector, Timeline canvas/panel, Export sheet/progress, Magic popover, Settings, Theme | `src/ui/` (Workspace, TabStrip, MainWindow…) |
| Build/release | `scripts/*`, `Vendor/ffmpeg`, `appcast.xml`, `.githooks/pre-commit` | ffmpeg build, release, DMG, notarize, Sparkle feed | `scripts/`, `packaging/` |

Things the code shows that the README doesn't, all of which LX must copy:
- **Undo is snapshot-based**, with no action names (`EditorModel.mutate/live/commitLive/amendLastStep`, 200
  steps). The library, linked folders and Magic history are not undone.
- **A tiny bundled `Base.mp4`** is stretched under the whole timeline so there is always video driving
  rendering. LX's own player doesn't need it; keep the behaviour of "black where nothing shows".
- **HDR/Log and reversed clips bypass AVFoundation's tracks** and decode through `ReverseFrames`, in
  0.5 s chunks with a cache of 3 chunks, because AVFoundation's conversion is inaccurate. In LX every clip
  decodes through our own decoder, so the same chunk cache serves all of them.
- **Two composition lanes per track** (`index % 2`), so neighbouring clips can overlap in a transition.
- **Curved fades are approximated with 16 linear pieces, and ducking with 50 ms pieces**: replicate this
  exactly, so the audio matches the Mac sample for sample (within resampler noise).
- **Speed limits for sound**: a clip's sound plays only when its speed is between 1/32 and 32.
- **Tabs (1.1)**: every tab is an `EditorModel` with a slot; only the one in front plays, rebuilds and serves the AI
  apps, the others let go of their player. Slots and the tab in front are kept (`tabSlots`, `activeTab`), and each
  tab's `lastSession`/`recoveryFor` keys and recovery copy carry its slot. Tabs behind reopen when first shown.
  Copied clips are shared by every tab, with their media and track places, so a paste into another project brings
  the media (reusing what's there by file) and keeps V3 on V3.
- **HDR projects (1.1)**: `Project.hdr` (nil is SDR). The composite runs in linear Rec.2020 with SDR white at 1.0;
  the grade works on the HLG signal; the export is HEVC Main 10 tagged BT.2020/HLG whatever codec was picked;
  snapshots for AI apps stay SDR; matching a sequence to a clip never makes it HDR.
- **Grade levels and off (1.0.12)**: a level adds `level/100 × 0.2` of grey to its range's wheel offset; an off grade
  bakes no cube but is kept and saved; changing a grade turns it back on.
- **Scrub frames** are 12 per second, up to 1920 px, stored as HEIC files on disk, with a 6 GB cap evicting
  the files used longest ago. **Hover strips** are 24–120 JPEGs at 480 px.
- **The export loudness target** measures BS.1770 integrated loudness first, then applies
  `gain = min(target − lufs, −1 − peakDB)` to every track.
- **Export bitrate** = w·h·min(fps,60)·bpp, where bpp is 0.2 (High) or 0.085 (Web), ×0.62 for HEVC.
  Keyframe every 2 s, H.264 High AutoLevel, AAC 256 k / 160 k at 48 kHz stereo, WAV 24-bit, BT.709 tags.

---

## 2. Architecture

```
            ┌──────────────────────── Qt 6 Widgets UI (src/ui) ────────────────────────┐
            │ MainWindow · LibraryPanel · ViewerWidget(Vulkan) · Inspector · Timeline… │
            └──────────────▲───────────────────────────────▲───────────────────────────┘
                           │ signals (main thread)         │ present (no readback)
  ┌────────────────────────┴───────┐        ┌──────────────┴──────────────────────────────┐
  │ EditorModel (src/model)        │        │ GpuRenderer (src/rendering)                 │
  │ Project · undo · edits         │──plan─▶│ RenderPlan → GpuGraph (one submit per frame)│
  │ pure C++20, unit-tested        │        │ Vulkan compute ─┬─ CPU fallback (OpenMP)    │
  └──────────────┬─────────────────┘        └────────▲────────┴─────────────────▲─────────┘
                 │                                   │ frames (DMA-BUF / upload) │ NV12/P010 out
  ┌──────────────┴───────────┐   ┌───────────────────┴──────────┐   ┌───────────┴──────────┐
  │ PlaybackEngine           │   │ MediaStore / Decoder pool    │   │ Exporter / ExportJob │
  │ audio clock = master     │◀──│ libav* + VA-API/NVDEC/Vulkan │   │ libav* encoders      │
  │ AudioStream (miniaudio)  │   │ chunk cache, scrub cache     │   │ VAAPI/NVENC/Vk/SW    │
  └──────────────────────────┘   └──────────────────────────────┘   └──────────────────────┘
  Analysis (src/analysis): ORT tags · whisper.cpp · MediaAnalyzer · beats   — background queues
  Magic (src/magic) · MCP server (src/mcp, QTcpServer 127.0.0.1:47920) · Updates
```

**Threads.** UI on the main thread. One render thread owns the Vulkan queue: preview frames are composed
there and presented. A decoder pool (one per open source, hardware where possible). One real-time audio
callback thread (no locks: SPSC ring buffers, as the Mac's `os_unfair_lock`-guarded scrub buffer). A
`BlockingWork` pool with 3 slots for long file reads (the same as the Mac). Background queues for
analysis, one file at a time (the same as the Mac). An export thread per running job, one job at a time
(`ExportQueue`).

**Model / engine split.** `src/model` has no Qt GUI, no Vulkan and no FFmpeg, so it can be unit-tested and
diffed against Swift line by line. Swift `struct`s become C++ value types with `operator==`, and `Codable`
becomes hand-written JSON (see §10). `Set<UUID>` becomes `QSet<QUuid>`, or a sorted `std::vector` where order
matters.

---

## 3. The Mac → Linux technology mapping

| Mac | Used for | Linux (first choice → fallback) | Notes |
|---|---|---|---|
| Core Image on Metal (`CIContext(mtlDevice:)`) | The whole compositor, cubes, scaling, overlays | **Vulkan compute** `GpuRenderer`/`GpuGraph` → **OpenMP CPU** | §5. One graph per frame. |
| `CIColorCube` (32³ grade, 64³ HDR/Log) | Grades, source colour | 3D texture `VK_FORMAT_R32G32B32A32_SFLOAT` (or R16G16B16A16), linear filtering, half-texel remap | Core Image samples the cube trilinearly, with the input clamped to 0…1 |
| `CVPixelBuffer` NV12/P010 in, BGRA out | Zero-copy decoder frames | `VK_EXT_external_memory_dma_buf` + `VK_EXT_image_drm_format_modifier` from VA-API `vaExportSurfaceHandle`; Vulkan Video frames used directly; otherwise one staging upload | Respect the matrix and range attachments (BT.709/2020, video/full) |
| AVFoundation `AVURLAsset`, readers | Probe, decode | **FFmpeg libavformat/libavcodec** (system, dynamically linked), `hw_device_ctx` = VAAPI / CUDA / Vulkan → SW | The Mac's "convert with ffmpeg" step becomes unnecessary: LX reads MXF/MKV/WebM directly |
| VideoToolbox | HW decode/encode, `VTPixelTransferSession` scaling | VA-API / NVDEC / Vulkan Video decode; `h264_vaapi`, `hevc_vaapi`, `*_nvenc`, `*_vulkan` encode; scaling in Vulkan | SW encode fallback: see §17 Q1 (licensing) |
| `AVPlayer` + `AVPlayerItemVideoOutput` | Preview playback, seeks, seamless loop | Our own `PlaybackEngine`: a frame scheduler on the audio clock, coalescing seeks (fast/exact), a spare-decoder seamless loop | Same behaviour as `PlaybackEngine.swift`, not the same objects |
| `AVSampleBufferAudioRenderer` + synchronizer | Per-track audio lanes, gapless edits | **miniaudio** (PipeWire → PulseAudio → ALSA) output; our mixer queues 0.2 s ahead with a 0.25 s lead | The lanes stay, with per-lane volume for instant mute/solo |
| `AVAudioEngine` source node | Instant audio scrubbing | A second miniaudio device/stream reading the in-memory mix at 32 kHz | 80 ms snippets, the same ease in and out |
| `AVAudioMix` ramps, `.spectral` time-pitch | Fades, ducking, gain; speed changes | Our mixer; **Signalsmith Stretch** (MIT) for speed changes | Rubber Band (GPL) avoided; SoundTouch (LGPL) as a backup |
| Resampling inside AVFoundation | Rate conversion | **libswresample** (soxr engine if available) | One resample per lane, as the Mac does |
| `AVAssetWriter` | MP4/WAV export | libavformat mp4 muxer (+faststart), libavcodec encoders, native AAC | The same settings maths, §7 |
| `AVAssetImageGenerator` | Thumbnails, filmstrips | Decoder pool + GPU scale + RGBA8 readback | Frames from HDR files go through the same cube as playback |
| ImageIO HEIC (scrub cache), JPEG (strips) | On-disk frame caches | **Scrub cache: intra-only H.264/HEVC at 12 fps, hardware decoded** (HEIC is HEVC-intra, so this is the closest match); JPEG via libjpeg-turbo when there's no HW codec. Strips: JPEG 480 px | These are internal caches, never shared with the Mac, so the format is free |
| ImageIO stills (HEIC/JPEG/PNG/TIFF/RAW) | Image clips | Qt image plugins + **libheif** (decode) + libraw (optional) | EXIF orientation honoured |
| Vision `VNClassifyImageRequest` | Scene tags | **ONNX Runtime**: CLIP / MobileCLIP / SigLIP image encoder + text embeddings for Vision's label vocabulary (zero-shot), the same thresholds re-fitted | Labels and synonyms come from `SceneTags.swift`. EP order: CUDA/TensorRT, ROCm/MIGraphX, OpenVINO (Intel GPU), then CPU |
| Vision face rectangles | `has_faces`, planner scores | YuNet (OpenCV Zoo, MIT) ONNX | |
| Vision aesthetics score | Planner scores | LAION aesthetic MLP on the CLIP embedding, mapped to −1…1 | An approximation; `isUtility` (screenshot/document) is a CLIP zero-shot |
| Speech `SpeechAnalyzer/SpeechTranscriber` | Word-timed transcripts | **whisper.cpp** with `GGML_VULKAN` (CPU fallback); per-token timestamps → words | Model download on request (base.en / small / turbo); language auto-detect |
| Foundation Models (`LanguageModelSession`, guided generation) | Local Magic's understanding of the request; extra keywords for tags | **llama.cpp** with `GGML_VULKAN`, a small instruct GGUF (3–4B, Q4_K_M), JSON-schema → GBNF constrained output | Opt-in download. Without it, Local Magic uses the rule-based parser, as on Macs without Apple Intelligence |
| Accelerate / vDSP | FFTs (beats), down-mixes | pocketfft (BSD) on the CPU; VkFFT (MIT) as a GPU option | §16 |
| `Network.framework` `NWListener` | MCP HTTP server | `QTcpServer` bound to `QHostAddress::LocalHost`, port 47920, or any free port if taken | The same JSON-RPC, the same "hello/list answered by the bridge" trick |
| `Process` (Claude Code / Codex) | Magic agent runs | `QProcess` with the same argument lists; the CLIs are found on `$PATH`, `~/.local/bin` and `~/.npm-global/bin` | `claude` and `codex` are both on this dev box |
| `.mcpb` Claude extension, bundle IDs | Connect Claude Desktop / ChatGPT | `claude mcp add …`, `~/.codex/config.toml`; Claude Desktop is offered only when it is installed (detected); the ChatGPT desktop app isn't on Linux, so Codex CLI is the ChatGPT route | Wording stays "Claude" / "ChatGPT" |
| MetalFX `MTLFXSpatialScaler` | Export upscaling (1.0.5) | FSR 1 EASU in `Kernel.inc` (GPU and CPU) | Export only; video fitted at more than 1.01×; not in HDR projects (as on the Mac) |
| Core Image `extendedLinearITUR_2020` context, `matchedToWorkingSpace(from:)` | HDR projects (1.1) | `ColorMath.h`: BT.2100 HLG (OOTF gamma 1.2 at 1000 cd/m²), PQ over 203 cd/m², BT.709 and sRGB curves, BT.2087 matrices; a per-layer `transfer` and the frame's `hdr` mode in `Kernel.inc` | GPU ≡ CPU (`tst_gpu` hdr cases); HLG levels as the Mac's self-test |
| EDR (`wantsExtendedDynamicRangeContent`) | Showing HLG in the viewer | Shown as SDR for now (`ER_HDR_VIEW`: SDR white at white, highlights clipped) | An HDR swapchain needs `VK_EXT_swapchain_colorspace` and Wayland colour management (§17) |
| `SpeechTranscriber.supportedLocales` | Speech language (1.0.12) | whisper.cpp's language list; Automatic = whisper's own detection | |
| Sparkle (EdDSA appcast) | Updates | `Updates` reads the same `appcast.xml` with a `sparkle:os="linux"` enclosure; EdDSA verified with libsodium; the AppImage replaces itself; Flatpak and distro packages get "Open release page" | The alert wording and layout match `UpdateDriver` |
| `NSOpenPanel/NSSavePanel` | Open/import/save/export | `QFileDialog` (uses xdg-desktop-portal under Flatpak) | |
| `NSAlert` | Alerts | `EditroomAlert` (styled `QDialog`), the same text and button order | Mac order is [Cancel][Default] on the right: keep it |
| Dock/Finder open, drag in | Opening files | `.desktop` + MIME `application/x-editroom` (`*.room`, `*.cutplay`), single instance via `QLocalServer` forwarding argv | |
| `UserDefaults` / Application Support / Caches | Settings, recovery, caches | `QSettings` (INI, `~/.config/Editroom/Editroom.conf`), `$XDG_DATA_HOME/Editroom`, `$XDG_CACHE_HOME/Editroom` | Same key names |
| System appearance (Auto theme) | Theme | `QGuiApplication::styleHints()->colorScheme()` (portal `org.freedesktop.appearance`) | |
| SF Pro (system font), `NSFontManager` | UI and text clips | **Inter** (OFL, bundled) for the UI and for "system font" text; fontconfig for named families | SF Pro may not be redistributed. A Mac project naming a font that isn't installed falls back through fontconfig, and a warning lists the missing fonts |
| `NSAttributedString` drawing (text, captions) | Rasterising titles and captions | `QTextLayout` / `QPainter` into a premultiplied RGBA8 image, cached, then uploaded once as a GPU layer | Text pixels can't match exactly (font rasteriser); geometry, size and placement must |

---

## 4. Repository layout

```
editroomLX/
  CMakeLists.txt  CMakePresets.json  PROMPT.md  IMPLEMENTATION.md  README.md  LICENSE
  cmake/            FindFFmpeg.cmake, Shaders.cmake (glslc → SPIR-V → embedded via qrc)
  shaders/          rendering.comp (main compositor kernels), yuv.comp, cube.comp, text.comp, analysis.comp
  src/
    app/            main.cpp, Application, MainWindow, Menus, Shortcuts, SelfTest, FormatCheck, Bench, Updates
    model/          Project, Clip, Track, ColorGrade(+LookMath, CubeLUT, GradeBaker), SpeedRamp, ZoomMove,
                    Captions, MediaFolders, Timecode, EditorModel, Undo, Json (Swift-compatible codec)
    rendering/      GpuRenderer.{h,cpp}, GpuVulkan.cpp, GpuGraph.{h,cpp}, CpuRenderer.cpp (OpenMP),
                    ColorMath.h (shared CPU/GLSL maths), SourceColor, RenderPlan (≈ TimelineInstruction),
                    TextRenderer, CaptionRenderer, Interop (DMA-BUF), Present (swapchain)
    media/          MediaStore, Decoder (libav + hwaccel), FramePool, ChunkCache (≈ ReverseFrames),
                    ScrubCache, ScrubStrips, Thumbnails, Stills (libheif), Probe
    audio/          Mixer, AudioStream (lanes), AudioScrubber, TimeStretch, ReverseAudio, Waveforms,
                    Loudness (BS.1770), Meters, Device (miniaudio)
    engine/         CompositionBuilder (→ RenderPlan + AudioPlan), PlaybackEngine, Exporter, ExportQueue
    analysis/       SceneTags (ORT), Transcripts (whisper.cpp), MediaAnalyzer, Beats, Models (download/verify)
    magic/          MagicAgent, MCPDraft, Local* (Brain, Add, Captions, Combined, Improver, Pointed, Ramp,
                    Speech, Title), AutoPlanner, MagicPlan, MagicShow, MagicThinking, AgentPresence, Mentions
    mcp/            MCPServer, MCPBridge (--mcp), MCPTools, AgentConnect, MediaDownload, ContactSheet
    ui/             Theme, Palette, Style (QProxyStyle), widgets/ (PillButton, Hairline, Splitter, ToggleRow…),
                    LibraryView, PreviewView, ViewerWidget, InspectorView, TimelinePanel, TimelineCanvas,
                    TimelineLayout, ExportSheet, ExportProgress, MagicPopover, MentionField, SettingsView,
                    EditroomAlert
  resources/        fonts/Inter*, icons/, editroomlx.desktop, editroom.xml (MIME), strings.json (all UI text)
  third_party/      miniaudio, signalsmith-stretch, pocketfft, (VkFFT), nlohmann/json (or QJson*)
  tests/            unit/, golden/ (Mac reference PNGs + projects), gpu/, format/, selftest/, bench/
  scripts/          parity-diff.py, make-test-clips.sh, mac/render-frames.swift, release.sh, appimage.sh
  packaging/        AppImage, Flatpak manifest (org.editroom.Editroom), .deb control
  docs/             PARITY.md, ARCHITECTURE.md, sync/SYNC-*.md, bench/*.md
```

---

## 5. Rendering: GpuRenderer, GpuGraph, CPU fallback

### 5.1 What it has to reproduce (from `TimelineCompositor.compose`)

For output time `t` (with +1e-4, as on the Mac), on a canvas of `instruction.canvas`, starting from opaque black:
1. For each layer, bottom first, where `start ≤ t < end`: `opacity = layer.opacity(t)`. Skip if ≤ 0.001.
2. Source:
   - **video**: the decoded frame → orientation (EXIF/rotation matrix) → **fit** scale
     `s = min(W/w, H/h)`, centred → `SourceColor.toRec709` (64³ cube), applied **after** scaling, as the
     Mac does;
   - **image**: fitted at build time;
   - **solid**: a colour fill of the frame;
   - **text**: a pre-rasterised image placed at `(x·W, (1−y)·H)` (in Core Image the y axis runs up).
3. Wipe: crop to `[0, W·w(t)] × [0, H]`, with `w = smoothstep(progress)`.
4. Transform (`ClipTransform.at(u)` with the zoom move): scale about the centre, then offset
   `(x·W, −y·H)` in y-up space.
5. Grade: 32³ cube (`GradeBaker`), if not neutral.
6. Opacity: `CIColorMatrix` with the alpha vector `(0,0,0,o)` scales alpha by `o`. Whether Core Image
   un-premultiplies around the filter or not, the result is premultiplied RGBA × `o`, which is what LX
   computes. Confirm this with a golden frame at o = 0.5 on a semi-transparent text layer.
7. `composited(over:)`: premultiplied source-over.
8. Reframe: translate by `−offset` (crop window), crop to the output.
9. Captions: drawn in output space, over everything.
10. Output tagged BT.709 (primaries, transfer and matrix), unmanaged (no colour-space conversion anywhere).

**HDR projects (1.1).** The same steps, with each layer brought into linear Rec.2020 after its source colour (HDR
clips as HLG or PQ; SDR video and Log after its cube through BT.709's curve; titles, solids, photos and captions as
sRGB), the grade done on the HLG signal (linear → HLG, cube, back), opacity and source-over in linear light, and the
result written as HLG (the export, P010 with BT.2020's matrix) or as Rec.709 for a screen without HDR (the viewer,
thumbnails, AI snapshots). `RenderPlan.hdr` carries the mode; `BuildOptions.hdrOutput` picks HLG.

**Order and coordinate conventions matter.** Core Image is y-up. LX works y-down internally and converts
every y term (text `1−y`, transform `−y`, reframe `top`) in `RenderPlan`, with unit tests pinning each one.

### 5.2 GpuGraph

- `RenderPlan` (built on the main thread from `Project`, like `CompositionBuilder.build`) is immutable and
  hashed (the Mac's `signature`) so unchanged edits don't rebuild anything.
- `GpuGraph` is a tiny DAG of compute passes with transient images allocated from a pool (aliasing by
  lifetime). Each frame records **one command buffer**:
  `import/upload sources → per-layer [yuv2rgb+orient+fit+sourceCube] → [wipe/transform resample] →
  [gradeCube] → [opacity+over into accumulator] → [reframe] → [captions over] → (present | rgb2yuv pack → readback | DMA-BUF export)`.
  Adjacent per-layer steps are **fused into one kernel** (`rendering.comp` with specialisation constants per
  layer kind), so a typical layer is one dispatch, reading the source once and writing into the accumulator.
- Resampling: bilinear on the fit and transform, the same as Core Image's default `transformed(by:)`. For
  downscales larger than 2× the Mac result is aliased, but for parity LX does the same. A `quality` flag
  (export only, off by default) can enable a mip/area prefilter **only** if Mac golden frames show Core Image
  doing so. Check this with a measurement before deciding.
- Working format: RGBA16F accumulator (Core Image's default intermediate precision on Apple GPUs), rounding
  to 8 bits only at the end. The CPU path uses float32 and the same rounding (`round-half-even` vs `+0.5`:
  pin it with a test).
- The cubes are 3D textures with linear filtering, sampled at `(c·(N−1)+0.5)/N` to reproduce CIColorCube's
  texel-centre mapping. They are cached per grade (`GradeBaker` hash), for at most 200 entries, as on the Mac.
- Text and captions are rasterised on the CPU (QPainter) only when their style or size changes, uploaded
  once, and cached (64 entries, as on the Mac).
- The preview presents the accumulator straight into the viewer's swapchain (`QVulkanInstance` +
  `QWindow::createWindowContainer`, or a `QRhiWidget`-hosted Vulkan image). There is **no CPU readback in
  playback or scrubbing**.
- Export: one readback per frame as NV12 (8-bit) or P010, into a persistently mapped host-visible ring of 3
  buffers, so the GPU renders frame N+1 while the encoder takes frame N. With VA-API encode, export
  DMA-BUF images to VA surfaces instead (no readback).

### 5.3 CPU fallback (OpenMP)

`CpuRenderer` runs the same `RenderPlan` in float32, `#pragma omp parallel for` over rows, using the same
`ColorMath.h` functions as the GLSL (`#include`d into both: written in a C/GLSL common subset with macros
for `vec3`/`float3`). It is chosen when `vkCreateInstance` fails, when no device has compute + transfer,
when `EDITROOM_GPU=cpu`, or for `--selftest` offscreen without a device.

### 5.4 Device selection and robustness

Prefer a discrete GPU, then integrated, then lavapipe (but only in tests: lavapipe is never chosen for
interactive use unless forced, since it is slower than the CPU path). Log the device, driver and API
version. A lost device rebuilds the context and retries once, then falls back to the CPU for the session,
with a one-line message in the log (no dialog: the Mac never shows GPU errors).

### 5.5 GPU / CPU equivalence tests

`tests/gpu/`: for each kernel and for whole frames (every layer kind × every transition × grade on/off ×
HDR kinds × transforms × reframe), render on the GPU and on the CPU and assert max |Δ| ≤ 1/255 and PSNR
≥ 50 dB on the 8-bit output. A test documents any looser tolerance with its reason (for example fp16
accumulation in the cube interpolation). Skip with `QSKIP("No Vulkan device")` when `GpuRenderer::available()`
is false. CI runs them twice: on the real GPU when present, and on lavapipe (`VK_ICD_FILENAMES=…/lvp_icd.json`).

---

## 6. Media, playback and audio

### 6.1 Import and probe (`MediaStore`)
- Probe with libavformat: duration, fps (r_frame_rate/avg), size **after rotation** (the display matrix),
  audio presence and rate, colour (`color_trc`: `arib-std-b67` → HLG, `smpte2084` → PQ; Apple Log, see
  §17 Q4).
- `MediaAsset` fields as on the Mac. `playablePath` is never written for new imports (no conversion needed),
  and is preserved from Mac projects (used only if the file exists on this machine).
- "File changed on disk" detection by size + mtime, as on the Mac, with offline (red) only after loading
  has finished (`d8664cc`).
- Linked folders (`MediaFolders.swift`): the same rules (no hidden files, nothing inside packages, name
  order). A `QFileSystemWatcher` replaces macOS's rescans-on-focus; rescan on window activation too, for parity.

### 6.2 Decoding
- One `Decoder` per (asset, size limit). Hardware decode via `av_hwdevice_ctx_create` (VAAPI → CUDA →
  Vulkan), with the frames mapped to DMA-BUF (`av_hwframe_map` to DRM PRIME) and imported into Vulkan. When
  that isn't possible, `av_hwframe_transfer_data` gives one upload.
- `ChunkCache` ports `ReverseFrames`: 0.5 s chunks (fewer frames per chunk when big), the three most
  recently used kept, read-ahead in the playing direction, idle release after ~2 s, size limit = the long
  side of the canvas. Used for **every** clip (forwards, reversed, HDR), since there is no AVComposition.
- Fast vs exact seeks, as in `PlaybackEngine.seek(fast:)`: fast goes to the nearest decodable frame;
  exact decodes from the keyframe.

### 6.3 Playback (`PlaybackEngine`)
- The **audio clock is master** (as on the Mac: "the sound leads"). The video scheduler picks the frame
  for `audioClock + outputLatency` and composes or presents it on vsync.
- The same behaviours: rebuilding while playing swaps the plan at the playhead with no pause; coalesced
  seeks; sync checks every 0.5 s with a one-frame threshold; a **seamless loop** using a spare decoder primed
  at the loop start; skimming; the full-screen viewer (F).
- The scrub path draws from `ScrubCache` frames on the GPU while dragging, then the exact frame when the
  drag rests (the same as the Mac).

### 6.4 Audio
- `AudioPlan` from `CompositionBuilder`: per track, clips with source ranges, speed/ramp pieces (the same
  `insertRamped` cut points: `max(4, min(90, ceil(span·30)))` steps per ramp), gains, fades (16-piece
  curves) and ducking (50 ms pieces, using `AudioLevels.activity/duckGain`).
- The mixer reads each lane at its dominant source rate, time-stretches pieces whose speed ≠ 1 (Signalsmith
  Stretch), applies ramps sample-accurately on the same piecewise-linear breakpoints, and resamples once per
  lane to the device rate.
- `AudioStream` lanes: queued 0.2 s ahead, new reads start 0.25 s early with the head discarded (settling),
  per-lane volume for mute/solo (heard at once), lanes removed only when stopped. These all follow the
  Mac's reasons.
- `AudioScrubber`: the whole mix rendered to memory at 32 kHz int16 stereo (≤ 45 min), with 80 ms snippets
  played from a real-time callback.
- `ReverseAudio`: the same cache name hash (FNV-1a over path, size and date), 16-bit stereo WAV at the
  file's own rate, built backwards from a scratch file.
- Waveforms, meters and loudness: the same peak/RMS stretch sizes, the same meter ballistics (release 0.9,
  0.5 s hold, 0.45 fall, −60 floor), and BS.1770 ported verbatim (K-weighting biquads at 48 kHz, 400 ms blocks
  with 75 % overlap, −70 absolute gate, −10 relative gate).

---

## 7. Export

- `ExportSettings` ported verbatim (Format Video/Audio, Quality High/Web, Codec H.264/HEVC, shapes
  Sequence/Vertical 9:16/Square 1:1/Portrait 4:5, `renderSize`, `crops`, `videoBitrate`, `rangeOnly`).
- Encoder choice, in order, per codec: VA-API → NVENC → Vulkan encode → software (§17 Q1). Rate control:
  average bitrate = Mac formula, GOP = 2·fps, H.264 High (auto level), HEVC Main / Main10 (8-bit sources →
  Main). AAC (native FFmpeg encoder, 256 k / 160 k, 48 kHz stereo); WAV PCM s24le 48 kHz stereo. MP4 with
  `+faststart`, BT.709 tags (`color_primaries/trc/colorspace = bt709`, range tv).
- Loudness target: measure, then `gain = min(target − lufs, −1 − peak)` added to every track's gain, as on
  the Mac. Waits for reversed audio first, as on the Mac.
- HDR projects: HEVC Main 10 from P010 (hevc_vaapi → hevc_nvenc → libx265), tagged `bt2020`/`arib-std-b67`/`bt2020nc`,
  limited range; packed on the GPU (`pack.comp`'s 10-bit mode) and read back once, 3 bytes a pixel.
- `ExportQueue`: one at a time in the background, each a snapshot of the project, ETA once enough progress
  has been made, finished items tidying themselves away, cancel. The same pill UI in the title strip.
- Pixel parity for export is checked on the **pre-encode** frames (bit-for-bit against the CPU path,
  within tolerance against the Mac goldens), and post-encode with PSNR ≥ 40 dB against the pre-encode frames.

---

## 8. UI, themes, menus and shortcuts

### 8.1 Window and chrome
- A `QMainWindow` with **native decorations**, plus a 38 px "title strip" (the palette's `panel`
  colour) carrying the project tabs (`TabStrip`: each as wide as its name, 84–190 px, the one in front in a pill
  with ×, a dot for unsaved changes, + for a new tab, "N more tabs ⌄" for those that don't fit), the export
  progress pill, the Magic button and the Export pill. The panels under it are built for the tab in front
  (`MainWindow::buildEditor`), as the Mac rebuilds `ContentView` per tab. This copies the Mac's unified toolbar. Client-side decorations (`Qt::FramelessWindowHint`
  with our own traffic-light-free controls) are optional later, behind a setting, because of Wayland and
  tiling-WM differences.
- Layout as in `ContentView`: Library (252, 190–440) | splitter | Preview | hairline | Inspector (320),
  then the horizontal splitter, then the Timeline (360; clamped to 170 … height−240). A hairline under the
  title strip. "Drop to import" overlay (accent at 6 %, 2 px rounded stroke, pill label).
- Theme: `Palette` copied hex for hex. `Theme::NS::…` becomes `Theme::color(Role)`. Every widget paints with
  QPainter or QSS from the palette; a theme change rebuilds the views (`.id(themes.look)` → recreate).
- The Mac's SwiftUI views become custom `QWidget`s. `TimelineCanvas` (2201 lines, AppKit-drawn) ports almost
  1:1 to a `QWidget::paintEvent`, with the same geometry constants from `TimelineLayout.swift`; filmstrips
  and waveforms are drawn from the same caches.
- Viewer: a Vulkan surface embedded in the Preview panel, with transport controls, timecode (monospaced
  digits), Fit menu, loop and full screen, as in the screenshot.

### 8.2 Menus (exact titles and order)
```
File:  New  New Tab  Open…  Open Recent ▸ (… / Clear Menu) | Close Tab  Save  Save As… | Import Media…  Export… | Quit Editroom
Edit:  Undo  Redo | (Cut Copy Paste Select All for text fields) | Settings…
Clip:  Split at Playhead  Add Dissolve  Even Out Audio Levels  Detach Audio  Duplicate  Freeze Frame  Reverse |
       Copy Grade  Paste Grade | Match Sequence to Clip | Delete  Ripple Delete  Ripple Trim Start to Playhead
       Ripple Trim End to Playhead  Close All Gaps | Add Text  Add Solid  <captionsMenuTitle> |
       Add Video Track  Add Audio Track
Mark:  Add Marker  Clear All Markers | Set In  Set Out  Clear In and Out
View:  Zoom In  Zoom Out  Zoom to Fit | ☐Filmstrips ☐Snapping ☐Sticky Edges ☐Skimming ☐Audio Scrubbing | Enter Full Screen
Window: Show Previous Tab  Show Next Tab
Help:  About Editroom  Check for Updates…
```
The enabled rules come from the same model properties (`canAddDissolve`, `selectionIsLinked`…). `--dump-menu`
prints the same `MENU` lines as the Mac, so `parity-diff.py` can compare the two.

### 8.3 Shortcuts
- `ShortcutAction`, `KeyCombo` and `ShortcutStore` ported verbatim. `KeyCombo(event:)` maps Qt keys to the
  same names (`space`, `delete`=Backspace, `forwardDelete`=Delete, arrows, `home`, `end`, `return`, `tab`,
  `pageUp`, `pageDown`). Display uses the Linux style ("Ctrl+Shift+S", "Alt+F", "Backspace") per the
  platform; the stored form is the Mac's (`command` = Ctrl).
- Plain-key and arrow shortcuts are handled by the editor window (`KeyCommands`), never as menu
  shortcuts (Backspace in a dialog must not delete clips), exactly as the Mac does.
- Conflicts: Alt+drag (duplicate while dragging, `TimelineCanvas:694`) is taken by many window managers
  (GNOME/KDE move windows with Alt+drag, or Super+drag on newer defaults). Implement Alt+drag for parity,
  and document the WM setting in `PARITY.md`. Ctrl+drag is **not** added (that would diverge from the Mac).

### 8.4 Settings
The same tabs and sections (General: Theme / View / Saving / Transcription (Speech language), plus LX's "On this
computer" model downloads; Shortcuts with groups File / Edit / Playback /
Tools / Clip / Mark / View, record / remove / restore, "Restore Defaults"; Magic: Enable Magic, Show Magic at
work, AI apps, Editing from other apps; Updates: Software Update, Release Notes). The same keys
(`settingsTab`, …).

---

## 9. Analysis, Magic and MCP

### 9.1 Scene tags (`SceneTags`)
- A frame every ~2 s (the same sampling), a CLIP-family image embedding on the GPU (ORT), a cosine
  similarity against pre-computed text embeddings of **Vision's label vocabulary plus the Mac's everyday
  synonyms** (`SceneTags.swift` maps "portal" to "door", etc.). Keep tags with a calibrated score ≥ the
  Mac's threshold (re-fit on a labelled set so the precision is comparable). Store them in the same
  structure (`confidence`, `times`, keywords).
- The cache is named the same way (path, size, date, version). Write to `$XDG_CACHE_HOME/Editroom/Tags`.
  The tags algorithm version is suffixed `-lx` so Mac caches are never mixed in.
- Extra keywords (the Mac uses Foundation Models on macOS 27): a llama.cpp vision model only if one is
  installed. Otherwise none, as on Macs without Apple Intelligence.

### 9.2 Transcripts
whisper.cpp (Vulkan), word timestamps (`token_timestamps` + DTW), output in the Mac's `Transcript`
structure (words with file times). The same queue (`soon` first, `limit` wait), the same storage keyed by
file, and the same "changed file → re-transcribe" rule.

### 9.3 MediaAnalyzer and beats
Port the maths verbatim: brightness, edge variance, change between frames and colourfulness on small frames,
and the beat tracker (spectral-flux onsets, autocorrelation tempo favouring common tempos, grid fit, nudging,
downbeat from bass energy). The frame metrics move to a GPU reduction pass when the frames are already on
the GPU (§16). The beat tracker's FFTs use pocketfft, with VkFFT as a P3 candidate.

### 9.4 Magic
- **Claude / ChatGPT**: `MagicAgent` spawns `claude` / `codex` with the same arguments (model list from
  the Mac: `claude-opus-5-5`, `claude-sonnet-5-5`, `claude-haiku-5-5`, `claude-fable-5-2`; Codex models from
  `~/.codex/models_cache.json`; effort flags), pointed at `MCPServer` with a `?draft=` token. Edits go to
  `MCPDraft`; the swarm plays them as one undo step. Login flows: `claude auth login --claudeai`, `codex login`.
- **Local**: LocalBrain (llama.cpp when a model is installed; the rule-based parser otherwise), and every
  Local* planner ported verbatim (they are pure logic).
- **The show**: `MagicThinking` (dim, Looking → Analyzing → Thinking, corner brackets, sweep lines, the dot
  field over the viewer, dashed ghost clips) and `MagicShow` (the cursor swarm) are drawn on a transparent
  overlay widget over the main window, using the Mac's anchor keys (`MagicAnchor`). The dot field is a small
  compute/fragment pass in the viewer (GPU), drawn under the overlay.

### 9.5 MCP
- `MCPServer`: QTcpServer on 127.0.0.1:47920 (any free port if taken), Streamable HTTP with plain JSON
  replies, the same JSON-RPC methods and the same tool schemas (generated from one table, diffed against the
  Mac's with `parity-diff.py`).
- `MCPBridge` (`editroom-lx --mcp`): stdin/stdout lines; answers `initialize` / `tools/list` / `ping` itself
  while the app is closed; launches the app (`QProcess::startDetached`) on the first real call and reopens
  it if it quit; forwards the agent's hello so the cursor gets its name.
- `AgentConnect`: Claude Code (`claude mcp add editroom -- editroom-lx --mcp`, with the same scope), Codex
  (`[mcp_servers.editroom]` in `~/.codex/config.toml`), Claude Desktop only if it is found on Linux.

---

## 10. Files, settings and compatibility

- `src/model/Json.cpp` is a **Swift-`JSONEncoder`-compatible codec**:
  - Keys sorted, pretty-printed with 2-space indentation and `" : "` separators (Swift's style). Pin this with
    a byte-compare test against a Mac-saved file.
  - `UUID` as upper-case strings; `Data` as base64; `Double` formatted with the shortest round-trip
    representation (Swift prints `0.1` not `0.10000000000000001`; use `std::to_chars`); integers without `.0`.
  - Optionals omitted when nil (synthesised Codable uses `encodeIfPresent`).
  - `[UUID: T]` (`captionFixes`) encoded as an **unkeyed array of alternating key/value**, which is what
    Swift does for non-String/Int keys.
  - Enums by raw value (`"dipToBlack"`, `"in"`, `"out"`, `"inOut"`…).
  - Unknown keys and fields LX doesn't use (`bookmark`, `playablePath`) are kept and written back.
  - The defaults on decode are exactly the Mac's `decodeIfPresent ?? default` ones (Track, Clip…).
- Paths: Mac projects contain `/Users/...` absolute paths. Opening runs `resolveMovedMedia` (`relativePath`
  first), then the linked-folder rules, then offers "Locate…", as on the Mac. Saving writes the Linux
  absolute path and a fresh `relativePath`, so a project folder carried between the Mac and Linux keeps working.
- Recovery: `$XDG_DATA_HOME/Editroom/Recovery/Untitled.room` (the first tab; `Untitled <slot>.room` for the
  others, `Untitled (test).room` for test launches), with 1 s idle autosave, `lastSession` and `recoveryFor`
  (suffixed `.<slot>` for tabs after the first). The same "Save, Don't Save, Cancel" on quit when autosave is off.
- Sidecars (MediaNotes, analysis, transcripts, tags, reversed audio, scrub caches) keep their Mac
  names and layouts where they are data; internal caches may differ (scrub cache format, §3).

---

## 11. Tests

| Suite | What | How |
|---|---|---|
| `tests/unit` | Model maths: ColorGrade/LookMath/CubeLUT/GradeBaker, SpeedRamp (span/source/solve), ZoomMove, Timecode parse/format, Project edits (split, ripple, retime, sticky `carried`, `pullLeft`, `closeAllGaps`, transitions spans/plans, `cropped`), captions layout, BS.1770 against reference vectors (EBU Tech 3341 files) | Qt Test; inputs and expected outputs copied from Swift. Where Swift has no test, run the Swift code once on the Mac and save its outputs as fixtures (`scripts/mac/dump-fixtures.swift`) |
| `tests/selftest` | The ported `SelfTest.swift` checks (timeline geometry `check("initial")…`, multi-track edit + MP4/WAV export, `--import-test`, `--captions-test`, `--loop-test`, `--scrub-test`…) | `editroom-lx --selftest <media> <out>` (the same flags), `QT_QPA_PLATFORM=offscreen` |
| `tests/golden` | Mac reference frames vs LX (GPU and CPU) | ΔE2000 thresholds (PROMPT.md "Proof"); test clips from `scripts/make-test-clips.sh` (ffmpeg port of `make-test-clip.swift`: sdr_/hlg_/pq_/applelog_ patches) + `FormatCheck` patch-colour checks |
| `tests/gpu` | GPU vs CPU equivalence per kernel and per whole frame | Stated tolerance; `QSKIP` with no device; run on lavapipe in CI |
| `tests/format` | `.room` round-trip, Mac-written files byte-compare after re-save, LX-written files decoded by the Mac (`scripts/mac/check-open.swift` in the sync) | Corpus in `tests/format/corpus/` |
| `tests/audio` | Mixer vs Mac-rendered WAV of the same project (fades, ducking, ramps, reverse) | Null test: residual ≤ −60 dBFS outside stretched pieces; stretched pieces compared by loudness and onset times |
| `tests/mcp` | Tool list/schema equality with the Mac; scripted JSON-RPC sessions | Diff via `parity-diff.py` |
| `tests/ui` | Screenshots of key states per theme vs `docs/screenshot.webp` and the `design/` mocks | Perceptual diff, used as a review aid; not gating (fonts differ) |

---

## 12. Benchmarks (`editroom-lx --bench <dir>`)

Measure first (before optimising) and after. Report the median of 5 runs ± the spread, with the hardware,
driver, Vulkan device, backend and build type, in `docs/bench/<date>-<host>.md` and in the sync doc.
The dev box is an Intel Alder Lake-UP3 with Iris Xe (Mesa `intel_icd`) and 12 threads; also run on one
NVIDIA or AMD machine before claiming general numbers.

| Bench | Input | Metric |
|---|---|---|
| B1 compose 4K | 3840×2160 HEVC 10-bit, 3 layers, grade, dissolve, caption | ms/frame GPU vs CPU (excluding decode); and end to end including decode |
| B2 compose 6K | 6016×3384 source → 4K canvas | ms/frame |
| B3 still layer | **6000×4000** JPEG/HEIC photo with transform + zoom move + grade | first frame (decode+upload) and per-frame ms |
| B4 playback | 4K60 H.264 and HEVC, 60 s, 2 tracks | dropped frames, CPU %, GPU % |
| B5 scrub | Drag across 2 min of 4K long-GOP | p50 / p95 latency from mouse to presented frame |
| B6 export | 60 s 4K timeline → H.264 High | wall time (× realtime), per encoder (VA-API / NVENC / SW) |
| B7 reverse | `--reverse-bench`: 4K clip, picture at 1920 and 3840, sound | the same lines as the Mac's `RBENCH` |
| B8 transcribe | 10 min speech | × realtime, Vulkan vs CPU |
| B9 tags | 100 clips | s/clip, GPU EP vs CPU |
| B10 loudness, waveforms, beats | 60 min mix / 5 min song | seconds, threads 1 vs N |

The output lines start with `BENCH` and look like the Mac's (`log(...)` format), so Mac and Linux numbers
sit side by side in a sync doc. No claim of "faster" goes in any doc without a row in this table.

---

## 13. Packaging and updates

- **AppImage** (primary, self-updating): bundles Qt, miniaudio, ORT (CPU EP + the GPU EPs that are
  allowed to ship), whisper.cpp/llama.cpp with Vulkan; uses the **system** FFmpeg/VA-API drivers and the Vulkan loader.
- **Flatpak** (`org.editroom.Editroom`, KDE runtime 6.x + the ffmpeg-full extension): updates via Flatpak; the
  in-app check only tells.
- **.deb** for Ubuntu 26.04: depends on libavcodec61+/libva/vulkan loader.
- `scripts/release.sh` (build, test, package, sign the AppImage with EdDSA) and `scripts/publish.sh` (GitHub
  Release + add a Linux `<enclosure sparkle:os="linux">` to the shared `appcast.xml`, or to `appcast-linux.xml`
  if the owner prefers to keep the Mac feed untouched; §17 Q6).
- About panel: "Uses FFmpeg, licensed under the LGPL 2.1", plus the licences of whisper.cpp, llama.cpp,
  ORT, Inter, miniaudio, Signalsmith Stretch and any models.

---

## 14. Phases and exit criteria

Each phase ends with a green test run, a benchmark table (from P2 on) and a short entry in `docs/sync/`.

| Phase | Scope | Exit criteria |
|---|---|---|
| **P0 Foundation** (wk 1–2) | CMake/Ninja/presets, CI (GCC + Clang, offscreen, lavapipe), deps (§18), `src/model` port + Swift-compatible JSON, Timecode, `parity-diff.py` (strings, shortcuts, menus, schema, MCP tools) | All model unit tests pass; Mac `.room` corpus round-trips; parity-diff runs |
| **P1 Render core** (wk 2–5) | `GpuRenderer`/`GpuVulkan`/`GpuGraph`, `rendering.comp`, CpuRenderer, ColorMath, SourceColor, GradeBaker, RenderPlan (CompositionBuilder video side), text/caption rasterising, Mac golden-frame script | GPU≡CPU tests pass/skip; golden frames within tolerance for SDR/HLG/PQ/Log, grades, looks, transitions, transforms, reframe; B1–B3 measured |
| **P2 Media + playback** (wk 4–8) | Decoder (HW + DMA-BUF), ChunkCache, MediaStore, stills, thumbnails, scrub cache/strips, audio mixer, lanes, scrubber, time-stretch, reverse audio, PlaybackEngine, viewer swapchain | 4K60 playback without drops on the dev box (or a documented limit); scrub p95 measured; audio null tests pass; B4, B5, B7 recorded |
| **P3 Editor UI** (wk 6–12) | Theme/palette/style, MainWindow + title strip, Library (incl. folders, search, hover-scrub), Inspector (all sections), TimelinePanel + TimelineCanvas (all tools: select/cut, trim, ripple, sticky, snapping, markers, in/out, fades handles, ramps graph, zoom graph, track headers M/S/lock/eye/gain), menus, shortcuts + Settings, alerts, drag and drop, recovery/autosave | Ported SelfTest timeline geometry and edit checks pass; `--dump-menu` matches the Mac; screenshots reviewed against the Mac for all 5 themes |
| **P4 Export** (wk 10–12) | ExportSettings, encoders (HW → SW), loudness target, queue + pill, sheet with shapes/reframe preview | Pre-encode frames ≡ golden; files play in mpv/ffmpeg with BT.709 tags; LUFS within ±0.1 of the Mac for the same project; B6, B10 recorded |
| **P5 Analysis** (wk 11–15) | ORT runtime + model manager (download with progress/cancel, checksum), SceneTags, faces, aesthetics, whisper.cpp transcripts, MediaAnalyzer, beats, captions end to end, Media search (tags + "what was said") | Search finds the same files as the Mac on the test library (≥ 90 % top-5 agreement); transcripts' WER and word timings documented; B8, B9 recorded |
| **P6 Magic + MCP** (wk 14–18) | MCP server/bridge/tools, AgentConnect (Claude Code, Codex, Claude Desktop if present), MagicAgent + MCPDraft, Local planners, llama.cpp LocalBrain, the thinking show + swarm, MediaDownload, ContactSheet | Tool list identical to the Mac; a scripted Claude Code session edits a project; Local Magic's ported tests give the same plans as the Mac for the same inputs |
| **P7 Ship** (wk 18–20) | AppImage/Flatpak/.deb, updates (EdDSA), About/credits, desktop integration (MIME, icon, file open, single instance), `PARITY.md` complete, first sync doc | Clean install on Ubuntu 26.04, Fedora (current) and Arch; update from N-1 to N works; all suites green |

The weeks are an estimate for one developer working with an agent; the order is what matters (P1 before P2/P4, since
everything renders through it).

---

## 15. The sync process

Run it for every Mac release, or at least monthly.

1. `git -C ../editroom log --oneline <lastSyncedSha>..HEAD`. Sort each commit as **port**, **n/a (Mac-only, reason)** or **defer (reason)**.
2. Port, test, and update `PARITY.md`.
3. `scripts/parity-diff.py ../editroom .`, which compares:
   - every user-visible string literal in Swift `Text(…)`, `Button(…)`, `title:`, alerts → `resources/strings.json`;
   - `ShortcutAction.defaults` → the LX table;
   - the `--dump-menu` output from both;
   - `Project`/`Clip`/`Track`/… Codable fields → the LX JSON schema;
   - the MCP tool names and input schemas.
4. Run all suites, golden frames (regenerate the Mac references when Mac rendering changed) and benchmarks.
5. Write `docs/sync/SYNC-<yyyy-mm-dd>-<macsha>.md`:

```markdown
# Sync <date> — editroom <old>..<new> (Mac <version>)
## Ported            (commit → LX change/PR)
## Not ported        (commit → reason: Mac-only / deferred until …)
## Parity diff       (strings / shortcuts / menus / schema / MCP: all equal, or the list)
## Tests             (suite → pass/skip/fail, tolerances)
## Timings           (bench → before → after → Δ, hardware, backend; median of 5)
## GPU candidates    (path → P1/P2/P3 → expected gain → what to measure → status)
```

---

## 16. GPU candidates (first list)

The Mac runs these on the CPU, or LX would at first. Rank them in each sync, and only claim a gain after
measuring it.

| Code path (Mac source) | Today | GPU idea | Priority | What to measure |
|---|---|---|---|---|
| `MediaAnalyzer` frame metrics (brightness, edge variance, change, colourfulness) on small frames | CPU / Accelerate | A reduction pass in `analysis.comp` on frames already decoded to the GPU (one graph: downscale → Sobel → reductions) | **P1** | s/clip on 100 clips; CPU load during import |
| Thumbnails / filmstrips / hover strips (decode → scale → JPEG) | Decode HW, scale+encode CPU | GPU scale in the decode graph; batch several timestamps per submit; nvJPEG/VA-API JPEG encode where present | **P1** | Time to fill a 50-clip library; UI hitches |
| Scrub cache building (12 fps frames from long files) | CPU encode (HEIC on the Mac) | A GPU scale + hardware intra encoder (VA-API/NVENC), all in one pipeline | **P1** | Minutes per hour of footage |
| Waveform peaks / RMS for long audio | CPU | Keep on the CPU with OpenMP (memory-bound); GPU only if profiling says so | P3 | ms per hour of audio, 1 vs N threads |
| Beat tracking FFTs (spectral flux) | vDSP / pocketfft | VkFFT batch STFT | P3 | s per 5 min song; it's probably already fast |
| BS.1770 loudness | CPU, serial biquads | Keep on the CPU; parallelise per channel/segment (with exact stitching of the filter state) | P2 | s per 60 min mix |
| Time-stretch for speed changes | CPU | Keep on the CPU (latency, real time); multi-lane threads | P3 | CPU % at 8 stretched lanes |
| Grade cube baking (32³ = 32k evaluations) | CPU | A compute pass evaluating `GradeRecipe`/LookMath per texel (shared `ColorMath.h`) | P2 | ms per slider tick while dragging a grade wheel (the Mac's 200-entry cache hides it) |
| SourceColor cubes (64³) | CPU, once per kind | Not worth it (cached once) | — | — |
| Text/caption rasterising | CPU (QPainter) | Signed-distance-field glyph atlas on the GPU, for karaoke captions whose lit word changes often | P2 | ms per caption change at 4K; export time with word-lit captions |
| ContactSheet for agents (grid of frames + time labels) | CPU (AppKit) | Compose the grid in `GpuGraph`, labels as a text layer | P3 | ms per sheet |
| Reversed audio render | CPU, I/O bound | none | — | — |
| Export pre-encode RGB→YUV | GPU (planned) | Zero-copy DMA-BUF into VA-API (no readback) | **P1** | × realtime on 4K export |
| HDR export's P010 pack (1.1) | Done: `pack.comp`'s 10-bit BT.2020 mode (166–170 → 38.5–40 ms a 4K frame) | Next: hand P010 to VA-API as a DMA-BUF | P2 | × realtime of a 4K HDR export |
| HDR viewer output (1.1) | SDR view | HLG/PQ swapchain on HDR screens | P2 | That it matches the export on an HDR screen |

---

## 17. Risks, open questions and things to decide

1. **Software H.264/HEVC encoding licence.** The Mac ships an LGPL-only FFmpeg and encodes in hardware.
   On Linux, machines without VA-API/NVENC need a software encoder: libx264/libx265 are GPL, and openh264 is
   BSD but H.264-only and lower quality. Options: (a) ship LGPL FFmpeg, use OpenH264 for software H.264 and
   leave HEVC hardware-only; (b) use the *system* FFmpeg dynamically (distro builds include x264), not
   bundled; (c) make LX GPL. **The owner needs to decide this.** Plan of record until then: (b) for
   .deb/Flatpak, (a) for the AppImage.
2. **Pixel parity limits.** Core Image's internal precision and resampling choices are undocumented. The
   golden-frame tolerance may have to be loosened for scaled layers. Measure on Mac goldens first, then set
   the numbers in `tests/golden/README.md`.
3. **Fonts.** SF Pro can't ship. Text clips and captions will differ in glyph shapes and metrics.
   Placement maths and sizes match; projects naming Mac-only fonts fall back through fontconfig, and the
   app lists the missing fonts.
4. **Apple Log signalling.** The Mac reads `LogTransferFunction` from the format description. FFmpeg may
   not surface it. Investigate the MOV sample-description extension atoms (and the `com.apple.*` metadata
   iPhones write), and add a small MOV atom reader if needed. Test it with real iPhone 15/16/17 Pro Apple Log
   clips plus the `applelog_` patch clip.
5. **Hardware decode → Vulkan zero copy** varies by driver (Intel iHD vs Mesa, NVIDIA proprietary vs
   nouveau, modifiers). Keep the one-upload fallback always working, and record which path ran in the bench output.
6. **Updates feed.** Share `appcast.xml` with a Linux enclosure, or keep a separate `appcast-linux.xml`?
   Sparkle on the Mac ignores enclosures for other OSes, but this needs checking against the Mac's custom
   `UpdateDriver`.
7. **Claude Desktop on Linux.** Detect it at runtime; if absent, the Claude route is Claude Code only, and
   the wording in Settings › Magic must still read right (tracked as a string difference in `PARITY.md`).
8. **Alt+drag** conflicts with window managers (§8.3). It is documented, not remapped.
9. **Wayland**: window placement on first launch ("fill the screen") isn't allowed to clients; use
   `showMaximized()` on first launch (closest match). Full-screen viewer via `showFullScreen()` works.
10. **Local models' size and quality.** Whisper/LLM/CLIP downloads total about 1–3 GB, and are opt-in, with
    progress, cancel, checksums and a "Remove downloaded models" button. The quality of Local Magic's request
    understanding will differ from Apple's model. Measure it with the Mac's LocalBrain test prompts.
11. **HDR display output.** Since 1.1 the Mac shows an HDR project's frames with the screen's headroom (EDR).
    Linux HDR output needs the compositor's colour-management protocol (KDE Plasma 6, GNOME 48+) and a Vulkan
    swapchain in an HDR colour space; LX shows HDR projects as SDR until that can be checked on an HDR screen.
12. **Tabs and resources.** A tab behind keeps its editor (undo history, library, caches) but no player; every
    player shares one Vulkan device. Many tabs of large projects still hold their libraries and caches in memory.

---

## 18. Setting up the dev machine (Ubuntu 26.04, this box)

Present: Qt 6.10.2 (Core/Gui/Widgets/OpenGL/Concurrent/Network/DBus/Test/Wayland), Vulkan headers 1.4.341, GCC 15.2,
CMake, Ninja, the ffmpeg 8.0.1 binary, Mesa `intel_icd` + `lvp_icd`, `claude` and `codex` CLIs.

Missing: install them before P0.
```bash
sudo apt install libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev \
  libva-dev libdrm-dev libvulkan-dev vulkan-tools glslc spirv-tools mesa-vulkan-drivers \
  libheif-dev libjpeg-turbo8-dev libsodium-dev libsoxr-dev libpipewire-0.3-dev libpulse-dev \
  qt6-base-private-dev qt6-svg-dev clang clang-tools libomp-dev intel-media-va-driver vainfo
# Vendored or fetched by CMake (FetchContent, pinned): miniaudio, signalsmith-stretch, pocketfft,
# whisper.cpp (GGML_VULKAN=ON), llama.cpp (GGML_VULKAN=ON), onnxruntime (prebuilt, pinned), VkFFT (optional).
```
Check: `vulkaninfo --summary` lists the Iris Xe and llvmpipe; `vainfo` shows H.264/HEVC decode and encode
entry points.
