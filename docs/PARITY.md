# PARITY — where Editroom LX differs from the Mac, and why

Mac baseline: `editroom@5f31e48` (1.1.2, build 16). Last sync: [SYNC-2026-11-28-5f31e48](sync/SYNC-2026-11-28-5f31e48.md). Each row says what LX does instead. Rows marked
**pending** are not done yet; they are not decided differences.

## Status by phase

| Phase | State |
|---|---|
| P0 Foundation | **Done.** Model, `.room` codec, Timecode, shortcuts, `EditorModel` (undo, live edits, autosave, recovery), `parity-diff.py`. Gaps: CI, and a corpus of real Mac-saved projects. |
| P1 Render core | **Done.** One compositor kernel (`ColorMath.h` + `Kernel.inc`) runs as a Vulkan compute shader and on the CPU with OpenMP. GPU vs CPU: max \|Δ\| ≤ 1, PSNR ≥ 92 dB on Iris Xe and lavapipe. FormatCheck patches for SDR, HLG, PQ and Apple Log are within 1–2 codes of Rec.709. Gaps: no Mac golden frames yet. |
| P2 Media and audio | **Done.** FFmpeg decode (VA-API → Vulkan Video → CUDA → software), stills and HEIF, chunk cache and reverse frames, AudioPlan, mixer with Signalsmith time-stretch, BS.1770 loudness, waveforms, reversed sound, miniaudio playback. |
| P3 Editor UI | **Mostly done.** Project tabs (1.1) are a full port of Workspace.swift and TabStrip.swift: tabs in the title strip, New Tab, Close Tab, Show Previous/Next Tab, the "more tabs" menu, tabs reopening at launch with their own recovery copies, tabs behind letting go of their player, paste between projects (`tst_tabs`). The Media panel is a full port of LibraryView.swift (linked folders, opening and going back, Thumbnails or List, search, Shift/Ctrl picking, hover-scrub into the viewer, the Text and Solid buttons, Locate and Reload; `tst_library`). The viewer and transport are a full port of PreviewView.swift (drag to place, zoom, empty-timeline prompt, typed time, output meters, full-screen viewer; `tst_viewer`). Scrubbing uses made-ahead frames (`tst_scrub`). The timeline is a full port of TimelineCanvas.swift and TimelinePanel.swift: every drag (move with linked sound following, Alt-copy, trims sticky or free, fade dots, gain line, box select, markers and their panel, in/out edges), autoscroll, snapping, context menus, drops, filmstrips with grades applied, waveforms, the ruler, the track headers with level meters, the gain pill and Duck Under, and the two range bars (`tst_timeline`). Audio scrubbing and track meters work. Main window, library, preview, timeline, export dialog, settings, the Mac's menus and wording. The inspector is a full port of InspectorView.swift: folding sections with Reset, rows with typed values and ↑/↓ steps, fade curves, the three grade wheels, the speed-ramp graph, the zoom graph, the font picker and the caption style (`tst_inspector` drives them with the mouse). Missing: see *Pending* below. |
| P4 Export | **Done.** Export queue, GPU composite and NV12 pack, hardware encoders (VA-API, NVENC, Vulkan) with libx264/libopenh264 fallback. HDR projects (1.1) export 10-bit HEVC tagged BT.2020/HLG (`tst_export::hdr`). |
| P5 Analysis | **Done.** Frame metrics, best moments and the beat tracker (`src/analysis`). Faces: YuNet, shipped with the app (`engine/Faces`). Aesthetics and "utility": CLIP prompts (`SceneTagStore::aesthetics`). Scene tags: CLIP over Vision's labels, with the Mac's search and aliases (`engine/SceneTagStore`, `model/SceneTags`). Transcripts: whisper.cpp (`engine/Transcripts`), feeding captions, `get_transcript`, `set_captions` and speech search. The speech language (1.0.12) is in Settings › General › Transcription. The speech and scene models are downloaded only when the person asks, in Settings › General › On this computer (or the offer shown when captions are first wanted). Tests: `tst_transcripts`, `tst_scenes`; the model-dependent cases skip unless `EDITROOM_WHISPER_MODEL` and `EDITROOM_CLIP_MODEL` are set. |
| P6 Magic and MCP | **Mostly done.** All 43 MCP tools work, `auto_edit` and the transcript and caption tools included: the HTTP server on 127.0.0.1:47920, the stdio bridge, drafts, Claude Code and Codex runs, and connecting those apps. Local Magic is ported: LocalBrain's word helpers, AutoPlanner, the Choreographer, LocalImprover, and the run itself (`tst_local`). Local's planners are all ported: captions, title, add, ramp, speech pauses, combined requests, pointed edits, moves by name, @mentions and typed times (`tst_local`). The Magic box lists what `@` can point at and bolds mentions and times; the agent brief says where they were pointing. Edits are played by cursors on the timeline (a swarm for Local, one named cursor per agent), live and as one undo step, and Esc puts everything back. `@folder:` and `@folder:…:only` (1.1.2) point Magic and the AI apps at a folder in Media (`tst_local::mentionFolder`). |
| P7 Packaging | **Mostly done.** CPack `.deb` and `.tgz` (FFmpeg linked dynamically, dependencies found by shlibdeps), the AppImage (`scripts/make-appimage.sh`, shipped since 1.0.10.1), a desktop file, the MIME type for `.room`, the icon, single instance with files handed on, and an About box with credits. The Flatpak recipe (`packaging/flatpak/`) is written but hasn't been built: this machine has no `flatpak-builder` or KDE SDK. |

