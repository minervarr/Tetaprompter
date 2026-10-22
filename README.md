# Teleprompter_Vk

A Vulkan/Wayland teleprompter for Linux, ported from the Python/GTK4
[`teleprompter`](../teleprompter) project. The script is a strict timing score;
the presenter sees a fullscreen scrolling read while `ffmpeg` records the camera
and microphone **without ever compositing the script into the video**.

This repository is deliberately organised like the other minervarr desktop
projects (Matrix_Player, Klend-ario): a small portable core, a Vulkan frontend,
first-party framework submodules, and the original Python implementation kept
in `prototype/` as the behavioural reference.

![Teleprompter at 1920x1080](docs/screenshot.png)

## Build

Requirements (Arch names in parentheses):

- CMake ≥ 3.22 and Ninja
- a C++17 compiler
- Vulkan loader + headers (`vulkan-icd-loader`, `vulkan-headers`)
- Slang compiler (`slangc`, in the Vulkan SDK)
- Wayland client/cursor + xkbcommon (`wayland`, `xkbcommon`)
- `ffmpeg` / `ffprobe`

```sh
git submodule update --init --recursive
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./scripts/verify.sh
```

The executable lands at `build/gui/teleprompter`; shaders, fonts and the bundled
test score are copied beside it automatically. Run it from anywhere:

```sh
./build/gui/teleprompter                 # library, fullscreen on DP-2
./build/gui/teleprompter my-score.txt    # open a score fullscreen on DP-2
./build/gui/teleprompter --windowed my-score.txt
./build/gui/teleprompter --output DP-2 my-score.txt
```

`scripts/build.sh` is the same recipe, and `scripts/run.sh` builds and runs.

## Command line

```
teleprompter [script] [--line PCT] [--output CONNECTOR] [--device DEV]
             [--audio SRC] [--size WIDTHxHEIGHT] [--fps N] [--windowed]
teleprompter --print-timing [--words] [script]
teleprompter --check
```

- `--print-timing` validates a score and prints paragraph starts and the summed
  total; it exits 1 and names the source line when timing is incomplete.
- `--words` adds every word/comment/pause to that dump.
- `--check` opens the camera and microphone for one second each and exits
  non-zero on failure.
- `--size` must be an even `WIDTHxHEIGHT`; `1280x720` is the recording default.
- `--output` selects the Wayland connector for the fullscreen window (default
  `DP-2`, the local portrait monitor). If the connector is absent the
  compositor chooses. A purely windowed surface cannot be positioned by a
  Wayland client, so use fullscreen/`--output` when the monitor matters.

There is intentionally **no `--duration`**: the total is the sum of the
authored word times.

### Development switches

These exist for automated verification and are not part of the user contract:

```
--capture out.ppm   render 6 frames, write the last one as a PPM, exit
--seek SECONDS      start the read at a clock position
--autoplay          start scrolling immediately
--record-test N     record N seconds, finalize, exit
--demo-help         open the key map on the first frame
--demo-picker       open the built-in file picker on the first frame
```

## The score

See [`prototype/skills.md`](prototype/skills.md) for the full authoring guide.
The short form:

```text
{warm: Welcome}[0.45]Welcome [0.35]to [0.30]your [0.35]teleprompter.

[1.0] [smile — a comment is shown and timed like everything else.]

[0.30]Every [0.30]displayed [0.30]word [0.30]needs [0.30]a time.
```

- `[seconds]` before a word/comment is its duration; the prefix is stripped.
- A paragraph containing only mood tags is a header and needs no time.
- `[2.0]` alone is a silent beat.
- `[mm:ss]` anchors, zero durations, and bare comments are hard errors.
- `{mood}`, `{mood: Theme}`, and `{#rrggbb}` persist until changed.
- `**bold**` and `*italic*` still work.

## Keys

