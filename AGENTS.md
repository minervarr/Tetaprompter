# AGENTS.md — Teleprompter_Vk

Guidance for AI agents working in this repository.

## Project

A single-user Linux teleprompter: a Vulkan/Wayland window draws a timing score
while `ffmpeg` records the camera and microphone. The recording is clean — the
script is never composited into the video.

This is the C++17 port of `../teleprompter` (Python/GTK4). The Python source is
preserved read-only in `prototype/` as the behavioural reference.

## Hard rules

- **C++17 + CMake + Ninja** for the product. No Qt, GTK, Tk, browser or webview
  in the C++ path.
- Use the first-party framework submodules:
  - `libs/firstparty/framework/Vk_Canvas_Lb_LAW` (Vulkan canvas, text, Wayland)
  - `libs/firstparty/framework/App_shell` (`Host`/`AppView` seam, paths, host)
  - `vulkan_font_engine`, FreeType and msdfgen arrive nested through vk_canvas.
- `prototype/` is reference data. Do not edit it unless the task is explicitly
  about the Python app.
- Keep the portable logic in `core/` free of Vulkan and Wayland headers.
- Recording is exclusively `ffmpeg` in a subprocess. No OpenCV, GStreamer
  bindings or in-process encoder.
- The script text must never end up in the recording. ffmpeg captures the
  camera device directly; the UI is not part of that stream.
- No camera preview in the UI. The camera opens only while recording.
- No user-facing config files except disposable state. The recents cache at
  `$XDG_STATE_HOME/teleprompter/recents.json` and the one-line font preference
  at `$XDG_STATE_HOME/teleprompter/settings.json` are best-effort; losing either
  must never prevent a script from opening.
- Do not add a `--duration`, anchors or an estimated fallback. The score's
  summed word/comment times are the only total.

## Files

```
teleprompter.pyz-shaped C++ app: core/ + gui/
core/include/teleprompter/   public core headers
core/src/                    parser, timeline, colours, recents, recorder
gui/src/app.{hh,cc}          AppView implementation, pages, input, draw
gui/src/layout.{hh,cc}       word wrapping + per-glyph placement
gui/src/text.{hh,cc}         RasterFont loader + text-quad emitter
tests/core_tests.cc          pure-logic checks (no display)
samples/test-script.txt      shipped demo score
assets/fonts/                New Computer Modern, OpenDyslexic and mono faces
assets/fonts/openDix/OFL.txt OpenDyslexic SIL OFL 1.1
prototype/                   Python/GTK reference implementation
docs/                        screenshot, PORTING.md, ROADMAP.md
manifest.json                version + build entry point (CMake and scripts/)
cmake/TestHelpers.cmake      teleprompter_add_test(), Debug-only
scripts/                     build.sh, run.sh, verify.sh, builder sources/zipapp
patches/                     notes/patches for framework changes
```

## Build / verify

```sh
git submodule update --init --recursive
./scripts/build.sh                  # Debug into build/ (tests included)
./scripts/verify.sh
./build/gui/teleprompter --print-timing samples/test-script.txt
./build/gui/teleprompter --print-timing --words samples/test-script.txt
./build/gui/teleprompter --check            # camera + mic, 1 s each
./build/gui/teleprompter --windowed         # interactive check
```

`scripts/build.sh` drives the stdlib-only Python zipapp
(`scripts/teleprompter_builder.pyz`, sources in `scripts/src/`):

```sh
./scripts/build.sh build --release   # Release into build_release/ (no tests)
./scripts/build.sh build --no-gui    # portable core + tests only
./scripts/build.sh build --dry-run
./scripts/test_builder.sh
```

The builder probes for `slangc`/`$VULKAN_SDK`, the Wayland/xkb pkg-config
modules and `wayland-scanner`; it enables `TP_BUILD_GUI` when they are present
and falls back to a core-only build when they are not. `--gui`/`--no-gui`
override. Tests are Debug-only: `teleprompter_add_test()` produces no target in
Release. The direct cmake recipe is equivalent:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

For a non-interactive rendering check:

```sh
./build/gui/teleprompter --seek 8 \
  --capture /tmp/tp.ppm samples/test-script.txt
ffmpeg -y -loglevel error -i /tmp/tp.ppm /tmp/tp.png
```

`--capture` writes a PPM after 6 frames. It is fullscreen on the compositor's
current output unless `--output` pins one, and exits, so it does not leave a
window behind. The test script's total is `0:23`.

## Behaviour contract

Keyboard is captured by the app's `onKeyDownPortable` (App_shell forwards it):

| Key | Action |
|---|---|
| Space | Start / pause; restart at the end |
| R | Start / stop recording |
| Ctrl+R | Return to library / refresh it |
| Ctrl+O | Open the built-in file picker |
| F / F11 | Toggle fullscreen; Esc leaves |
| Up / Down | Reading line ±2 % |
| Shift+Up / Down | Same alias |
| C | Cycle 15/25/35/50 % |
| A | Centred / flush left |
| K | Karaoke fill |
| M | Mood colours |
| T | Choose the script font (library button too) |
| Left / Right | Nudge scroll |
| Home / End | Top / bottom |
| F5 or L | Reload |
| H | HUD + section timer |
| ? or F1 | Key map; any key/click closes |
| Ctrl+Q | Quit, stopping recording first |