## Pending (not decided differences; still to build)

| Area | Mac | LX now |
|---|---|---|
| HDR in the viewer (1.1) | An HDR project's frames are shown with the screen's headroom (EDR) | Shown as SDR: SDR white at white, brighter highlights clipped, as on a screen with no headroom. Presenting HLG to an HDR screen needs an HDR swapchain (`VK_EXT_swapchain_colorspace` with the Wayland colour-management protocol, KDE Plasma 6 and GNOME 48+) and an HDR screen to check it on. The export is HDR. |
| Flatpak | — | `packaging/flatpak/com.wonderassembly.EditroomLX.yml` is written but not built here (no `flatpak-builder`; it needs the KDE 6.10 SDK from Flathub, a few GB): `flatpak install --user flathub org.flatpak.Builder org.kde.Sdk//6.10 org.kde.Platform//6.10`, then `flatpak run org.flatpak.Builder --user --install build-flatpak packaging/flatpak/com.wonderassembly.EditroomLX.yml`. |

Done since the last sync, and no longer pending: Magic's cursor show (its outlines now sit on tracks the coming edit
adds too), presenting while zoomed (only the part in view is presented), the shortcut editor (it records in place and
now sees every key while recording, as the Mac's key monitor does), and the Settings wording (checked against the Mac;
see below).

## Decided differences