| Key | Action |
|---|---|
| Space | Start / pause; at the end, restart from the top |
| R | Start / stop recording |
| Ctrl+R | Return to the library (refresh it there) |
| Ctrl+O | Open another script |
| F / F11 | Toggle fullscreen; Esc leaves it |
| Up / Down | Move the reading line by 2 % (Shift+Up/Down is the same) |
| C | Cycle the reading line (15 / 25 / 35 / 50 %) |
| A | Centred / flush-left text |
| K | Word-by-word karaoke fill |
| M | Mood colours |
| Left / Right | Nudge the scroll position |
| Home / End | Jump to the top / bottom |
| F5 or L | Reload the current script |
| H | Show / hide the HUD and section timer |
| ? or F1 | Show the key map |
| Ctrl+Q | Quit, finalizing any recording first |

## What is in here

```
core/                     portable C++17 timing, colours, recents, recorder
gui/                      Vulkan/Wayland window, text layout, karaoke renderer
tests/                    stdlib-style assert tests for the pure core
libs/firstparty/framework/App_shell              submodule
libs/firstparty/framework/Vk_Canvas_Lb_LAW       submodule
assets/fonts/             shipped UI/mono faces
samples/test-script.txt   built-in, read-only demo score
prototype/                the original Python app, samples, tests and docs
docs/                     screenshot, porting notes and roadmap
scripts/                  build / run / verify helpers
```

### Engine use

- **`Vk_Canvas_Lb_LAW`** supplies the renderer, SDF shape pipeline, wayland
  platform backend, RasterFont/MSDF text paths, and the `Canvas` immediate-mode
  primitives.
- **`App_shell`** supplies the `Host`/`AppView` seam, the raw-Wayland host,
  paths, and the log-file bootstrap.
- The nested **`vulkan_font_engine`** (with FreeType and msdfgen) is what turns
  the shipped OTF faces into the per-size glyph cache the renderer uploads.
- The original Python/GTK app is **not linked or run** by the C++ binary.

The app calls small additions to the framework: `Host::setFullscreen()`,
`WindowOptions::output` (Wayland connector selection), a console-restore helper
for CLI modes without a TTY, and an optional extra fd in
`WaylandDisplay::dispatch()` so the 16 ms scroll timer is not paced by the
display's idle timeout. They are recorded as patches in `patches/`; CMake
applies them idempotently to the pinned upstream submodules at configure time.
If you already have the patched commits checked out, pass
`-DTP_APPLY_FRAMEWORK_PATCHES=OFF`.

## Why the port looks different from the GTK app

GTK/Pango did four jobs the Vulkan path has to do explicitly:

1. **Word wrapping and line measurement** — `gui/src/layout.cc` builds lines
   from measured word widths, and records every glyph's line and clock.
2. **Per-character colouring** — Pango attributes become per-glyph vertex
   colours emitted through the same RasterFont atlas.
3. **Widgets** — the HUD, cards, section timer, help card and file picker are
   immediate-mode shapes, text and hit rectangles redrawn each frame.
4. **Scrolling** — a word-knot `Timeline` maps script seconds to pixels; the
   last line lands on the reading line at full weight.

The timing model, parser, section timer, karaoke maths and formatting are direct
ports of the Python code; see `docs/PORTING.md` for the module map and the
behavioural checks.

## Recording

The command is the verified one from the Python app:

```sh
ffmpeg -hide_banner -loglevel error -nostats \
  -f v4l2 -input_format mjpeg -video_size 1280x720 -framerate 30 -i /dev/video0 \
  -f pulse -i default \
  -c:v libx264 -preset veryfast -crf 23 -pix_fmt yuv420p \
  -c:a aac -b:a 160k -movflags +faststart -y OUT.mp4
```

Stop writes `q` to ffmpeg's stdin; the recorder falls back to SIGTERM/SIGKILL
only after timeouts. Takes live in `recordings/` beside the executable, named
`YYYY-MM-DD_HHMMSS.mp4`.

## Tests

```sh
./build/tests/teleprompter_core_tests
./scripts/verify.sh
```

`scripts/verify.sh` additionally builds the bundled Python archive, checks its
test suite, runs `--print-timing` on the sample, compares the totals, and runs
the Vulkan capture on the sample score when a Wayland session is available.

## License

AGPL-3.0-or-later, matching the framework submodules. See `LICENSE`.