- Startup without a path is the **library** (recents newest first, missing
  files pruned, always-available **Test script**, built-in **Open script**
  picker). The initial window is fullscreen on the compositor's current output
  unless `--output CONNECTOR` pins one; `--windowed` restores a windowed start.
  Invalid cards say
  `needs timing` and do not switch pages.
- Opening a valid script switches to the fullscreen prompter unless
  `--windowed`. Ctrl+R pauses, leaves fullscreen and refreshes the library.
- Recents store absolute paths + unix timestamps in
  `$XDG_STATE_HOME/teleprompter/recents.json`, dedupe by resolved path, cap at
  30, and write atomically. The test script is never recorded in recents.
- Fonts: two presets, `newcm` and `opendyslexic`, plus arbitrary `--font
  PATH`. All text uses the chosen face, including stage directions and
  comments. OpenDyslexic uses a 0.70 size factor and falls back to New Computer
  Modern per style for the em dash, ellipsis and arrows it lacks. The chosen
  font is remembered in
  `settings.json` whether selected from the menu or supplied with `--font`.
  The renderer stays on RasterFont, which rasterizes actual Bézier outlines at
  the exact pixel size — the highest-quality fixed-size path. MTSDF is not the
  default.
- Word prefixes: `[seconds]` before a word/comment; `{mood}` / `{mood: Theme}`
  / `{#rrggbb}` persist; a tag-only paragraph is a header. Every displayed
  token needs a positive time. `[1.5] [look]` is a timed comment; `[2.0]` alone
  is a silent beat; `[mm:ss]`, zero durations and bare comments are errors.
- `paragraph_plan()` returns `Paragraph`/`Word` records with summed seconds,
  absolute clocks, character and UTF-8 byte offsets. `ScriptTimingError`
  carries every issue with its source line.
- Karaoke: spoken words start at `KARAOKE_GRAY`; the active word fills
  left-to-right per character over its authored seconds; finished words stay
  lit. Comments are always amber, italic, and brighten uniformly; inline
  comments get a dark chip, standalone comments a card with an amber left rule.
  `K` disables the fill; `M` disables spoken mood tint only.
- The reading line is two amber side bars + a `- 25%` HUD badge. The first line
  starts on it; the last line lands on it at full weight. `--line` sets the
  starting fraction (clamped 5–60 %).
- Section card: while a standalone comment or silent beat is active, a top
  centre card shows title, `m:ss left of m:ss` and an amber progress bar. It
  hides during spoken text/inline comments/end and with `H`; HUD dimming does
  not fade it.
- HUD: Scripts/Record buttons, state chip, mood/theme/time-left chip, recording
  dot/timer, message, countdown, cue badge, total. It dims to 30 % after 3 s of
  playback; pointer motion or pausing restores it. A red frame rings the window
  while recording.
- Reaching the end stops playback, shows `Done`, holds the last line on the
  reading line; Space restarts.

## Architecture notes

- `core` has no Vulkan/Wayland includes. `gui` owns the `AppView`, `Renderer`,
  `TextPainter` and `ScriptLayout`.
- The build front end is a stdlib-only Python zipapp
  (`scripts/src/teleprompter_build`, regenerated to
  `scripts/teleprompter_builder.pyz`); it only drives cmake + ninja.
  `manifest.json` is the version source for both the builder and CMake.
- `TextPainter` loads a `FontPreset` (`newcm` or `opendyslexic`) via
  `RasterFont::open/addStyle`, registers New Computer Modern fallbacks for
  missing codepoints, bakes every collected codepoint at every used size, and
  emits glyph quads itself so per-character karaoke colours are possible. It
  does **not** use `Canvas::text` for script text. `TextPainter::reload()` is
  the runtime switch; the atlas is re-uploaded through `Renderer::initMsdf()`.
- `layout.cc` wraps on measured word widths, records every glyph's line centre,
  and builds `ScriptLayout::word_center`. Several words on one line produce
  repeated timeline positions; that is how a line dwells.
- `Timeline` maps weight (seconds) to pixels. `pos_from_weight()` drives the
  scroll; `weight_from_pos()` adopts wheel scrubs. The first line centre is 0;
  `max_scroll` is the last line centre so the last line can reach the cue.
- `App` draws one `Canvas` shape batch plus one text-quad batch per frame:
  `renderer_->draw({}, 0, {}, {}, text_quads_, shapes_)`. HUD/section panels
  are also emitted as solid text-pipeline rects so they occlude script text
  (shapes are composited before all text).
- Recording is `core/recorder.cc`: `fork`/`execvp`, stdin pipe for `q`, a
  daemon stderr drain (bounded deque), and a stop thread. The tick loop polls
  and reports through `GLib`-free state (`onTimer` from App_shell).
- Fullscreen is `Host::setFullscreen()` (added to App_shell). App_shell also
  exposes `app_shell_restore_console()` for CLI output when stdout is a pipe.
- The built-in file picker is immediate-mode: directories then `*.txt`, `*.md`,
  `*.text`; Enter opens, Backspace goes up, Esc cancels.
- The base font size is 5.5 % of monitor height, clamped 28–120 px. Per-preset
  `size_scale` is applied to it (OpenDyslexic uses 0.70); line height is
  `max(1.35×size, font.lineHeight(size))`, and paragraph gap follows the same
  metric. Column width is at most 66 % of the viewport and centred.

## If touching the framework submodules

Commit with `./git_wrapper` inside the submodule, not plain `git commit`. Keep
changes small and documented. Record the commit and a patch in `patches/`.
Never rewrite the vendored Python prototype.
