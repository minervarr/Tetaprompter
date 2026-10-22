# ROADMAP

Progress and next steps for the word-timed teleprompter score.
Last updated: 2026-10-22.

The requirements themselves are in `requirements.md` (verbatim + design). This file
records what has been built, what is deliberately left out, and what to do next.

## Done

### Script format

- The script is a **strict score**: every displayed word and comment carries its own
  positive `[seconds]` prefix. Missing, zero or legacy anchor timing is a hard error
  with the offending source line.
- `{mood}` / `{mood: Theme}` tags persist across words and paragraphs and can change
  between words. `{#rrggbb}` sets an exact colour.
- Bracketed text is a **comment** and has its own visual language: amber, italic, a
  chip behind inline comments, and a smaller monospace card with an amber left rule for
  comment-only paragraphs. It is timed like every other visible token; `[1.5] [comment]`
  owns 1.5 seconds, and a bare `[comment]` is rejected.
- `[2.0]` alone is a silent beat.
- Old `[mm:ss]` anchors are rejected. Old `**[Do this.]**` / `*[Do this.]*` comments
  still work when preceded by a positive time.
- There is no estimate and no fallback. A plain-text script does not open until timed.
- The bundled test script is fully timed (including the line-6 comment).

### Timing and scrolling

- `paragraph_plan(text)` sums explicit token durations. There is no duration argument,
  no rescaling, no anchors and no `--duration`.
- The HUD total is read-only and equals `Σ token seconds`.
- Word knots drive the scroll timeline via `Timeline.rebuild_entries()`; several
  words on one visual line share a position, so the line dwells for their sum.
- Karaoke is **word-level for spoken text**: unread words are neutral grey, the active
  word fills left-to-right from grey to its lit colour over exactly its duration, and
  finished words stay lit. `K` toggles it, `M` toggles the mood tint for spoken words.
- Comments do not look like lyrics: they are amber annotations with a uniform
  dim-to-bright ramp over their own clock, so the sample's `[1.5] [look …]` owns exactly
  1.5 seconds without competing with the spoken karaoke. Inline comments get a chip;
  standalone directions get a monospace card.
- `↑ / ↓` fine-tune the reading line; duration keys and presets are gone.

### UI / diagnostics

- The HUD duration button became a read-only total label.
- `--print-timing` prints starts and the summed total; on incomplete timing it prints
  every issue with its source line and exits non-zero. `--print-timing --words` lists
  every word/comment/pause with its clock.
- Library cards show `needs timing` instead of a read time for invalid scores; opening
  one reports the error and leaves the current page in place.
- A top-centre **section card** follows a standalone comment or silent beat: title,
  `0:32 left of 0:45`, and an amber progress bar. It is outside the HUD dimming, hides on
  spoken paragraphs/inline comments/end, and follows `H`.
- `--check` still verifies camera and microphone, unchanged.
- Recording still uses the verified ffmpeg command (v4l2 + PulseAudio → H.264/AAC);
  the script is never part of the recording.

### Verification completed

- `python3 -m py_compile teleprompter.py`
- `python3 -m unittest discover -s tests -q` — 73 tests pass.
- `python3 teleprompter.py --print-timing samples/test-script.txt` — 8 paragraphs,
  64 words, 2 comments, total `0:23`; the timed comment owns 1.5 s.
- `python3 teleprompter.py --print-timing script.txt` — exits 1 with line-numbered
  missing timing for the legacy untimed script.
- `python3 teleprompter.py --check` — camera and mic OK.
- Live-window probes on DP-2: amber comment card and inline chip, uniform comment
  brightness at 0/50/100 %, spoken per-character fill, UTF-8 byte offsets after an em
  dash, `K`/`M` toggles, seek/restart repaints, unchanged adjustment upper after paints,
  duration == total weight. Section card at the 45 s `icpnaDemo.txt` wait: `0:45 left of
  0:45`, `0:22 left of 0:45` at the midpoint with a half bar, and hidden on the next
  spoken paragraph.
