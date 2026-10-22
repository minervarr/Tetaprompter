# Essential requirements

These are the user's answers from the planning session, preserved **verbatim**.
Spelling and wording are intentionally unmodified.

## How literal should “every letter has its own time” be?

This is defined on the script itself; like the app only sums the total time. Who defines how much is the author of the script.

## Should the scroll and spotlight follow individual words, or only paragraph totals?

Word timing, line highlight (Recommended)

## What should happen to [mm:ss] anchors, --duration, ↑/↓ and the duration presets?

I don't get it, like on the teleprompter the user doesn't see that; inside the script is needed to know how much time it needs that word; check previous answers to continue.

## Do you still want a small script header for non-time settings such as reading-line height, alignment, spotlight and mood?

All scripts are going to be detailed, the authors must define how much time every word needs; like also could happen that per word every single word has its own mood/feeling, and with the color help is possible to mimetize better; by that reason I still think is needed a commend, like [smile a moment and them...] also defined by the author, so that as a comment is not counted on the amount of time needed, but will be shown to help the client while reading the script.

---

# Design derived from those answers (not verbatim)

- The script is the score. The author writes every displayed word's and every comment's
  duration in seconds.
- The app strips the annotations, displays only the words/comments/colours, sums the
  durations, and shows that sum as the read total.
- Word timing drives scrolling at word boundaries, so a long `Ahhhhh` gets the time the
  author gives it.
- Karaoke is word-level for spoken text: unread words are grey, the active word fills
  from grey to its final colour over exactly its duration, and finished words stay lit.
  Comments are a separate amber, italic annotation style and brighten uniformly over
  their own beat; they never look like lyrics.
- Moods/feelings are per word and rendered as colours.
- Every displayed token needs a positive time. `[1.5] [comment]` is shown and costs
  1.5 seconds; bare `[comment]` is rejected; `[2.0]` alone is a silent 2-second pause.
- `[mm:ss]` anchors, `--duration`, the duration presets and the duration arrow bindings
  are removed. The HUD total is calculated, not configured.

## Update 2026-10-22 (later user decision)

The user tightened the contract after seeing the first timed build:

- The app must refuse to run unless **everything** displayed is detailed with a positive
  `[seconds]` time. There is no estimated fallback, no `est` marker and no `~` total.
- Comments are still not spoken, but they are visible, so they need their own positive
  time and it counts toward the total.
- The old line-level spotlight was replaced by Apple-Music-style word karaoke: spoken
  words start grey and the current word fills left-to-right from zero to full brightness.
- Bracketed comments are deliberately not styled like lyrics: they are amber and italic,
  with a chip behind inline comments and a smaller monospace card with an amber left rule
  for comment-only paragraphs. They brighten uniformly over their own beat; `M` does not
  recolour them. This keeps stage directions obviously different from readable text.
- The 1.5-second timed comment in the bundled sample was being highlighted for only about
  0.78 s because wrapped continuation bands received synthetic start times. The word clock
  is now the sole authority for the comment ramp, so the comment owns exactly 1.5 s.
- Standalone comments and silent beats are **sections**. While one is active, a top-centre
  card shows its title, remaining time, total time and progress. Normal spoken text and
  inline comments do not trigger it. This gives the presenter a dedicated timer for long
  waits without changing the script format.

## Proposed syntax

```text
{warm}[0.45]Good [0.30]morning, [0.55]everyone!
[0.35]It's [0.20]so [0.50]wonderful [0.25]to [0.20]see [0.25]you [0.25]all [0.45]today.

[1.0] [smile and let the room settle]

{somber}[0.40]But [0.30]here [0.50]is [0.25]the [0.45]thing.

[1.5] [look at the camera]

{bright}[0.35]Thank [0.30]you.
```

| Syntax | Meaning |
|---|---|
| `[0.45]word` | 0.45 seconds for the following word |
| `{warm}[0.45]word` | the word is warm-coloured and takes 0.45 seconds |
| `{warm}` / `{warm: Opening}` | mood/feeling from here on; optionally names a theme |
| `{#ffcc00}` | custom colour |
| `[1.0] [smile and nod]` | comment: shown dimmed and italic, 1.0 second counted |
| `[1.5] [look at the camera]` | timed comment: shown, 1.5 seconds counted |
| `[2.0]` | timed silence: 2 seconds counted |

Every displayed word and comment needs a positive explicit duration. Missing or zero
timing is a hard error with the source line; nothing is estimated or rescaled.

---

Progress, current limitations and next steps: see [`ROADMAP.md`](ROADMAP.md).
