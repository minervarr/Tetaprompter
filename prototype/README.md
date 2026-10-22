# Teleprompter

A no-frills fullscreen teleprompter for Linux that records you with the webcam and
microphone while you read. The script appears on your screen only — the recording
contains just you, clean, with sound.

The script is a **strict score**: the author writes how long every displayed word and
comment takes, and optionally the mood/feeling of every word. The app strips those
annotations, sums them, and uses that sum as the read total. There is no target duration
to set, and a score with missing timing does not open.

To write or prompt a new score, start with [`skills.md`](skills.md).

## Requirements

- Linux with GTK4 and PyGObject (`sudo pacman -S python-gobject` on Arch)
- `ffmpeg` (`sudo pacman -S ffmpeg` on Arch)

No pip, no npm, no build step.

## Run

```sh
python3 teleprompter.py                       # open the script library
python3 teleprompter.py my-script.txt         # open that script directly
```

Useful options:

```sh
python3 teleprompter.py --line 50             # reading line at mid-screen
python3 teleprompter.py --windowed            # keep the prompter windowed
python3 teleprompter.py --device /dev/video2  # pick another camera
python3 teleprompter.py --size 1920x1080      # higher resolution recording
python3 teleprompter.py --print-timing samples/test-script.txt
python3 teleprompter.py --print-timing --words samples/test-script.txt
python3 teleprompter.py --check               # camera + mic test, then exit
```

There is deliberately **no `--duration`**: the total is calculated from the script's
word and comment times. `--print-timing` is the way to audit that total before a take;
it exits non-zero and names the source line when timing is incomplete.

`--check` opens the camera and the microphone for one second each — the camera light
blinks briefly — and reports whether they work, so a busy camera or a dead microphone
turns up before a take instead of after it. Exit code is non-zero if anything failed.

`--size` must be `WIDTHxHEIGHT` with even numbers (H.264 needs them); `1280x720` is the
default.

## The script library

Starting the app without a path opens a windowed **script library**:

- **Recent scripts** — every script you have opened, newest first, with its calculated
  read time, folder, and when it was last opened. A script with incomplete timing says
  `needs timing`; opening it shows the offending line instead of switching pages.
- **Open script…** — the file picker, filtered to `*.txt`, `*.md` and `*.text`.
- **Test script** — a built-in demo score, always below the recents, showing its total
  read time. It is read-only and shipped inside the app, so it never touches your own
  files.

Choosing a valid script switches to the fullscreen prompter. **Ctrl+R** returns to the library
(pausing the read and leaving fullscreen); **Ctrl+O** opens the file picker from either
screen. Recording must be stopped before switching scripts.

The recent list is a small cache at
`$XDG_STATE_HOME/teleprompter/recents.json` (usually
`~/.local/state/teleprompter/recents.json`). Delete that file to start with an empty
list. On the very first launch, an existing `script.txt` is listed once so it is easy to
find again.

Edit a script in any editor while the app is open, then press **F5** (or **L**) to
reload it. If the edit introduces missing or zero timing, the reload is refused and the
previous valid score stays on screen.

## Single-file build

The whole app also packs into one executable archive, with nothing beyond the
requirements above:

```sh
python3 tools/build_pyz.py      # writes teleprompter.pyz in the repo root
./teleprompter.pyz              # run it from anywhere
python3 tools/build_pyz.py --run      # build, then start it
python3 tools/build_pyz.py --force    # rebuild even when nothing changed
```

The built-in test score is embedded in the archive. Your own `script.txt` and the
`recordings/` folder live beside the `.pyz`, not inside it: drop the file in a folder and
your script and takes stay next to it. Passing a script path still works
(`./teleprompter.pyz my-script.txt`), and the recent list follows you between the source
and the archive because it is stored in XDG state, not next to the app.

## Keys

