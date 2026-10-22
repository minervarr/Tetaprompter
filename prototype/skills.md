---
name: teleprompter-script
description: 'Write or edit a strict teleprompter score where every displayed word and comment carries a positive duration in seconds, moods are coloured per word, and the app refuses to run anything with missing timing. Use when someone asks for a teleprompter script, a timed read, a script for an exact total, or a prompt that produces one.'
---

# Writing a teleprompter score

This is the authoring contract for this repository's teleprompter
(`teleprompter.py` + `samples/test-script.txt`). The script is a **score**, not a
plain text file with a target duration.

**Every displayed token must have an explicit positive `[seconds]` prefix.**
That includes spoken words and every comment or stage direction. There is no
estimated fallback, and the app refuses to open a score with incomplete timing.
The total is exactly the sum of the prefixes.

The bundled `samples/test-script.txt` is a fully timed example. The checked-in
`script.txt` is legacy untimed text and is refused until every token is timed.

## The file format

Plain text, UTF-8, paragraphs separated by **blank lines**. A single newline
inside a paragraph is just wrapping whitespace.

```text
{warm: Opening}[0.45]Good [0.30]morning, [0.55]everyone!

[0.35]This [0.30]is [0.30]the [0.40]second [0.30]thought.
```

The presenter sees `Good morning, everyone!` and the app sums
0.45 + 0.30 + 0.55 = **1.30 seconds** for the first paragraph.

## Words

Put the duration immediately before the word it belongs to:

```text
[0.35]Every [0.25]word [0.30]gets [0.40]its [0.25]own [0.35]time.
```

- A token is a whitespace-separated word, punctuation included.
  `[0.30]morning,` times the whole token, comma and all.
- Seconds may be decimals: `[0.45]`, `[1.2]`, `[0.05]`.
- A held sound can carry a long time: `[1.6]Ahhhhhhhhh!`.
- Repeated words are independent: `[0.2]no [0.4]no [0.8]no!`.
- Every word must be positive. `[0]word` is rejected. A word with no prefix is
  rejected with its source line number.
- A duration prefix left alone at the end of a paragraph is a **silent beat**
  (`[2.0]`) and displays nothing.

## Moods and themes

`{mood}` before a word tints it from there on. `{mood: Theme}` also starts a
named theme. Tags persist across words and paragraphs until the next tag.

```text
{somber}[0.40]But [0.30]here [0.50]is [0.25]the [0.45]thing.
{warm: Opening}[0.35]Thank [0.30]you.
```

| Tag | Reads as | Tint |
|---|---|---|
| `{somber}` | heavy, restrained | slate blue |
| `{quiet}` | low, reflective | cool grey |
| `{plain}` | neutral (default) | white |
| `{warm}` | friendly, animated | ivory |
| `{bright}` | playful, energetic | pale gold |

`{#rrggbb}` gives an exact colour. A paragraph containing only tags is a
**header**: it changes the mood/theme for the next paragraph and displays
nothing. A header does not need a timer because it has no visible content.

Moods describe delivery. On screen, unread **spoken** words are neutral grey;
the active word fills from grey to its mood colour over exactly its duration;
finished words stay in their mood colour. `M` turns the mood colour off for
spoken words (the fill then goes to white) and `K` turns the word-by-word
karaoke off (spoken text is shown at full brightness).

## Comments and stage directions

A bracket is a comment. Comments are **not spoken**, and they are deliberately
not styled like lyrics: they are amber and italic, with a quiet chip behind an
inline comment or a smaller monospace callout with an amber left rule when they
are their own paragraph. A comment brightens evenly across its authored beat
rather than filling word by word. `M` does not change comment colour.

Comments are visible, so they must have their own positive duration and their
time counts toward the read:

```text
[1.0] [smile and let the room settle]

[1.5] [look at the camera and breathe]
```

An inline comment works the same way:

```text
[0.2]Hi [0.3][smile] [0.3]there.
```

There are no free comments. `[smile]` without a number is a timing error. If a
comment is only an author note and should never appear on screen, leave it out
of the score instead of giving it zero time.

A comment on its own paragraph is also a **section**. While it is active, a
top-centre timer shows its title, `0:32 left of 0:45`, and a progress bar. This
makes waits such as `[45.00] [Slide 1 is up…]` easy to monitor without watching
the whole-script countdown. Silent beats are sections too; inline comments are
not.

`[2.0]` alone is a **silent beat**: two seconds pass, nothing is displayed.

## Markup

`**bold**` and `*italic*` are rendered as styled text. Everything else is shown
literally. Do not use headings (`#`), bullets, tables or code fences: they will
be displayed as spoken text.

## How the total works

There is no duration setting and no rescaling:

```text
total = Σ every [seconds] prefix
```

The badge shows that sum and the countdown counts down from it. Timing is applied
at word boundaries. Several words can share one visual line; the line holds its
position for the sum of those words. A comment gets exactly the time written
before it; its amber brightness ramp and its place on screen last exactly that
long.

To audit a score:

```sh
python3 teleprompter.py --print-timing my-talk.txt
python3 teleprompter.py --print-timing --words my-talk.txt
```

