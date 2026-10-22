# Porting notes — Python/GTK4 → C++17/Vulkan

The original app is `prototype/teleprompter.py` (2,931 lines, GTK4 +
PyGObject). This port keeps its **behaviour and data model** and replaces the
rendering/input stack.

## Module map

| Python | C++ |
|---|---|
| `paragraph_plan`, `_parse_tokens`, `_finish_paragraph` | `core/src/script.cc` |
| `Timeline`, `rebuild_entries`, `pos_from_weight` | `core/include/teleprompter/script.hh` |
| `mood_colour`, `comment_colour`, `word_fill`, `character_fill`, `format_time`, `format_age` | `core/src/script.cc` |
| `load_recents`, `save_recents`, `remember_recent` | `core/src/recents.cc` |
| `Recorder`, `build_ffmpeg_cmd`, `check_devices`, `pick_error_line` | `core/src/recorder.cc` |
| `section_state`, `section_title` | `core/src/script.cc` |
| Pango label wrapping/measurement | `gui/src/layout.cc` |
| Pango per-character `attr_foreground` | `gui/src/text.cc` (`TextPainter`) |
| GTK HUD, cards, section card, help, library | `gui/src/app.cc` immediate-mode draw |
| GTK `ScrolledWindow` + adjustment | `Timeline` + `scroll_` + `draw_script` |
| GTK FileDialog | built-in picker in `gui/src/app.cc` |
| font selection | `gui/src/fonts.{hh,cc}` + `TextPainter::reload()` |
| font preference | `core/src/settings.cc` (`settings.json`) |
| `Gtk.Application.inhibit` (IDLE) | `Host::setKeepAwake()` |
| `Gtk.ApplicationWindow` fullscreen | `Host::setFullscreen()` (App_shell addition) |

## Deliberate differences

- **The file picker is built in.** Wayland clients cannot show a native file
  dialog without xdg-desktop-portal, and this app has no portal dependency. The
  picker lists directories and `.txt` / `.md` / `.text` files with the same
  keyboard spirit (arrows, Enter, Backspace, Esc).
- **Window title is static.** App_shell's Wayland host does not currently expose
  `setWindowTitle`; the compositor still gets the app id and the title
  `Teleprompter`.
- **No GTK icon theme.** Buttons use text/simple shapes rather than symbolic
  icons. The HUD remains recognisable and the behavior is unchanged.
- **Fonts are explicit presets.** New Computer Modern Sans is the default;
  OpenDyslexic is a second preset with a 0.70 size factor and New Computer
  Modern fallbacks for the em dash, ellipsis and arrows it lacks. The selected
  face applies to every screen, including stage directions and comments.
  `T`/the library button/`--font` choose it, and the choice is remembered.
- **Text quality stays on RasterFont.** It rasterizes the actual Bézier outlines
  per size (8-bit coverage, subpixel phases), which is the highest-quality path
  for this fixed-size UI. MTSDF is not used because it would be softer at HUD
  sizes; the engine still has it for consumers that need continuous scaling.
- **No Pango markup re-parse at draw time.** `**bold**` / `*italic*` are turned
  into per-word style flags while parsing; the plain display text and byte
  offsets remain the authority.
- **Direction cards are left-aligned inside the card**, centred in the column.
  The Python label was centred; this is a readability choice for long stage
  directions and is the only intentional visual difference in the script area.
- **Recording lives beside the executable**, matching the `.pyz` rule, not
  beside the Python source tree.
- **Fullscreen follows the compositor's current output by default.** Pass
  `--output CONNECTOR` to pin a specific monitor (`DP-2`, `HDMI-A-1`, …). A
  windowed Wayland surface cannot be positioned by the client, so `--windowed`
  leaves placement to the compositor.
- **The initial library is fullscreen by default** on the compositor's current
  output; `--output` pins a monitor and `--windowed` restores a windowed start.
- **Log file** lives at `$HOME/.local/state/teleprompter/teleprompter.log`, the
  same state directory as recents. CLI modes (`--print-timing`, `--check`,
  `--help`) restore the original stdout/stderr so scripts and pipes see output.

## Invariants carried over

- `Σ paragraph.seconds == timeline.total_weight == duration`.
- Every displayed word/comment has a positive time; errors carry source lines.
- A line with several words repeats a timeline knot; the line dwells for the
  sum of those words.
- First line centre is at the reading line at weight 0; the last line centre is
  at the reading line at full weight.
- Karaoke fill duration is the authored word duration, not a synthetic band.
- Comments use their own amber ramp and never mood colours.
- The total badge is read-only; the countdown is derived from the summed total.
- The script text is never in the camera recording: ffmpeg reads `/dev/video0`
  directly and the Vulkan surface is a different device.

## Framework changes

Two small App_shell additions were needed and are committed in the submodule:

1. `Host::setFullscreen(bool)` and its Wayland implementation, so the library
   can start windowed and the prompter can go fullscreen without destroying the
   surface/Renderer.
2. `app_shell_restore_console()`, called for CLI modes after the bootstrap has
   redirected stdout/stderr to the log file. `--print-timing` and `--check` work
   from a pipe or CI.

Patches are in `patches/`; the submodule pins the commits used by this build.

## Verification performed

- `teleprompter_core_tests`: strict timing errors/source lines, UTF-8
  character and byte offsets, comments/silent beats, sections, mood persistence,
  markup, formatting, timeline knots, recents round-trip, sample total `0:23`.
- `--print-timing` on the sample: 8 paragraphs, 64 words, 2 comments, 22.5 s.
- Vulkan capture at 1180×760 and fullscreen 1920×1080: library, prompter,
  stable README screenshot, section card at 8 s, end state at 22.5 s, inline
  comment chip, bold/italic, key-map and picker cards.
- Both font presets captured on DP-2 (1080×1920): New Computer Modern and
  OpenDyslexic, including em dash / ellipsis / arrow fallback, the font menu,
  inline comment chip and a saved `settings.json` selection.
- `--check`: camera `/dev/video0` + microphone `default` OK.
- `--record-test 3`: produced a 1280×720 H.264/AAC MP4, `ffprobe` verified the
  video stream at 30/1 and an AAC 48 kHz stereo stream. Audio startup latency on
  the first second is a PipeWire source property, not a transcriber bug.
- Python prototype: 73 unit tests pass; its `.pyz` still builds and prints the
  same `0:23` total.