| Key | Action |
|---|---|
| **Space** | Start / pause scrolling (at the end, starts over from the top) |
| **R** | Start / stop recording |
| **Ctrl+R** | Return to the script library (re-reads the list there) |
| **Ctrl+O** | Open another script from disk |
| **F** / **F11** | Toggle fullscreen; **Esc** leaves fullscreen |
| **↑ / ↓** | Move the reading line up / down by 2 % |
| **C** | Cycle the reading line through its presets (15 / 25 / 35 / 50 %) |
| **← / →** | Nudge the text up / down |
| **Home / End** | Jump to the top / bottom |
| **F5** or **L** | Reload the script file |
| **H** | Show / hide the toolbar and section timer |
| **A** | Centre the script text / return to a flush left edge |
| **K** | Toggle word-by-word karaoke fill |
| **M** | Toggle mood colours |
| **?** or **F1** | Show the key map card (any key or click closes it) |
| **Ctrl+Q** | Quit (any running recording is finalized first) |

## While you read

The toolbar across the top keeps you oriented without shouting:

- A **Scripts** button on the left returns to the library, next to the Record button.
- A state chip shows where the read is: **Ready → Playing → Paused → Done**.
- When the script names a theme, a second chip shows the mood, the theme and the time
  left in that theme.
- The **total badge** in the top-right is calculated from the script's word times. It is
  read-only.
- The **countdown** next to it turns amber under 30 seconds and red under 10, so the end
  of the read never surprises you.
- When a standalone comment or silent beat is active, a **section card** appears at the
  top centre: it names the directive and shows `0:32 left of 0:45` with an amber progress
  bar. It stays visible when the HUD dims, and disappears on the next spoken paragraph.
  Inline comments do not start a section card.
- A **hairline progress bar** along the very bottom edge tracks how much of the script is
  behind you; it fills exactly with the clock, not with the pixel distance.
- While the script plays, the toolbar **fades back** after a few seconds so nothing pulls
  your eyes off the lens. Move the mouse (or press Space to pause) to bring it back in
  full; **H** still hides it completely.
- While recording, a **red frame** rounds the screen, the dot next to the timer blinks and
  the button turns red — there is no mistaking a live take.

Press **?** (or **F1**) any time and the full key map appears in the middle of the screen.

## The script is the score

The author writes a `[seconds]` prefix in front of every displayed word and comment. The
prefix is stripped before display; the app only sums the numbers:

```text
{warm}[0.45]Good [0.30]morning, [0.55]everyone!
[0.35]It's [0.20]so [0.50]wonderful [0.25]to [0.20]see [0.25]you [0.25]all [0.45]today.

[1.0] [smile and let the room settle]

{somber}[0.40]But [0.30]here [0.50]is [0.25]the [0.45]thing.

[1.5] [look at the camera]

{bright}[0.35]Thank [0.30]you.
```

The presenter sees only:

```text
Good morning, everyone!
It's so wonderful to see you all today.

[smile and let the room settle]

But here is the thing.

[look at the camera]

Thank you.
```

The total above is about **8.8 s**, so the badge shows `0:09`.

| Syntax | Meaning |
|---|---|
| `[0.45]word` | 0.45 seconds for the following word |
| `{warm}[0.45]word` | the word is warm-coloured and takes 0.45 seconds |
| `{warm}` / `{warm: Opening}` | mood/feeling from here on; optionally names a theme |
| `{#ffcc00}` | custom colour, if the built-in moods are not enough |
| `[1.0] [smile and nod]` | amber annotation with a chip; **1.0 second** counts |
| `[1.5] [look at the camera]` | timed annotation; **1.5 seconds** counts |
| `[2.0]` | timed silence: 2 seconds, nothing displayed |

Rules worth knowing:

- The mood and theme **persist** across words and paragraphs until the next tag.
- You can change the mood between words: `{warm}[0.2]One {somber}[0.3]Two.`
- A paragraph containing only mood tags is a header; it sets the state for the next
  paragraph and displays nothing.
