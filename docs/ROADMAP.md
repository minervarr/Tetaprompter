# Roadmap

The C++ port is feature-complete against the current Python behaviour contract
for the score, scrolling, karaoke, HUD, sections, library, fullscreen and
recording. The remaining work is polish and platform reach.

## Done in this port

- Strict timed-score parser with UTF-8 offsets, comments, silent beats, moods,
  themes and source-line errors.
- Word-knot timeline, reading line, karaoke fill, comment chips/cards, section
  timer, HUD dimming, countdown and progress hairline.
- Windowed library with recents, read times, `needs timing` cards, test script,
  built-in picker, Ctrl+R/Ctrl+O/F5/L reload paths.
- Vulkan rendering through Vk_Canvas + RasterFont, fullscreen switching through
  App_shell, idle inhibition, clean ffmpeg recording and `--check`.
- Pure core tests, sample/Python parity checks and screenshot captures.

## Next polish

1. **Window title per script.** Add `Host::setWindowTitle()` + Wayland
   `xdg_toplevel_set_title`, then show the filename / "Test script".
2. **Native file picker.** Optional xdg-desktop-portal `FileChooser` path behind
   a runtime probe; keep the built-in picker as the fallback.
3. **Pointer drag seeking.** Wheel scrubbing exists; press-drag on the script
   area would match a touchpad better.
4. **HiDPI verification.** The font scale is monitor-height based; test a 2×
   buffer-scale output and a portrait display.
5. **Audio pre-roll.** PipeWire's source startup can miss the first fraction of
   a very short take; a short pre-roll or `-thread_queue_size` experiment would
   make 1–2 s test recordings carry audio.
6. **Framework upstreaming.** Push the App_shell fullscreen/console patches and
   keep the pinned submodule SHAs current.
7. **Packaging.** Provide an Arch PKGBUILD/desktop file that installs the
   executable, assets, fonts, sample and a `.desktop` entry with the same app id.

## Explicit non-goals

- No camera preview.
- No script compositing into the recording.
- No duration resampling, estimates or `--duration`.
- No user-facing config file; only the disposable recents cache.
- No second script format or plugin system.