`--print-timing` exits non-zero and lists the source line for every missing,
zero or legacy timing. The live window refuses to open an invalid score.

## Budgeting a target duration

Because the app only sums, you own the arithmetic:

1. Draft the script naturally, one paragraph per thought. Put stage directions
   on their own paragraphs.
2. Assign a **relative weight** to every word and every comment:
   roughly `0.25` for a short unstressed word, `0.35` for a normal word,
   `0.5–0.7` for a long or emphasised word, plus `0.2` for a comma and `0.5`
   for a full stop.
3. Decide which comments must hold the screen. Give each one a real positive
   duration; it will be part of the total.
4. Sum the weights. To hit a target, multiply every weight by
   `target_seconds / weight_sum`, then round to two decimals.
5. Add timed silent beats for deliberate pauses (`[1.5]` alone).
6. Verify with `--print-timing` and fix every error before recording.

For reference, calm English is about **2.4–2.8 spoken words per second** when
pauses are included. Comments and silent beats are extra screen time.

## The score-writing prompt

Copy this, fill the braces, and hand it to any model:

> Write a strict timed teleprompter score for a spoken presentation.
>
> **Target:** the script should total about **{DURATION} seconds**. The app does
> not fit or rescale: it sums the per-word and per-comment times you write, so
> your numbers are the duration.
>
> **Format (mandatory):**
> - Plain text, paragraphs separated by blank lines.
> - Every displayed word must be preceded by a positive `[seconds]` prefix,
>   e.g. `[0.35]This [0.25]is [0.30]the [0.45]opening.`
> - Every comment/stage direction must also be preceded by a positive
>   `[seconds]` prefix, e.g. `[1.0] [look at the camera]`. No comment may be
>   untimed or zero-length.
> - Punctuation stays attached to its word: `[0.40]today,`
> - Use `{warm}`, `{somber}`, `{quiet}`, `{plain}` or `{bright}` before a word
>   when the feeling changes. `{mood: Theme}` names a theme. Tags persist.
> - A silent beat is `[2.0]` alone. It displays nothing.
> - No headings, no bullets, no tables, no code fences. `**bold**` and
>   `*italic*` are allowed for emphasis.
> - Do not use `[mm:ss]` anchors; they are rejected.
>
> **Timing:**
> - Default pace: 0.35 s for a normal word, 0.25 for a short unstressed word,
>   0.5–0.7 for a long or emphasised word. Add about 0.2 s to a word ending in a
>   comma and 0.4–0.6 to a word ending a sentence.
> - Comments are visible stage directions and count. Give each one the time it
>   should remain on screen (usually 0.8–2.0 s).
> - Sum every prefix, including comments and silent beats, and scale them so the
>   total is within 3 % of {DURATION} seconds. State the final total in your
>   answer.
>
> **Content:**
> - Topic: {TOPIC}
> - Audience: {AUDIENCE}
> - Tone: {TONE}
> - Shape: a hook, 2–3 concrete points or examples, and a closing line that lands.
> - Write for the ear: concrete nouns, active verbs, no filler, no clichés.
>
> Before finishing, re-add the numbers in a second pass and check that every
> visible token has a positive timer.

## A complete example

```text
{warm: Welcome}[0.45]Welcome [0.35]to [0.30]your [0.35]teleprompter.

[0.35]This [0.30]built-in [0.40]test [0.30]script [0.35]shows [0.30]how [0.25]the [0.30]score [0.40]works.
[0.30]Open [0.30]one [0.25]of [0.25]your [0.30]own [0.35]scripts [0.30]from [0.25]the [0.30]library.

[1.0] [smile — a comment is shown and timed like everything else.]

[1.5] [look at the camera and breathe]

{somber: The turn}[0.40]Now [0.35]the [0.30]mood [0.35]changes [0.30]word [0.30]by [0.25]word.

{bright}[0.35]Every [0.30]word [0.30]and [0.30]comment [0.35]has [0.30]its [0.30]own [0.30]time.
```

## Mistakes that bite

- **Forgetting a prefix.** The score is rejected and the offending source line is
  reported. Never ship a take with incomplete timing.
- **Writing `[smile]` without a number.** Comments are displayed and timed too.
  Use `[1.0] [smile]`.
- **Using `[0:20]` or `[mm:ss]`.** Old anchors are rejected. Write word
  durations instead.
- **Writing `[Pause 10 seconds]` and expecting it to count.** It is an untimed
  comment and is rejected. Use `[10.0] [Pause 10 seconds]` or `[10.0]` alone.
- **Putting a number in the wrong place.** `[0.4] word` times the next token;
  `[0.4]` at the end of a paragraph is a silent beat, not a prefix for the next
  paragraph.
- **Using a made-up mood.** `{happy}` is shown literally and reported. Use the
  five moods or `{#rrggbb}`.
- **Assuming the app will stretch the last section.** It will not; the total is
  the sum.

## Using this guide

This file is the single canonical copy. It carries skill frontmatter, so to make
it discoverable by opencode, copy it to
`.opencode/skill/teleprompter-script/SKILL.md`.