| Area | Mac | LX | Why |
|---|---|---|---|
| Speech recognition | Apple's Speech framework, on-device (macOS 26), comes with macOS | whisper.cpp v1.8.2 (MIT, static, CPU) with the multilingual `ggml-base` model, downloaded once when asked (about 150 MB, to `~/.local/share/Editroom/Models`) | Linux has no system speech recogniser. Same word-by-word transcript format and cache layout (`Editroom/Transcripts`). |
| Scene tags | Vision's `VNClassifyImageRequest` (plus Apple Intelligence keywords on macOS 27) | CLIP ViT-B/32 (MIT, quantised ONNX from Xenova) picks among Vision's own label words, a frame every couple of seconds; downloaded once when asked (about 150 MB). Kept where the share of all labels in a frame is at least 0.08 | No Vision on Linux. The labels, aliases and search are the Mac's, so the words people search with match. Scores differ in scale from Vision's, so the threshold differs. |
| Faces | Vision face rectangles | YuNet 2023mar (MIT, 230 KB, shipped in `share/editroom/models`), score ≥ 0.8, NMS 0.3; faces under 0.4 % of the frame are left out, as on the Mac | No Vision on Linux. Small enough to ship, so nothing is downloaded. |
| Aesthetics and utility | `VNCalculateImageAestheticsScoresRequest` (overallScore −1…1, isUtility) | With the scene model: CLIP prompts. The score is 2·P(well made) − 1 against "badly made"; utility is P(screenshot) + P(document) > 0.6. Without it: the Mac's own "not available" value (0.12), as on macOS 14 | No Vision on Linux. Same range and use in AutoPlanner's scorer. Analyses are cached as `-lx2` (faces) or `-lx2a` (with aesthetics), so getting the model looks at clips again. |
| ONNX Runtime | — | Loaded with `dlopen` when first needed, from the system's `libonnxruntime` (1.23, MIT). Headers are vendored in `third_party/onnxruntime` | Editroom runs without it. Ubuntu's build also prints hundreds of "Schema error" lines when it loads, so stderr is silenced during the load. |
| Reading Local requests | Apple's on-device language model (FoundationModels) fills `RecipeOut` | Rules built from the same word lists the Mac checks the model's answer against (`LocalBrain::checked`), plus the Mac's own helpers (shot counts, speed, titles, quotes, named media, what files show) | Linux has no system language model. It runs on this computer with nothing downloaded. Requests worded unusually may be read less well than on the Mac. |
| SelfTest | `--selftest` runs a 2,000-line tour of the engine | `--selftest <media> <out>` builds the Mac's own test edit and exports it four ways (H.264, HEVC web 4K, no audio, WAV); the rest of the tour is LX's unit tests. `--format-check <out> <files…>` is a full port. Both run in ctest | The engine checks already exist as ctest suites, which is where LX keeps them. |
| Vulkan Video encode | — | Before a Vulkan encoder is used, one red and one blue frame are encoded and decoded in software. If the colour doesn't come back, that encoder is skipped for the session and the next one (libx264, libopenh264) is used | On Intel ANV (Mesa, `ANV_DEBUG=video-encode`), `h264_vulkan` keeps brightness but scrambles chroma, and ffmpeg's own CLI does the same. `--format-check` found it. |
| Theme and menus (owner's request, 2026-11-26) | Dark until chosen; orange accent in every theme; menus in the Mac's style | Grey until chosen. The Grey theme has no orange: its accent (switches, the playhead, what's active) is a mid grey. Menus look like the desktop's: a grey panel, a lighter grey rounded row under the pointer, the text unchanged, 14 px type (menu bar too). Text selection and list highlights are grey in every theme | The owner asked for it. |
| App icon (owner's request, 2026-11-26) | The Mac's icon | `icons/icon.jpeg`: the rounded square cropped out with a crisp rounded mask (the "videoLX" caption under it left out), installed at 16–512 px in hicolor | The owner's new icon. |
| Updates | Sparkle, with its appcast | Help › Check for Updates… and Settings › Updates ask GitHub's API for the latest release of ClaudiuJitea/editroomLX and compare versions, in Sparkle's wording. Automatic checks are off until turned on, then at most once a day at startup. Nothing is downloaded or installed: "View Release" opens the page | Linux has no Sparkle; packages are installed by the person (or their package manager). |
| Export upscaling (1.0.5) | MetalFX's spatial upscaler enlarges video drawn bigger than its pixels on export | AMD FidelityFX Super Resolution 1's EASU (MIT), ported into the shared kernel (`er_sample_easu` in `Kernel.inc`), so the GPU and CPU paths give the same pixels (`tst_gpu` case `upscale`; `tst_render::upscale` checks same colours and way up as a plain resize, an edge no softer, and no ringing). Used on export only, for video layers fitted at more than 1.01×. No sharpening pass (FSR's RCAS), since that needs a second pass over the result. The Export dialog says "Upscaled from …p" | Linux has no MetalFX; EASU is the nearest open spatial upscaler. |
| HDR to SDR (1.0.7) | HLG and PQ are converted by AVFoundation as they're read, as macOS's own SDR export does (steered by Dolby Vision metadata where there is some) | HLG and PQ with a Dolby Vision configuration (iPhone HDR, `AV_PKT_DATA_DOVI_CONF`) use the curve the Mac fitted to macOS's own conversion of iPhone HLG (0.317 × light^0.88 + 0.014, the lift fading toward black, saturation 0.88, highlights easing from 0.85). HDR without it (BT.2408 graded, as the test patches) keeps white at white, which is what the Mac's format check expects from AVFoundation | Linux has no AVFoundation. The Mac measured the curve within about 2% of macOS on iPhone clips. Not yet checked against a real iPhone file on Linux: the test clips carry no Dolby Vision. |
| Drag from Photos (1.0.6) | File promises (Photos, Mail): placeholders, "From Photos" copies beside the project | Not ported | Linux has no file promises: file managers (GVfs phones and network shares too) hand over `file://` paths, which import directly. |
| Drops vanish into Media (1.0.6) | A native drop layer stops macOS's fade-back animation | Not ported | Linux desktops draw drops themselves; there's no fade to stop. The Media panel still lights up for drops anywhere in the window. |
| Edits while playing (1.0.7) | AVPlayer spare-player hand-off fixes | Not needed | LX's player swaps the plan at the playhead and only ever renders the frame being heard, so there's no stray start frame or freeze to fix. |
| MKV sound (1.0.8) | Vorbis, DTS and TrueHD converted to AAC in a playable copy | Decoded directly | LX reads every file with the system FFmpeg, which decodes these; there's no playable copy. |
| Alerts (1.0.8) | Centred on the window, 10% above the middle; queued clicks dropped so macOS doesn't beep | Centred the same way on X11; on Wayland the compositor places dialogs over their window | Wayland doesn't let clients place windows. Qt doesn't beep at queued clicks. |
| Bluetooth sound (1.0.10) | The output's delay (AirPods ~171 ms), as Core Audio reports it, is added to how far ahead sound is queued and to when the picture starts | The audio clock (`AudioStream::currentTime`, which the picture and playhead follow) is what's handed to the device less the device's buffer and the output's own delay. The delay is the sound server's `sink_usec` for a short silent probe stream (libpulse, loaded at run time; PipeWire and PulseAudio), read when playback starts and every 4 s while playing. Playback starts with the sound and plays out to the end. `editroom-lx --sound-delay` prints it (built-in speakers here: 5 ms beside a 30 ms buffer); `tst_audio::outputDelay` checks the clock with a forced 250 ms | LX pulls sound from the mixer, so the crackle and silence the Mac fixed can't happen; the sync can. Not yet measured with real Bluetooth headphones. |
| Name (owner's request, 2026-11-27) | "Editroom" | "EditroomLX" wherever it's seen: the launcher (`editroomlx.desktop`, icon `editroomlx`), the window's app id and title ("Untitled — EditroomLX"), the menus (Quit EditroomLX, About EditroomLX), alerts, Settings, update messages, the file dialogs, and what AI apps are told (the tool instructions and Magic's brief are rebranded as they're loaded, with "this Mac" read as "this computer", so the files stay as extracted from the Mac). Internal names stay "Editroom": settings, caches and data folders (so nothing moves), the MCP server id `editroom` in agents' configs, the User-Agent | Tells it apart from other Editroom builds on the desktop. |
| Bookmarks | `bookmark` (macOS bookmark data) finds moved files | Kept byte for byte and written back; files are found through `relativePath`, linked folders and Locate… | Bookmark data can only be resolved by macOS. |
| `playablePath` | Set when a file was converted with ffmpeg | Kept from Mac projects and used if the file exists here; never written for new imports | LX decodes MKV/WebM/MXF and the rest directly. |
| Unknown keys | Ignored by Swift's decoder (and lost on save) | Kept on every object and written back | So a file from a newer Mac build survives being saved on Linux. |
| Lenient reading | A missing required key makes the Mac refuse the file | LX uses the field's default | Reads more than the Mac. LX always writes every required key, so the Mac reads what LX writes. Unknown enum values are still refused, as on the Mac. |
| `captionFixes` order | A Swift dictionary written as `[key, value, …]`, in whatever order the hash gives | The same shape, with entries sorted by UUID | Gives stable output. Order carries no meaning, and the round-trip test compares it as a map. |
| Compact JSON (autosave, recovery) | Unsorted keys | Sorted keys | Gives stable diffs. Saved files are pretty-printed with sorted keys on both. |
| Shortcut display | `⌘⇧S`, `⌥F`, `⌃T`, `⌫`, `⌦`, `↩` | `Ctrl+Shift+S`, `Alt+F`, `Meta+T`, `Backspace`, `Delete`, `Enter`, with Qt's modifier order (Meta, Ctrl, Alt, Shift). Arrows stay `← → ↑ ↓` | Platform convention (PROMPT.md). The stored form is the Mac's. |
| Hidden files and packages in linked folders | `.skipsHiddenFiles`, `.skipsPackageDescendants` | Dot-files are skipped. Folders with a macOS package extension (`.app`, `.photoslibrary`, `.fcpbundle`, …) are skipped | Linux has no packages. This keeps a Mac drive mounted on Linux behaving the same. |
| "Is media" | `UTType` conformance to movie/video/audio/image, plus a list of ffmpeg extensions | Freedesktop MIME type (`video/*`, `audio/*`, `image/*`, or one of their ancestors) by extension, plus the same list | The closest thing to UTType on Linux. |
| Cut and Play settings migration | Present | None | There was never a Cut and Play on Linux. |
| `MenuCleanup` | Removes items macOS adds to menus | None | Qt adds no system items. |
| SF Pro | System font | Inter (bundled, OFL) | SF Pro may not be redistributed. Text and caption glyphs will differ, but placement and size match. |
| Linked folders renamed or moved on their drive | Followed through macOS bookmarks | Not followed on their own (bookmarks can't be resolved on Linux): Locate… finds the folder, and everything in it with it. Files added to a linked folder still come in when Editroom comes back to the front. |
| Scrub frames | HEIC, one file per video in Application Support | JPEG, one file per video in `~/.cache/Editroom/Scrub`, the same layout (count, frame ends, frames) | Quicker to write and read here; Linux has no HEIC encoder in Qt. Rec.709 video only, as on the Mac. |
| "Show in Finder" | Finder | "Show in File Manager" (the freedesktop FileManager1 interface, or the folder opens) | Platform. |
| Viewer full screen | The window covers the screen, Dock and menu bar hidden | The window goes full screen with only the viewer showing | Same idea, done with the desktop's full screen. |
| Inspector icons | SF Symbols | Line icons drawn with QPainter in `src/ui/Symbols.cpp`, named after the symbols they stand in for | SF Symbols may only be used on Apple platforms. |
| Inspector name column | 52 px | As wide as "Exposure" and "Fade Out" need in Inter (about 56 px), never narrower than 52 | Inter is wider than SF Pro; at 52 px the names were cut off. |
| Snapping toggle while dragging | ⌘ held | Ctrl held | Platform convention. |
| Copy while moving | ⌥ held | Alt held | Same key; some Linux desktops take Alt-drag for moving windows, which may need turning off there. |
| Snap feedback | A light tap on the trackpad (NSHapticFeedbackManager) | None | No haptics on Linux. |
| Wheel | Trackpad deltas; ⌥ or ⌘ zooms | Pixel deltas from touchpads, notches (×12 px, as the Mac's line deltas) from wheels; Alt or Ctrl zooms | Same feel on both kinds of device. |
| Colour wells | The system colour panel, as the colour changes | Qt's colour dialog: the colour applies as it's picked, Cancel puts it back | No system colour panel on Linux. |
| App menu | About, Check for Updates…, Settings…, Quit | About Editroom and Check for Updates… under Help, Settings… under Edit (Ctrl+,), Quit Editroom under File (Ctrl+Q) | Linux has no app menu (PROMPT.md). |
| Tabs (1.1) | Tabs in the window's title bar (the toolbar), Show Previous/Next Tab in the Window menu | Tabs in the 38 px title strip under the desktop's own title bar; a Window menu (before Help) holds Show Previous Tab and Show Next Tab. The editor under the strip is built for the tab in front, as the Mac's `ContentView` is. Tabs behind aren't even made until first shown (the Mac makes their model and waits to load the project). Every tab's player shares one GPU | Native window decorations (PROMPT.md); the rest is the same behaviour, cheaper. |
| Close Tab (1.1) | ⌘W | Ctrl+W | PROMPT.md said "no Close or Ctrl+W" because the Mac had none; since 1.1 the Mac has Close Tab on ⌘W. |
| Add Text (1.1) | ⌃T | Meta+T (the Super key with T): ⌃ maps to Meta | Some desktops keep Super shortcuts for themselves; Add Text is also in the Clip menu and the Media panel. |
| Ctrl+Shift+[ and ] | ⇧⌘[ and ⇧⌘] | Both Ctrl+Shift+[ and Ctrl+{ (and ] / }) | Most layouts send { with Shift held; both are accepted so the shortcut works either way. |
| Speech language (1.0.12) | Automatic is the Mac's language; the list is what Apple's speech recognition has on that Mac; a language it doesn't have (Vietnamese) is said | Automatic hears which language each clip is in (whisper.cpp detects it); the list is whisper.cpp's languages, by name; a language it doesn't have (or anything but English with an English-only model) is said, in the same places | whisper.cpp hears nearly 100 languages and can tell them apart, so Automatic does that rather than assume the desktop's language. The setting is the Mac's (`speechLanguage`, a locale id or language code). |
| HDR projects (1.1) | Core Image colour-managed in extended linear Rec.2020 (half float); HLG and PQ read as HLG by AVFoundation | The same working space written down in `ColorMath.h`: BT.2100 HLG (system gamma 1.2 at 1000 cd/m², so HLG 0.75 is SDR white at 1.0), PQ to cd/m² over 203, BT.709's curve for SDR video (and Log after its cube), sRGB for titles, solids, photos and captions, BT.2087's 709→2020 matrix. The GPU and CPU share it (`tst_gpu` hdr cases) | No Core Image on Linux. HLG levels and standard white match the Mac's self-test exactly (0.750 / 0.950 / 0.750); mid-tones of SDR clips in an HDR project still need a Mac golden frame. |
| HDR export encoders (1.1) | VideoToolbox HEVC Main 10 | hevc_vaapi, then hevc_nvenc, then libx265, Main 10, P010 | The Vulkan Video encoders aren't used for 10-bit: their colour check is 8-bit. |
| Viewer presentation | A Metal layer, with SwiftUI drawn over it | The viewer presents straight from the GPU on X11 and Wayland: a native Vulkan window over the picture (only the part in view when zoomed), blitted from the composite with nothing read back (mailbox when there is one). It paints a read-back frame instead while a library item is previewed and under Magic's thinking scene, which draw over the picture: Qt widgets can't draw over a native window. Hardware-decoded frames reach it as DMA-BUFs (VA-API → DRM PRIME → `VK_EXT_image_drm_format_modifier`, at most ~384 MB out per decoder; `EDITROOM_DMABUF=0` turns it off); Vulkan-decoded frames are downloaded on Intel ANV (exporting them crashes). `tst_present`, `tst_dmabuf` | A window system can't put widgets over a native child window; everything else is the Mac's behaviour. |
| Settings › General › On this computer | — | Rows to download or remove the speech and scene models | macOS comes with both; on Linux they're downloaded only when asked. |

## Licences to confirm

- **FFmpeg in the `.deb`.** Editroom links FFmpeg dynamically and needs nothing beyond LGPL. Ubuntu's
  `libavcodec` is built with `--enable-gpl`, though, so a system FFmpeg may itself be GPL. The binary Editroom
  ships contains no GPL code. The owner should decide whether to document this or depend on an LGPL-only build.

## To confirm against a real Mac (open questions)

- **Empty containers in pretty JSON.** LX writes `[\n\n<indent>]`, as Foundation's JSON writer does, since
  it has no special case for empty containers. Confirm by byte-comparing a Mac-saved project that has an
  empty `clips` array. The format test does this as soon as such a file is in `tests/format/corpus/`.
- **Doubles at the edges.** Swift's `description` switches to exponent notation below 1e-4 and above
  2^54. LX copies this (`json::swiftDescription`). The rows in `tst_json` for 1e16 and 1e20 come from the
  `swift_dtoa` thresholds, not from a Mac run.
- **Key order.** Foundation's `.sortedKeys` is case-insensitive and numeric. Every key the model has is
  plain ASCII camelCase, so this matches code-point order anyway.
- **Real Mac files.** Every file in `tests/format/corpus/` is synthetic so far, written by hand from the
  Swift model. Add Mac-saved projects, and run `scripts/mac/check-open.swift` (to be written) on files LX
  saved, in the first sync done with a Mac.

## Parity tooling

`scripts/parity-diff.py` compares:

- the schema (all equal: 22 types, 141 fields);
- the shortcuts (all equal: 48 actions);
- the strings (290 literals collected into `resources/strings.json`; the two for Photos placeholders are listed but not used);
- the menus (LX prints its own with `--dump-menu`; no Mac capture yet);
- the MCP tools (43 on the Mac, 43 in LX, same names and schemas).

The string extractor catches the common SwiftUI and AppKit forms. Strings built in code, such as
`captionsMenuTitle`, are added by hand as the UI is ported.

## Analysis notes

- The beat tracker is the Mac's algorithm, step for step. vDSP's FFT is replaced by a plain radix-2 FFT
  scaled the way `vDSP_fft_zrip` scales, and the Hann window uses `vDSP_HANN_NORM`. On a 120 BPM click
  track it reports about 118 BPM; an independent numpy run of the same autocorrelation gives 117.6, so this
  is the algorithm, not the port. A Mac run on the same file is still needed to confirm the beats match.
- Frame metrics scale frames with Qt's smooth scaling where the Mac uses Core Graphics at medium quality,
  and grey uses Rec.601 weights where the Mac uses the device-grey conversion. Values are close, not equal;
  the scorer normalises by percentiles, so best moments should agree. To be confirmed against the Mac.
- Analysis results are cached in `~/.cache/Editroom/Analysis` with a `-lx` suffix so they are never mixed
  with a Mac's cache.