- **Every displayed word and every comment needs a positive `[seconds]` prefix.**
  There is no estimated fallback: a score with missing or zero timing is refused with
  the source line that needs attention.
- A comment can be inline (`[0.4]Hi [0.3][smile] [0.3]there.`) or on its own paragraph.
  It is italic, and its authored time counts toward the read. A standalone comment is
  also a **section**: its top-centre timer shows the authored start, remaining and total.
- A silent beat is a number with no text: `[2.0]` on its own. Silent beats are sections
  too, so a long hold gets the same countdown; inline comments do not.
- Old `[mm:ss]` anchors are rejected. A paragraph like `[Pause for 10 seconds.]` is an
  untimed comment and is rejected too; write `[10.0] [Pause for 10 seconds.]` or
  `[10.0]` alone.
- `**bold**` and `*italic*` still work in the displayed text. The timing prefixes are
  separate from that markup.

## Timing and scrolling

The total is simply `Σ token seconds`. Nothing is rescaled, stretched or fitted to a
preset. The badge shows the sum, and the countdown counts down from it.

Timing is applied at **word boundaries**. Several words can share one visual line; the
line holds its position for the sum of those words, so a long `Ahhhhh` gets exactly the
time the author gave it.

Audit a script before a take:

```sh
python3 teleprompter.py --print-timing                 # per-paragraph starts + total
python3 teleprompter.py --print-timing --words         # every word and its time
python3 teleprompter.py --print-timing other.txt
```

If any displayed word or comment is missing a positive `[seconds]` prefix, the command
prints the offending source line and exits non-zero. The app will not open that score.

## Where to look: the reading line

The script sits in a column centred horizontally, at most two thirds of the screen wide.
Centring keeps the words you are reading near the middle of the screen — where the camera
is in both setups — and the limited width keeps lines a comfortable length instead of
running from edge to edge across a TV. Each line is centred inside the column too, so
paragraphs sit symmetrically around the middle of the screen. Press **A** for a flush
left edge instead — some readers find it easier to re-anchor on the same spot at the
start of every line.

Two amber bars sit level with each other at the left and right edges of the screen. They
live in the side margins, clear of the text column, and they mark the **reading line**:
the height your eyes should rest at — which is where the camera lens should be too. The
script is padded around it: the first line starts on the line, and the words you speak
cross it exactly in step with their author-given times, the last line arriving on it as
the total expires.

- **C** cycles between the common positions: 15 %, 25 %, 35 % and 50 % from the top.
- **↑ / ↓** fine-tune it by 2 % at a time (5 % – 60 %).
- Start at a position straight away with `--line`: use roughly **15–25 %** when the
  camera sits above the screen, and **50 %** when it is behind the screen's centre.

The `─ 25%` badge in the toolbar shows the current position and is clickable. The total
badge next to it is read-only.

## Word-by-word karaoke and mood

Spoken text starts as readable grey. As each word becomes due, it fills from left to
right over exactly its authored duration, ending at full brightness. Finished words stay
lit; words still ahead stay grey. It is the same idea as a lyrics view: grey first, then
the current word fills in, then the next one.

**Bracketed comments are deliberately not lyrics.** They are always amber and italic,
so they never look like something to say. An inline comment gets a quiet amber chip
behind it; a comment on its own paragraph becomes a smaller monospace callout with an
amber rule down its left side. A comment brightens evenly across its own beat instead of
filling word by word. Press **K** to turn the spoken-word karaoke off and show the whole
script at full brightness; comments remain annotations. Press **M** to turn the mood
tint off: spoken words still fill, but finish white; comments stay amber.

A **mood tag** at the start of a word tints it from there on:

| tag | reads as | tint |
|---|---|---|
| `{somber}` | heavy, restrained | slate blue |
| `{quiet}` | low, reflective | cool grey |
| `{plain}` | neutral (default) | white |
| `{warm}` | friendly, animated | ivory |
| `{bright}` | playful, energetic | pale gold |