- A 2-second recording finalized correctly; `ffprobe` reported h264 1280x720 30/1
  and aac.
- `python3 tools/build_pyz.py --force && ./teleprompter.pyz --print-timing samples/test-script.txt`.

### Documentation

- `README.md` rewritten for strict timing and word karaoke.
- `skills.md` is the single canonical authoring contract + prompt.
- `AGENTS.md` behaviour contract and architecture notes updated.
- `docs/requirements.md` holds the verbatim user requirements plus the 2026-10-22
  strictness/karaoke decision.

## Deliberately out of scope / current limitations

- **No camera preview in the UI.** This is still a product decision. A black app
  window is the normal teleprompter background.
- **`--check` cannot detect a covered lens.** A sticker on the camera still produces
  frames, so `--check` reports OK and the recording is black.
- **No script file picker or capture-device GUI yet.** A different script, camera,
  microphone, resolution or fps still needs the positional path or CLI flags.
- **No non-time script header.** Reading line, alignment, karaoke and mood toggles
  remain GUI/CLI settings; only timing, mood tags and comments live in the script.
- **`script.txt` is not migrated.** The checked-in lesson script is legacy untimed text
  and is now rejected; it needs every word and every old `[Pause for N seconds.]`
  direction timed before it will open.
- **The scroll is continuous, not lyric-style line snapping.** A timed comment owns its
  full clock for the amber ramp, but the line still travels toward the reading line during
  that time. There is no freeze-frame hold.
- **Comments are not spoken, but they now cost time.** A comment is a visible stage
  direction; give it the beat it needs.

## Next steps (in suggested order)

1. **Camera black-frame warning.** Add a `--snapshot` command (one JPEG) and/or make
   `--check` sample a frame and warn when average brightness is near zero: “camera is
   producing a black image — lens covered?”. Keep the no-preview-in-the-UI rule.
2. **Capture settings in the GUI.** A small settings popover for device/audio/size/fps
   (or optional script metadata if the user decides hardware belongs in the score).
3. **Optional non-time header.** If wanted: `[cue:25]`, `[align:left]`,
   `[karaoke:off]`, `[mood:off]` at the top. Do not put timing or duration there.
4. **Migrate `script.txt`.** Hand-time the existing lesson script or add an authoring
   helper that annotates plain text with first-pass `[seconds]` prefixes; the author
   then adjusts them. It cannot open until this is done.
5. **Dwell tuning.** If the continuous scroll makes short single-word/comment lines
   hard to read, consider a small lyric-style transition at line boundaries. Keep the
   clock authoritative.

## Do not regress

- `Σ paragraph.seconds == timeline.total_weight == window.duration`.
- Every displayed word/comment has a positive explicit time; invalid scores are refused
  with source lines and `--print-timing` exits non-zero.
- The first line starts exactly on the reading line; the last line ends on it.
- `Timeline.rebuild_entries()` allows repeated positions for words sharing a line,
  and positions remain monotone.
- Pango ranges are **UTF-8 byte offsets**, never Python character offsets.
- `_paint_word()` only sets foreground/background/italic attributes — never bold or font
  size on spoken words, because those reflow the text and move the measured lines.
- Spoken words start grey and fill to their mood colour; comments stay amber and never
  look like lyrics. `K` off shows full brightness; `M` off makes spoken fills white.
- A timed comment such as the sample's 1.5 s `[look …]` must own exactly its clock with
  the uniform amber ramp.
- A section timer appears only for `is_direction` paragraphs (standalone comments and
  silent beats), never for inline comments or spoken paragraphs. It uses the paragraph
  clock, stays outside HUD dimming, and hides with `H`.
- The HUD total is calculated, not configured. There is no `--duration`, no preset
  cycling and no anchor parsing.
- The recording path stays `ffmpeg` as a subprocess; the script text never enters
  the recorded stream.