The tint is deliberately near-grayscale: hue carries the mood, brightness carries the
karaoke progress. A custom `{#rrggbb}` tag gives you any colour you want.

`{warm: Opening}` also starts a **theme**. The small chip next to the state then reads
`warm · Opening · 0:18` — the mood, the theme, and the time left in it. Moods and themes
can change between words.

Unknown tags such as `{happy}` are shown literally and reported in the toolbar, so they
never silently change what you read.

## Recordings

Press **R** to start and stop. Files land in `recordings/` as `YYYY-MM-DD_HHMMSS.mp4`
(H.264 video + AAC audio, 720p30 by default), ready to upload or edit. The camera is
only active while recording.

Stopping a take takes a moment: ffmpeg has to flush the file and move its index to the
front for instant playback. The button says *Finalizing…* until that is done, and only
then does it become the Record button again — you cannot start the next take into a
still-open file, and the saved-file message tells you the name and size when it is
safely on disk.

Run `python3 teleprompter.py --check` first if you want to be sure the camera and the
microphone are free before you sit down to read.

## Tests

The parsing and scroll math is unit-tested with the standard library only:

```sh
python3 -m unittest discover -s tests -v
```

It needs no display and no camera — it covers strict word/comment timing, comments and
silent beats, mood tags, UTF-8 byte offsets, the karaoke fill math, markup, argument
validation, the recent-script cache and the weight ↔ pixel timelines.

## Troubleshooting

- **The total is larger than expected** — check `--print-timing --words`; every comment
  and silent beat also counts. Shorten their times if the total matters.
- **A comment is rejected for missing timing** — comments are displayed, so they need
  their own positive prefix: `[1.0] [smile]`, never bare `[smile]`.
- **A `[Pause for 10 seconds.]` is rejected** — old directions are untimed comments now.
  Use `[10.0] [Pause for 10 seconds.]` or `[10.0]` alone.
- **A script refuses to open** — run `--print-timing` on it; the error names the source
  line with the missing or zero `[seconds]` prefix.
- **A `{tag}` is read aloud** — it is not one of the five moods and is not a `#rrggbb`
  colour, or it is not at the start of a word. Unknown tags are shown literally and
  reported in the toolbar.
- **Wrong microphone** — list sources with `pactl list short sources`, then run
  `python3 teleprompter.py --audio <source-name>`.
- **List cameras** — `v4l2-ctl --list-devices`.
- **A script vanished from the recent list** — files that no longer exist are pruned.
  Open it again from the picker, or press **Ctrl+R** on the library to refresh.
- **Reset the recent list** — delete `~/.local/state/teleprompter/recents.json` (or
  `$XDG_STATE_HOME/teleprompter/recents.json`).
- **The test script cannot be found in a source checkout** — keep
  `samples/test-script.txt` beside `teleprompter.py`; the `.pyz` build embeds it.
- **Window too small / text too big** — the font size is based on the largest monitor's
  height and is set at startup.
- **`Gdk-WARNING ... org.freedesktop.portal.Inhibit` when launched from a terminal** —
  should not appear: the app disables GDK's portal lookups (`GDK_DEBUG=no-portals`)
  because on Wayland it inhibits idle through the compositor's native protocol. If
  another GTK app prints it, it is harmless; GTK just cannot find the portal interface.

## Repository layout

```
teleprompter.py     the whole app (single file, by design)
script.txt          your working score
samples/            scores shipped with the app (embedded in the .pyz)
skills.md           score-writing guide for authors and models
tests/              stdlib unittest suite, no display needed
tools/build_pyz.py  builds the single-file archive
docs/               requirements and roadmap
recordings/         output mp4s, created on demand
```

The build tool always writes `teleprompter.pyz` to the repository root. Your own
scripts and takes are never placed inside the archive.
