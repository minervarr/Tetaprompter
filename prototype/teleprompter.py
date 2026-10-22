#!/usr/bin/env python3
"""Teleprompter — fullscreen scrolling script with clean webcam/mic recording via ffmpeg."""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import threading
import time
import zipfile
from bisect import bisect_left, bisect_right
from collections import deque, namedtuple
from pathlib import Path

# GDK probes the desktop portal at startup for idle-inhibit support, which on
# this Wayland/Sway setup only prints a harmless "Cannot get portal
# org.freedesktop.portal.Inhibit" warning: GTK satisfies IDLE inhibition via
# zwp_idle_inhibit_manager_v1 instead (see _update_inhibit). This app uses no
# portal, so turn the lookups off. Must happen before Gtk is initialised.
_gdk_debug = os.environ.get("GDK_DEBUG", "")
if "no-portals" not in _gdk_debug.split(","):
    os.environ["GDK_DEBUG"] = ",".join(filter(None, (_gdk_debug, "no-portals")))

import gi  # noqa: E402

gi.require_version("Gtk", "4.0")
gi.require_version("Gdk", "4.0")
gi.require_version("Pango", "1.0")
from gi.repository import Gdk, GLib, Gio, Gtk, Pango  # noqa: E402

APP_ID = "io.github.nerio.Teleprompter"
HERE = Path(__file__).resolve().parent


def _data_dir() -> Path:
    """Where script.txt and recordings/ live: beside the app, or beside the .pyz."""
    if HERE.is_dir():  # plain script: the source directory, as always
        return HERE
    entry = Path(sys.argv[0]).resolve()  # zipapp: __file__ points inside the archive
    return entry.parent if entry.is_file() else Path.cwd()


def _state_dir() -> Path:
    """Per-user state (the recent-script cache), following XDG on Linux."""
    base = os.environ.get("XDG_STATE_HOME", "")
    if base and os.path.isabs(base):
        return Path(base) / "teleprompter"
    return Path.home() / ".local" / "state" / "teleprompter"


DATA_DIR = _data_dir()
DEFAULT_SCRIPT = DATA_DIR / "script.txt"
RECORDINGS_DIR = DATA_DIR / "recordings"
STATE_DIR = _state_dir()
RECENTS_FILE = STATE_DIR / "recents.json"
RECENTS_MAX = 30
SAMPLES_DIR = HERE / "samples"
TEST_SCRIPT_NAME = "test-script.txt"

VIDEO_DEVICE = "/dev/video0"
AUDIO_DEVICE = "default"
VIDEO_SIZE = "1280x720"
FPS = 30
CRF = 23
CHECK_TIMEOUT_S = 20  # how long --check waits for the camera/mic to produce data

TICK_MS = 16
REPEAT_GUARD_S = 0.35
FONT_HEIGHT_RATIO = 0.055
PARA_GAP_RATIO = 1.35
LINE_HEIGHT_RATIO = 1.35  # must match the CSS line-height, the reading line uses it

# The reading line ("cue"): the height in the viewport, as a fraction from the top, where
# the presenter's eyes should rest — level with the camera. The text is padded so the
# words being spoken cross that line just when they are due, and two amber bars in the
# side margins mark it without ever touching the script. High (~15-25 %) suits a camera
# mounted above the screen; 50 % suits a camera behind the centre of the screen.
CUE_PRESETS = (0.15, 0.25, 0.35, 0.50)
CUE_DEFAULT = 0.25
CUE_STEP = 0.02
CUE_MIN = 0.05
CUE_MAX = 0.60

# The script column is centred horizontally and at most this wide, so the words being read
# stay near the middle of the screen — where the camera is — and a line never runs from
# edge to edge across a TV. Lines are centred inside the column too; press A for a flush
# left edge, which some readers find easier to re-anchor on at the start of every line.
COLUMN_RATIO = 0.66

# Timing model: the script is the score. Every displayed word and every comment must
# carry a positive `[seconds]` prefix. The app strips those prefixes, sums the numbers,
# and uses the sum as the read total. There is no estimate, no fallback and no rescaling:
# an incomplete score is refused with the source line that needs attention.
WORD_MAX_S = 600.0
TIMING_ISSUES_PRINTED = 20  # keep a very long legacy script from flooding the terminal

# Mood tints for the script text. Near-grayscale on purpose: hue carries the emotional
# register, brightness carries karaoke progress, and every stop stays light enough to read
# on black. `{tag}` in the script picks one; it persists until the next tag.
MOODS = {
    "somber": "#9fb0c4",
    "quiet": "#c3ccd6",
    "plain": "#f2f2f2",
    "warm": "#f5dcc0",
    "bright": "#ffe9a8",
}
MOOD_DEFAULT = "plain"
KARAOKE_GRAY = "#7d8591"       # unread words, readable but clearly waiting
KARAOKE_NEUTRAL = "#f2f2f2"    # lit words when mood colours are disabled
COMMENT_DIM = "#a78c60"        # annotation waiting for its beat
COMMENT_LIT = "#f0c27a"        # annotation at full brightness
COMMENT_BG = "#2a2114"         # inline-comment chip behind the brackets


def scale_hex(color: str, factor: float) -> str:
    """Multiply an #rrggbb colour by a brightness factor, clamped to the byte range."""
    color = color.lstrip("#")
    rgb = [int(color[i:i + 2], 16) for i in (0, 2, 4)]
    return "#{:02x}{:02x}{:02x}".format(
        *(max(0, min(255, round(channel * factor))) for channel in rgb)
    )


def lighten_hex(color: str, amount: float) -> str:
    """Mix an #rrggbb colour towards white by `amount` (0..1)."""
    color = color.lstrip("#")
    rgb = [int(color[i:i + 2], 16) for i in (0, 2, 4)]
    return "#{:02x}{:02x}{:02x}".format(
        *(round(channel + (255 - channel) * amount) for channel in rgb)
    )


def mix_hex(start: str, end: str, amount: float) -> str:
    """Mix two #rrggbb colours by `amount` (0 = start, 1 = end)."""
    amount = max(0.0, min(1.0, float(amount)))
    a = start.lstrip("#")
    b = end.lstrip("#")
    return "#{:02x}{:02x}{:02x}".format(*(
        round(int(a[i:i + 2], 16) + (int(b[i:i + 2], 16) - int(a[i:i + 2], 16)) * amount)
        for i in (0, 2, 4)
    ))


def comment_colour(progress: float) -> str:
    """The uniform amber brightness of an annotation at *progress* through its beat."""
    progress = max(0.0, min(1.0, float(progress)))
    return mix_hex(COMMENT_DIM, COMMENT_LIT, progress)


def word_fill(word_clock: float, word_seconds: float, now: float) -> float:
    """How much of a word has been sung: 0 at its start, 1 at its end."""
    if word_seconds <= 0:
        return 1.0 if now >= word_clock else 0.0
    return max(0.0, min(1.0, (now - word_clock) / word_seconds))


def character_fill(progress: float, index: int, count: int) -> float:
    """Brightness of one character in a left-to-right fill sweep."""
    if count <= 0:
        return max(0.0, min(1.0, progress))
    edge = max(0.0, min(1.0, progress)) * count
    return max(0.0, min(1.0, edge - index))

# The HUD dims itself while the script plays so nothing pulls the presenter's attention away
# from the lens; pointer motion (or pausing) brings it back. H still hides it entirely.
HUD_DIM_AFTER_S = 3

# The countdown in the HUD changes colour when the read is running out of time.
REMAIN_WARN_S = 30
REMAIN_DANGER_S = 10

# Single source of truth for the help overlay (and a good checklist for the README table).
KEY_HELP = (
    ("Space", "Start / pause scrolling — at the end, restart from the top"),
    ("R", "Start / stop recording"),
    ("Ctrl+R", "Return to the script library (refresh it there)"),
    ("Ctrl+O", "Open another script from disk"),
    ("F or F11", "Toggle fullscreen — Esc leaves fullscreen"),
    ("↑ / ↓", "Move the reading line up / down by 2 %"),
    ("C", "Cycle the reading line (15 · 25 · 35 · 50 %)"),
    ("A", "Toggle centred / flush-left text"),
    ("K", "Toggle word-by-word karaoke fill"),
    ("M", "Toggle mood colours"),
    ("← / →", "Nudge the text"),
    ("Home / End", "Jump to the top / bottom"),
    ("F5 or L", "Reload the script file"),
    ("H", "Show / hide the toolbar and section timer"),
    ("? or F1", "Show this help"),
    ("Ctrl+Q", "Quit — finalizes any recording first"),
)

# The test script lives in samples/ so it can be embedded in the .pyz instead of
# being written beside it. The fallback only covers a broken/incomplete install.
TEST_FALLBACK = """{warm}[0.45]The [0.35]built-in [0.35]test [0.30]script [0.35]is [0.30]unavailable.

[0.35]Open [0.30]one [0.30]of [0.25]your [0.30]own [0.35]scores [0.30]from [0.25]the [0.35]library.
"""


def sample_script_text() -> str:
    """The built-in demo score: from samples/ in source, or from inside the .pyz."""
    source = SAMPLES_DIR / TEST_SCRIPT_NAME
    try:
        return source.read_text(encoding="utf-8")
    except (OSError, UnicodeError):
        pass

    archive = Path(sys.argv[0]).resolve()
    if archive.is_file():
        try:
            with zipfile.ZipFile(archive) as zf:
                return zf.read(f"samples/{TEST_SCRIPT_NAME}").decode("utf-8")
        except (KeyError, OSError, UnicodeDecodeError, zipfile.BadZipFile):
            pass
    return TEST_FALLBACK


def _path_key(path: Path) -> str:
    """Canonical key for deduplicating recent scripts."""
    path = Path(path).expanduser()
    try:
        return str(path.resolve())
    except (OSError, RuntimeError):
        return str(path.absolute())


def load_recents(path: Path = RECENTS_FILE) -> list[dict]:
    """Read the recent-script cache, dropping corrupt entries and missing files."""
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return []
    items = payload.get("scripts") if isinstance(payload, dict) else payload
    if not isinstance(items, list):
        return []

    result: list[dict] = []
    seen: set[str] = set()
    for item in items:
        if not isinstance(item, dict):
            continue
        raw = item.get("path")
        if not isinstance(raw, str) or not raw:
            continue
        script = Path(raw).expanduser()
        try:
            opened = float(item.get("opened", 0.0))
        except (TypeError, ValueError):
            opened = 0.0
        if not 0.0 <= opened <= time.time() + 86400:
            opened = 0.0  # NaN, infinity and clock-skewed values are not useful
        key = _path_key(script)
        if key in seen or not script.is_file():
            continue
        seen.add(key)
        result.append({"path": key, "opened": opened})
    result.sort(key=lambda entry: entry["opened"], reverse=True)
    return result[:RECENTS_MAX]


def save_recents(entries: list[dict], path: Path = RECENTS_FILE) -> bool:
    """Write the cache atomically. Recents are a convenience, never a hard failure."""
    payload = {"version": 1, "scripts": entries[:RECENTS_MAX]}
    temp = None
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        temp = path.with_name(f".{path.name}.{os.getpid()}.tmp")
        temp.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
        os.replace(temp, path)
        return True
    except OSError:
        if temp is not None:
            try:
                temp.unlink()
            except OSError:
                pass
        return False


def remember_recent(entries: list[dict], script_path: Path, now: float | None = None) -> list[dict]:
    """Move a script to the front of the list (or add it), keeping the newest first."""
    key = _path_key(script_path)
    stamp = time.time() if now is None else now
    fresh = [{"path": key, "opened": stamp}]
    fresh.extend(entry for entry in entries if entry.get("path") != key)
    return fresh[:RECENTS_MAX]


def display_path(path: Path) -> str:
    """A readable path for a card, with the home directory shortened to ~."""
    try:
        relative = Path(path).expanduser().relative_to(Path.home())
    except (ValueError, RuntimeError, OSError):
        return str(Path(path).expanduser())
    return "~" if str(relative) == "." else f"~/{relative}"


def format_age(opened: float, now: float | None = None) -> str:
    """How long ago a script was opened, in the compact form used on a card."""
    current = time.time() if now is None else now
    delta = max(0.0, current - opened)
    if delta < 60:
        return "just now"
    if delta < 3600:
        return f"{int(delta // 60)} min ago"
    if delta < 86400:
        return f"{int(delta // 3600)} h ago"
    if delta < 7 * 86400:
        days = int(delta // 86400)
        return f"{days} day{'s' if days != 1 else ''} ago"
    return time.strftime("%b %d, %Y", time.localtime(opened)).replace(" 0", " ")


def format_time(seconds: float) -> str:
    total = max(0, int(round(seconds)))
    return f"{total // 60}:{total % 60:02d}"


def human_size(num_bytes: int) -> str:
    size = float(num_bytes)
    for unit in ("B", "KB", "MB", "GB"):
        if size < 1024 or unit == "GB":
            return f"{size:.0f} {unit}" if unit == "B" else f"{size:.1f} {unit}"
        size /= 1024
    raise AssertionError("unreachable: the GB branch always returns")


def parse_size(value: str) -> str:
    """Validate a WIDTHxHEIGHT capture size (H.264 yuv420p needs even numbers)."""
    match = re.fullmatch(r"(\d{2,5})x(\d{2,5})", value)
    if not match:
        raise argparse.ArgumentTypeError(f"expected WIDTHxHEIGHT, e.g. 1280x720 (got {value!r})")
    width, height = int(match.group(1)), int(match.group(2))
    if width % 2 or height % 2:
        raise argparse.ArgumentTypeError(
            f"{value}: width and height must be even for H.264 (e.g. 1280x720)"
        )
    return value


# --------------------------------------------------------------- script format
#
# The script is the timing score. Paragraphs are separated by blank lines. The
# author writes a `[seconds]` prefix before each word (or comment), and can set a
# `{mood}` / `{mood: Theme}` colour before any word:
#
#   [0.45]Good [0.30]morning, [0.55]everyone!
#   {somber}[0.40]But [0.30]here [0.50]is [0.25]the [0.45]thing.
#
# A bracket is a comment: shown italic, and timed like any other visible token.
# `[1.5] [look at the camera]` owns 1.5 seconds; a bare `[look]` is a hard error.
# `[2.0]` alone is a silent pause. Prefixes are stripped before display; the app
# only sums the numbers.

Word = namedtuple(
    "Word", "text seconds mood theme char_start byte_start clock is_comment",
    defaults=(MOOD_DEFAULT, "", 0, 0, 0.0, False),
)
Paragraph = namedtuple(
    "Paragraph", "text is_direction seconds mood theme words start markup",
    defaults=(MOOD_DEFAULT, "", (), 0.0, ""),
)
SectionState = namedtuple(
    "SectionState", "index paragraph elapsed remaining progress",
)
_Token = namedtuple(
    "_Token", "text seconds mood theme is_comment is_silence",
    defaults=(False, False),
)


class TimingIssue(namedtuple("_TimingIssue", "line paragraph kind text reason")):
    """One missing or invalid duration in a score."""

    def describe(self) -> str:
        return f"line {self.line}: {self.kind} {self.text!r} {self.reason}"


class ScriptTimingError(ValueError):
    """The score is incomplete: every displayed token needs a positive time."""

    def __init__(self, issues: list[TimingIssue]):
        self.issues = list(issues)
        super().__init__("\n".join(issue.describe() for issue in self.issues))

    @property
    def summary(self) -> str:
        first = self.issues[0].describe() if self.issues else "incomplete timing"
        extra = f" (+{len(self.issues) - 1} more)" if len(self.issues) > 1 else ""
        return first + extra


_PARAGRAPH_RE = re.compile(r"\n\s*\n")
_DURATION_RE = re.compile(r"^\[(\d+(?:\.\d+)?)\]$")
_LEGACY_ANCHOR_RE = re.compile(r"^\[(\d{1,3}(?::\d{1,2}){1,2}(?:\.\d+)?)\]$")
_COMMENT_RE = re.compile(r"\[([^\[\]]*)\]")
_WRAPPED_COMMENT_RE = re.compile(
    r"^\s*\*{0,2}\s*\[([^\[\]]*)\]\s*\*{0,2}\s*$"
)
_WRAPPED_TIMED_RE = re.compile(
    r"^\s*\[(\d+(?:\.\d+)?)\]\s*\*{0,2}\s*\[([^\[\]]*)\]\s*\*{0,2}\s*$"
)
_WRAPPED_INLINE_RE = re.compile(r"\*{1,2}\s*\[([^\[\]]*)\]\s*\*{1,2}")
_MOOD_TAG_RE = re.compile(r"\{([^{}\n]*)\}")
_HEX_COLOUR_RE = re.compile(r"^#[0-9a-f]{6}$")
_BOLD_RE = re.compile(r"\*\*(.+?)\*\*")
_ITALIC_RE = re.compile(r"(?<!\*)\*([^*\n]+?)\*(?!\*)")


def _paragraph_blocks(text: str) -> list[tuple[str, int]]:
    """Paragraph text plus its 1-based source line, separated by blank lines."""
    blocks: list[tuple[str, int]] = []
    start = 0
    for separator in _PARAGRAPH_RE.finditer(text):
        raw = text[start:separator.start()]
        body = raw.strip()
        if body:
            offset = start + raw.find(body)
            blocks.append((body, text.count("\n", 0, offset) + 1))
        start = separator.end()
    raw = text[start:]
    body = raw.strip()
    if body:
        offset = start + raw.find(body)
        blocks.append((body, text.count("\n", 0, offset) + 1))
    return blocks


def split_paragraphs(text: str) -> list[str]:
    return [body for body, _line in _paragraph_blocks(text)]


def mood_colour(name: str) -> str:
    """A mood name from MOODS, or an author-supplied #rrggbb colour."""
    if name in MOODS:
        return MOODS[name]
    if _HEX_COLOUR_RE.match(name):
        return name
    return MOODS[MOOD_DEFAULT]


def _clamp_word_seconds(seconds: float) -> float:
    return max(0.0, min(WORD_MAX_S, float(seconds)))


def _parse_mood_tag(content: str, warnings: list[str]):
    """Return (mood, theme) for a `{...}` prefix, or (None, None) on a typo."""
    name, _, theme_text = content.partition(":")
    name = name.strip().lower()
    if name in MOODS or _HEX_COLOUR_RE.match(name):
        return name, theme_text.strip() or None
    warnings.append(
        f"unknown mood {{{content.strip()}}} — use {', '.join(MOODS)} or #rrggbb"
    )
    return None, None


def _parse_tokens(raw: str, state: dict, warnings: list[str],
                  issues: list[TimingIssue], *, paragraph: int,
                  first_line: int) -> list[_Token]:
    """Split a paragraph into timed words, comments and silent beats.

    `state` carries the current mood/theme across paragraphs. A duration prefix is
    held until the next word, comment or the end of the paragraph (a silent beat).
    Every displayed token must consume a positive amount of time; issues are
    collected with the source line so the whole score can be checked at once.
    """
    body = raw.strip()
    tokens: list[_Token] = []
    pending: float | None = None
    i = 0
    end = len(body)

    def issue(start: int, kind: str, text: str, reason: str) -> None:
        line = first_line + body[:start].count("\n")
        issues.append(TimingIssue(line, paragraph, kind, text, reason))

    def take_pending(start: int, kind: str, text: str) -> float:
        nonlocal pending
        seconds = pending
        pending = None
        if seconds is None:
            issue(start, kind, text, "has no [seconds] time")
            return 0.0
        if seconds <= 0:
            issue(start, kind, text,
                  "has zero duration; use a positive [seconds] time")
            return 0.0
        return seconds

    # Keep the old `**[Do this.]**` / `*[Do this.]*` direction spelling working as a
    # comment, including the timed `[1.5] **[look]**` form.
    timed_wrap = _WRAPPED_TIMED_RE.match(body)
    if timed_wrap is not None:
        text = f"[{timed_wrap.group(2).strip()}]"
        seconds = _clamp_word_seconds(float(timed_wrap.group(1)))
        if seconds <= 0:
            issue(0, "comment", text,
                  "has zero duration; use a positive [seconds] time")
        tokens.append(_Token(
            text, seconds, state["mood"], state["theme"], is_comment=True,
        ))
        return tokens
    if _LEGACY_ANCHOR_RE.match(body):
        issue(0, "anchor", body,
              "is no longer valid; use a positive [seconds] prefix")
        return tokens
    plain_wrap = _WRAPPED_COMMENT_RE.match(body)
    if plain_wrap is not None and _DURATION_RE.match(body) is None:
        text = f"[{plain_wrap.group(1).strip()}]"
        issue(0, "comment", text, "has no [seconds] time")
        tokens.append(_Token(
            text, 0.0, state["mood"], state["theme"], is_comment=True,
        ))
        return tokens

    while i < end:
        if body[i].isspace():
            i += 1
            continue

        if body[i] == "{":
            match = _MOOD_TAG_RE.match(body, i)
            if match is not None:
                mood, theme = _parse_mood_tag(match.group(1), warnings)
                if mood is not None:
                    state["mood"] = mood
                    if theme is not None:
                        state["theme"] = theme
                    i = match.end()
                    continue
                # Leave an unknown tag literal, exactly like the old format did.
                start = i
                text = body[i:match.end()]
                seconds = take_pending(start, "word", text)
                tokens.append(_Token(text, seconds, state["mood"], state["theme"]))
                i = match.end()
                continue

        if body[i] == "*":
            wrapped = _WRAPPED_INLINE_RE.match(body, i)
            if wrapped is not None:
                start = i
                text = f"[{wrapped.group(1).strip()}]"
                seconds = take_pending(start, "comment", text)
                tokens.append(_Token(
                    text, seconds, state["mood"], state["theme"], is_comment=True,
                ))
                i = wrapped.end()
                continue

        if body[i] == "[":
            match = _COMMENT_RE.match(body, i)
            if match is not None:
                duration = _DURATION_RE.match(match.group(0))
                if duration is not None:
                    pending = _clamp_word_seconds(float(duration.group(1)))
                    i = match.end()
                    continue
                if _LEGACY_ANCHOR_RE.match(match.group(0)):
                    issue(i, "anchor", match.group(0),
                          "is no longer valid; use a positive [seconds] prefix")
                    i = match.end()
                    continue
                start = i
                text = f"[{match.group(1).strip()}]"
                seconds = take_pending(start, "comment", text)
                tokens.append(_Token(
                    text, seconds, state["mood"], state["theme"], is_comment=True,
                ))
                i = match.end()
                continue

        start = i
        while i < end and not body[i].isspace():
            i += 1
        text = body[start:i]
        if text:
            seconds = take_pending(start, "word", text)
            tokens.append(_Token(text, seconds, state["mood"], state["theme"]))

    if pending is not None:
        tokens.append(_Token("", pending, state["mood"], state["theme"],
                             is_silence=True))
    return tokens


def _finish_paragraph(tokens: list[_Token], clock: float) -> Paragraph:
    """Turn parsed tokens into one display paragraph with a word-level clock."""
    raw_parts = [token.text for token in tokens if token.text]
    raw_display = " ".join(raw_parts) if raw_parts else " "
    plain_display = raw_display.replace("*", "")
    markup = to_markup(raw_display)

    words: list[Word] = []
    cursor = 0
    running = clock
    for token in tokens:
        plain_word = token.text.replace("*", "")
        if plain_word:
            found = plain_display.find(plain_word, cursor)
            char_start = found if found >= 0 else cursor
        else:
            char_start = cursor
        byte_start = len(plain_display[:char_start].encode("utf-8"))
        words.append(Word(
            plain_word, token.seconds, token.mood, token.theme,
            char_start, byte_start, running, token.is_comment,
        ))
        running += token.seconds
        if plain_word:
            cursor = char_start + len(plain_word) + 1

    total = running - clock
    spoken = [word for word in words if word.text and not word.is_comment]
    first = spoken[0] if spoken else (words[0] if words else None)
    return Paragraph(
        plain_display,
        not spoken,
        total,
        first.mood if first is not None else MOOD_DEFAULT,
        first.theme if first is not None else "",
        tuple(words),
        clock,
        markup,
    )


def paragraph_plan(text: str, warnings: list[str] | None = None) -> list[Paragraph]:
    """Parse the script into paragraphs whose word times sum to the read total.

    Every displayed word and comment must have an explicit positive `[seconds]`
    prefix. Mood-only headers display nothing and need no timer. Invalid timing is
    collected into a ScriptTimingError with source lines instead of being estimated.
    """
    warn = warnings if warnings is not None else []
    issues: list[TimingIssue] = []
    state = {"mood": MOOD_DEFAULT, "theme": ""}
    plan: list[Paragraph] = []
    clock = 0.0
    for number, (raw, first_line) in enumerate(_paragraph_blocks(text), start=1):
        tokens = _parse_tokens(
            raw, state, warn, issues, paragraph=number, first_line=first_line,
        )
        if not tokens:
            continue  # a mood-only header: it only changes state
        paragraph = _finish_paragraph(tokens, clock)
        plan.append(paragraph)
        clock += paragraph.seconds
    if issues:
        raise ScriptTimingError(issues)
    return plan


def _escape_markup(text: str) -> str:
    return text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def to_markup(text: str) -> str:
    """Pango markup for a display paragraph: **bold** and *italic*."""
    markup = _escape_markup(text)
    markup = _BOLD_RE.sub(r"<b>\1</b>", markup)
    return _ITALIC_RE.sub(r"<i>\1</i>", markup)


def preview_text(text: str, width: int = 70) -> str:
    flat = " ".join(text.replace("*", "").split())
    return flat if len(flat) <= width else flat[: width - 1] + "…"


def section_title(paragraph: Paragraph, width: int = 46) -> str:
    """A short human label for a standalone comment or silent beat."""
    text = paragraph.text.replace("[", " ").replace("]", " ").replace("*", " ")
    text = " ".join(text.split())
    if not text:
        return "Hold"
    return text if len(text) <= width else text[: width - 1].rstrip() + "…"


def section_state(plan: list[Paragraph], now: float,
                  starts: list[float] | None = None) -> SectionState | None:
    """The active section and its clock, or None during spoken text/end.

    A section is a paragraph with no spoken words: a standalone comment, a batch
    of comments, or a silent beat. Inline comments do not qualify.
    """
    if not plan:
        return None
    if starts is None:
        starts = [paragraph.start for paragraph in plan]
    index = bisect_right(starts, now) - 1
    index = max(0, min(index, len(plan) - 1))
    paragraph = plan[index]
    if not paragraph.is_direction:
        return None
    elapsed = max(0.0, min(paragraph.seconds, now - paragraph.start))
    remaining = max(0.0, paragraph.seconds - elapsed)
    progress = elapsed / paragraph.seconds if paragraph.seconds > 0 else 1.0
    return SectionState(index, paragraph, elapsed, remaining, progress)


def print_timing(script_path: Path, show_words: bool = False) -> int:
    """Dump the summed score: paragraph starts, optional word detail, total."""
    try:
        text = script_path.read_text(encoding="utf-8")
    except OSError as exc:
        print(f"cannot read {script_path}: {exc}", file=sys.stderr)
        return 1

    warnings: list[str] = []
    try:
        plan = paragraph_plan(text, warnings)
    except ScriptTimingError as exc:
        print(f"{script_path}: timing is incomplete", file=sys.stderr)
        for issue in exc.issues[:TIMING_ISSUES_PRINTED]:
            print(f" {issue.describe()}", file=sys.stderr)
        hidden = len(exc.issues) - TIMING_ISSUES_PRINTED
        if hidden > 0:
            print(f" … and {hidden} more timing issue(s)", file=sys.stderr)
        for warning in warnings:
            print(f" warning: {warning}", file=sys.stderr)
        return 1
    if not plan:
        print(f"{script_path}: no paragraphs to time")
        return 0

    spoken = [word for p in plan for word in p.words if word.text and not word.is_comment]
    comments = [word for p in plan for word in p.words if word.is_comment]
    total = sum(p.seconds for p in plan)

    print(f"{script_path} — {len(plan)} paragraphs, {len(spoken)} words, "
          f"{len(comments)} comments, total {format_time(total)} ({total:.1f}s)")
    print()
    print("   start   time  kind  mood     paragraph")
    for p in plan:
        kind = "note" if p.is_direction else "say "
        body = preview_text(p.text)
        if p.theme:
            body = f"{p.theme}: {body}"
        print(f" {format_time(p.start):>7}  {p.seconds:5.1f}s  {kind}  "
              f"{p.mood:<7}  {body}")

    if show_words:
        print()
        print("   start   time  kind   mood     word")
        for p in plan:
            for word in p.words:
                if word.is_comment:
                    kind = "note "
                elif not word.text:
                    kind = "pause"
                else:
                    kind = "word "
                label = word.text or "—"
                print(f" {format_time(word.clock):>7}  {word.seconds:5.2f}s  "
                      f"{kind}  {word.mood:<7}  {preview_text(label, 48)}")

    print()
    print(f" total {format_time(total)}  ({total:.1f} seconds, "
          f"{len(spoken)} spoken words)")
    for warning in warnings:
        print(f"warning: {warning}", file=sys.stderr)
    return 0


def script_read_time(text: str) -> str | None:
    """The calculated read time for a card; None when the score is incomplete."""
    try:
        plan = paragraph_plan(text)
    except ScriptTimingError:
        return None
    return format_time(sum(paragraph.seconds for paragraph in plan))


def read_script_time(path: Path) -> str | None:
    """Best-effort read time for a library card; None when unreadable or incomplete."""
    try:
        return script_read_time(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError):
        return None


# ------------------------------------------------------------- process helpers

_ERROR_HINT_RE = re.compile(
    r"cannot|busy|no such|not found|permission|denied|invalid|unable|failed|error",
    re.IGNORECASE,
)


def pick_error_line(text: str, limit: int = 200) -> str:
    """The most useful line of an ffmpeg stderr dump.

    ffmpeg prints the specific complaint first and generic summaries after it, so prefer
    the first line that names a cause and only fall back to the first line at all. The
    `[in#0 @ 0x...]` component prefix is stripped for readability.
    """
    lines = [line.strip() for line in text.splitlines() if line.strip()]
    if not lines:
        return ""
    best = next((line for line in lines if _ERROR_HINT_RE.search(line)), lines[0])
    return re.sub(r"^\[[^\]]*\]\s*", "", best)[:limit]


def check_devices(*, device: str, audio: str, size: str, fps: int) -> int:
    """Open the camera and the mic for a second each, exactly as recording would.

    A busy camera or a dead microphone is far cheaper to discover now than after a take.
    The camera LED blinks for a second — that is the check doing its job.
    """
    checks = (
        (f"camera      {device} {size}@{fps}", [
            "-f", "v4l2", "-input_format", "mjpeg",
            "-video_size", size, "-framerate", str(fps), "-i", device,
        ]),
        (f"microphone  {audio}", ["-f", "pulse", "-i", audio]),
    )
    failures = 0
    for label, inputs in checks:
        cmd = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-nostats",
               *inputs, "-t", "1", "-f", "null", "-"]
        try:
            done = subprocess.run(
                cmd, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE, text=True, timeout=CHECK_TIMEOUT_S,
            )
        except FileNotFoundError:
            print("ffmpeg not found — install the ffmpeg package", file=sys.stderr)
            return 1
        except subprocess.TimeoutExpired:
            print(f"{label} — FAIL: no data within {CHECK_TIMEOUT_S} s")
            failures += 1
            continue
        if done.returncode == 0:
            print(f"{label} — OK")
        else:
            reason = pick_error_line(done.stderr) or f"ffmpeg exit code {done.returncode}"
            print(f"{label} — FAIL: {reason}")
            failures += 1
    if failures:
        sys.stdout.flush()  # keep the summary after the per-device lines when piped
        print(f"\n{failures} of {len(checks)} devices failed", file=sys.stderr)
        return 1
    return 0


# --------------------------------------------------------------------- timeline
#
# The scroll engine maps *weight* (seconds of script consumed) onto pixels. A knot is the
# scroll position at the moment a paragraph takes over, the cum table the weight spent
# before it. Between knots the position is linear in weight — that is what makes spoken
# text cross the reading line exactly on the clock, whatever the cue height and however
# the paragraphs wrap.

class Timeline:
    """Weight ↔ pixel mapping for one script layout; rebuilt whenever the text is measured."""

    def __init__(self) -> None:
        self.knots: list[float] = [0.0]
        self.cum: list[float] = [0.0]
        self.total_weight = 0.0

    def clear(self, total_weight: float = 0.0) -> None:
        self.knots, self.cum = [0.0], [0.0]
        self.total_weight = total_weight

    def rebuild(self, plan: list[Paragraph], *, tops: list[float], text_end: float,
                cue_y: float, max_scroll: float, half_line: float) -> None:
        """Lay the plan over the measured paragraph tops (paragraph-level timeline)."""
        if not plan:
            self.clear()
            return
        total = 0.0
        knots, cum = [0.0], [0.0]
        last = len(plan) - 1
        for i, paragraph in enumerate(plan):
            total += paragraph.seconds
            cum.append(total)
            end = text_end - half_line if i == last else tops[i + 1] + half_line
            knots.append(max(min(end - cue_y, max_scroll), knots[-1]))
        self.knots, self.cum, self.total_weight = knots, cum, total

    def rebuild_entries(self, entries: list[tuple[float, float]], *, max_scroll: float) -> None:
        """Build the timeline from (scroll position, seconds) word knots.

        Several words can share one visual line, so positions may repeat. The first
        of a repeated group is where the line arrives; the extra entries are the time
        the line dwells while its words are read.
        """
        if not entries:
            self.clear()
            return
        total = 0.0
        knots, cum = [0.0], [0.0]
        previous = 0.0
        for position, seconds in entries:
            seconds = max(0.0, float(seconds))
            position = max(previous, min(float(position), max_scroll))
            total += seconds
            knots.append(position)
            cum.append(total)
            previous = position
        self.knots, self.cum, self.total_weight = knots, cum, total

    def pos_from_weight(self, weight: float) -> float:
        if len(self.knots) < 2:
            return 0.0
        if weight <= 0.0:
            return 0.0
        i = max(0, min(bisect_right(self.cum, weight) - 1, len(self.cum) - 2))
        w0, w1 = self.cum[i], self.cum[i + 1]
        p0, p1 = self.knots[i], self.knots[i + 1]
        if w1 <= w0:
            return p1
        return p0 + (min(weight, w1) - w0) / (w1 - w0) * (p1 - p0)

    def weight_from_pos(self, pos: float) -> float:
        if len(self.knots) < 2:
            return 0.0
        if pos <= self.knots[0]:
            return self.cum[0]
        if pos >= self.knots[-1]:
            return self.cum[-1]
        i = bisect_left(self.knots, pos)
        if self.knots[i] == pos:
            return self.cum[i]
        p0, p1 = self.knots[i - 1], self.knots[i]
        w0, w1 = self.cum[i - 1], self.cum[i]
        if p1 <= p0:
            return w1
        return w0 + (pos - p0) / (p1 - p0) * (w1 - w0)


def build_ffmpeg_cmd(out_path, *, device, audio, size, fps, crf) -> list[str]:
    return [
        "ffmpeg", "-hide_banner", "-loglevel", "error", "-nostats",
        "-f", "v4l2", "-input_format", "mjpeg",
        "-video_size", size, "-framerate", str(fps), "-i", device,
        "-f", "pulse", "-i", audio,
        "-c:v", "libx264", "-preset", "veryfast", "-crf", str(crf),
        "-pix_fmt", "yuv420p",
        "-c:a", "aac", "-b:a", "160k",
        "-movflags", "+faststart",
        "-y", str(out_path),
    ]


class Recorder:
    """Owns the ffmpeg subprocess. Start/stop must never block the UI thread."""

    def __init__(self, *, device, audio, size, fps, crf, out_dir):
        self.device = device
        self.audio = audio
        self.size = size
        self.fps = fps
        self.crf = crf
        self.out_dir = Path(out_dir)
        self.proc: subprocess.Popen | None = None
        self.path: Path | None = None
        self.started_at = 0.0
        self._log: deque[str] = deque(maxlen=200)

    @property
    def active(self) -> bool:
        return self.proc is not None

    def start(self) -> Path:
        self.out_dir.mkdir(parents=True, exist_ok=True)
        base = time.strftime("%Y-%m-%d_%H%M%S")
        path = self.out_dir / f"{base}.mp4"
        n = 2
        while path.exists():
            path = self.out_dir / f"{base}-{n}.mp4"
            n += 1
        self._log.clear()
        cmd = build_ffmpeg_cmd(
            path, device=self.device, audio=self.audio,
            size=self.size, fps=self.fps, crf=self.crf,
        )
        try:
            self.proc = subprocess.Popen(
                cmd, stdin=subprocess.PIPE,
                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
            )
        except FileNotFoundError as exc:
            raise RuntimeError("ffmpeg not found — install the ffmpeg package") from exc
        self.path = path
        self.started_at = time.monotonic()
        threading.Thread(target=self._drain, daemon=True).start()
        return path

    def _drain(self) -> None:
        proc = self.proc
        if proc is None or proc.stderr is None:
            return
        for line in proc.stderr:
            text = line.decode("utf-8", "replace").strip()
            if text:
                self._log.append(text)

    def poll(self) -> int | None:
        return self.proc.poll() if self.proc else None

    def take_proc(self) -> subprocess.Popen | None:
        proc, self.proc = self.proc, None
        return proc

    def last_error(self) -> str:
        """The most specific complaint ffmpeg made, for the HUD."""
        return pick_error_line("\n".join(self._log))

    def stop(self, callback) -> None:
        proc = self.take_proc()
        if proc is None:
            return
        path = self.path

        def worker():
            code = self._shutdown(proc)
            GLib.idle_add(callback, path, code)

        threading.Thread(target=worker, daemon=True).start()

    def stop_blocking(self) -> None:
        proc = self.take_proc()
        if proc is not None:
            self._shutdown(proc)

    @staticmethod
    def _shutdown(proc: subprocess.Popen) -> int:
        try:
            if proc.poll() is None and proc.stdin is not None:
                proc.stdin.write(b"q")
                proc.stdin.flush()
                proc.stdin.close()
                try:
                    return proc.wait(timeout=6)
                except subprocess.TimeoutExpired:
                    proc.terminate()
                    try:
                        return proc.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        proc.kill()
                        proc.wait()
        except (BrokenPipeError, OSError, ValueError):
            pass
        code = proc.poll()
        return -1 if code is None else code


def build_css(font_px: int) -> str:
    return f"""
window {{ background-color: #000000; }}
.script {{
    font-family: "Adwaita Sans", Cantarell, "Noto Sans", sans-serif;
    color: #f2f2f2;
    font-size: {font_px}px;
    font-weight: 600;
    line-height: {LINE_HEIGHT_RATIO};
    letter-spacing: 0.01em;
}}
.direction {{
    color: #f0c27a;
    background-color: rgba(240, 160, 60, 0.08);
    border: 1px solid rgba(240, 160, 60, 0.20);
    border-left: 4px solid rgba(240, 160, 60, 0.72);
    border-radius: 8px;
    padding: 5px 14px;
    font-family: "Noto Sans Mono", "DejaVu Sans Mono", monospace;
    font-size: {max(14, round(font_px * 0.68))}px;
    font-style: italic;
    font-weight: 500;
    line-height: 1.3;
}}
.hud {{
    background-color: rgba(18, 18, 18, 0.62);
    border-radius: 12px;
    padding: 4px 14px;
    transition-property: opacity;
    transition-duration: 400ms;
    transition-timing-function: ease;
}}
.hud.dimmed {{ opacity: 0.3; }}
.section-card {{
    background-color: rgba(20, 15, 8, 0.94);
    border: 1px solid rgba(240, 160, 60, 0.38);
    border-radius: 10px;
    padding: 7px 14px;
    box-shadow: none;
}}
.section-card label.section-title {{
    color: #b99a6a;
    font-size: 11px;
    font-weight: 700;
    letter-spacing: 0.08em;
}}
.section-card label.section-time {{
    color: #f0c27a;
    font-size: 16px;
    font-weight: 700;
}}
.section-progress {{ min-width: 280px; min-height: 3px; margin-top: 2px; }}
.section-progress > trough {{
    background-color: rgba(255, 255, 255, 0.12);
    border: none;
    min-height: 3px;
    border-radius: 2px;
}}
.section-progress > trough > progress {{
    background-color: #f0a03c;
    min-height: 3px;
    border-radius: 2px;
}}
.hud label {{ color: #cfcfcf; font-size: 15px; }}
.hud button label {{ font-size: 15px; }}
.hud button image {{ color: #cfcfcf; }}
.hud label.state {{
    color: #9aa4b2;
    font-size: 13px;
    font-weight: 600;
    background-color: rgba(255, 255, 255, 0.07);
    border-radius: 999px;
    padding: 2px 10px;
}}
.hud label.warn {{ color: #f0a03c; }}
.hud label.danger {{ color: #ff5f56; font-weight: 700; }}
.hud label.error {{ color: #ff8a80; }}
.hud label.rec-on {{ color: #ff5f56; font-weight: 700; }}
.hud button.rec-on label, .hud button.rec-on image {{ color: #ff5f56; font-weight: 700; }}
.rec-dot {{
    background-color: #ff5f56;
    border-radius: 999px;
    min-width: 10px;
    min-height: 10px;
    animation-name: rec-blink;
    animation-duration: 1.2s;
    animation-iteration-count: infinite;
    animation-timing-function: ease-in-out;
}}
@keyframes rec-blink {{
    0% {{ opacity: 1.0; }}
    50% {{ opacity: 0.2; }}
    100% {{ opacity: 1.0; }}
}}
.rec-frame {{ border: 2px solid rgba(255, 95, 86, 0.85); }}
.cue-bar {{
    background-color: rgba(240, 160, 60, 0.9);
    border-radius: 3px;
    box-shadow: 0 0 10px rgba(240, 160, 60, 0.4);
}}
.read-progress {{ margin: 0; padding: 0; }}
.read-progress > trough {{ background-color: transparent; border: none; min-height: 3px; }}
.read-progress > trough > progress {{
    background-color: rgba(240, 160, 60, 0.9);
    border-radius: 0;
    min-height: 3px;
}}
.library {{
    background-color: #0a0a0c;
    padding: 34px 38px 18px;
}}
.library-title {{ color: #f2f2f2; font-size: 30px; font-weight: 700; }}
.library-subtitle {{ color: #8a93a0; font-size: 14px; }}
.library-section {{
    color: #8a93a0;
    font-size: 12px;
    font-weight: 700;
    letter-spacing: 0.12em;
    margin-top: 8px;
}}
.library-hint {{ color: #5f6874; font-size: 12px; }}
.library-msg {{ color: #cfcfcf; font-size: 13px; }}
.library-msg.error {{ color: #ff8a80; }}
.open-script {{
    background-image: none;
    background-color: #f0a03c;
    color: #141414;
    border: none;
    border-radius: 999px;
    padding: 9px 18px;
    font-weight: 700;
    box-shadow: none;
}}
.open-script:hover {{ background-image: none; background-color: #ffb45c; }}
.open-script label, .open-script image {{ color: #141414; font-weight: 700; }}
.script-card {{
    background-image: none;
    background-color: #151518;
    border: 1px solid rgba(255, 255, 255, 0.08);
    border-radius: 12px;
    padding: 14px 16px;
    box-shadow: none;
    text-shadow: none;
}}
.script-card:hover {{
    background-image: none;
    background-color: #1d1d22;
    border-color: rgba(240, 160, 60, 0.55);
}}
.script-card:focus {{ border-color: #f0a03c; }}
.script-card.sample-card {{
    background-image: none;
    border-color: rgba(240, 160, 60, 0.32);
    background-color: #17161a;
}}
.card-title {{ color: #f2f2f2; font-size: 16px; font-weight: 600; }}
.card-path {{ color: #7d8794; font-size: 12px; }}
.card-meta {{ color: #9aa4b2; font-size: 12px; }}
.sample-badge {{
    color: #f0a03c;
    font-size: 10px;
    font-weight: 700;
    background-color: rgba(240, 160, 60, 0.12);
    border-radius: 999px;
    padding: 2px 8px;
}}
.empty-recents {{ color: #7d8794; padding: 10px 2px 6px; }}
.empty-title {{ color: #cfcfcf; font-size: 16px; font-weight: 600; }}
.empty-hint {{ color: #7d8794; font-size: 13px; }}
.help-backdrop {{ background-color: rgba(0, 0, 0, 0.74); }}
.help-card {{
    background-color: rgba(24, 24, 27, 0.97);
    border: 1px solid rgba(255, 255, 255, 0.10);
    border-radius: 16px;
    padding: 22px 28px;
}}
.help-title {{ color: #f2f2f2; font-size: 19px; font-weight: 700; }}
.help-key {{ color: #f0a03c; font-size: 14px; font-weight: 700; font-family: monospace; }}
.help-desc {{ color: #cfcfcf; font-size: 14px; }}
.help-footer {{ color: #7d8794; font-size: 12px; }}
"""


class LibraryPage(Gtk.Box):
    """The start screen: recent scripts, a test script, and a file picker."""

    def __init__(self, *, on_open, on_test, on_choose):
        super().__init__(orientation=Gtk.Orientation.VERTICAL, spacing=16)
        self.add_css_class("library")
        self._on_open = on_open
        self._on_test = on_test
        self._on_choose = on_choose
        self._msg_source = 0
        self._first_card: Gtk.Widget | None = None
        self._build()

    def _build(self) -> None:
        header = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=18)
        title_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=3)
        title_box.set_hexpand(True)

        title = Gtk.Label(label="Teleprompter")
        title.add_css_class("library-title")
        title.set_xalign(0.0)
        subtitle = Gtk.Label(label="Pick up a recent script, or open one from disk.")
        subtitle.add_css_class("library-subtitle")
        subtitle.set_xalign(0.0)
        title_box.append(title)
        title_box.append(subtitle)

        open_content = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        open_content.append(Gtk.Image.new_from_icon_name("document-open-symbolic"))
        open_content.append(Gtk.Label(label="Open script…"))
        self.open_btn = Gtk.Button()
        self.open_btn.set_child(open_content)
        self.open_btn.add_css_class("open-script")
        self.open_btn.set_valign(Gtk.Align.CENTER)
        self.open_btn.set_tooltip_text("Choose a script file (Ctrl+O)")
        self.open_btn.connect("clicked", lambda *_: self._on_choose())

        header.append(title_box)
        header.append(self.open_btn)
        self.append(header)

        self.msg = Gtk.Label(label="")
        self.msg.add_css_class("library-msg")
        self.msg.set_xalign(0.0)
        self.msg.set_ellipsize(Pango.EllipsizeMode.END)
        self.msg.set_visible(False)
        self.append(self.msg)

        scroller = Gtk.ScrolledWindow()
        scroller.set_policy(Gtk.PolicyType.NEVER, Gtk.PolicyType.AUTOMATIC)
        scroller.set_vexpand(True)
        self.content = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=12)
        self.content.set_margin_top(2)
        self.content.set_margin_bottom(8)
        scroller.set_child(self.content)
        self.append(scroller)

        self.recent_heading = self._section("RECENT SCRIPTS")
        self.content.append(self.recent_heading)

        self.recent_flow = self._flow()
        self.content.append(self.recent_flow)

        self.empty = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=4)
        self.empty.add_css_class("empty-recents")
        self.empty.set_halign(Gtk.Align.START)
        empty_title = Gtk.Label(label="No recent scripts yet")
        empty_title.add_css_class("empty-title")
        empty_title.set_xalign(0.0)
        empty_hint = Gtk.Label(label="Open a script and it will appear here next time.")
        empty_hint.add_css_class("empty-hint")
        empty_hint.set_xalign(0.0)
        self.empty.append(empty_title)
        self.empty.append(empty_hint)
        self.content.append(self.empty)

        self.content.append(self._section("ALWAYS AVAILABLE"))
        self.sample_flow = self._flow()
        self.content.append(self.sample_flow)

        hint = Gtk.Label(label="Ctrl+O open  ·  Ctrl+R refresh  ·  Ctrl+Q quit")
        hint.add_css_class("library-hint")
        hint.set_xalign(0.0)
        self.append(hint)

    @staticmethod
    def _section(text: str) -> Gtk.Label:
        label = Gtk.Label(label=text)
        label.add_css_class("library-section")
        label.set_xalign(0.0)
        return label

    @staticmethod
    def _flow() -> Gtk.FlowBox:
        flow = Gtk.FlowBox()
        flow.set_selection_mode(Gtk.SelectionMode.NONE)
        flow.set_max_children_per_line(3)
        flow.set_min_children_per_line(1)
        flow.set_column_spacing(14)
        flow.set_row_spacing(14)
        flow.set_homogeneous(True)
        flow.set_valign(Gtk.Align.START)
        return flow

    def set_recents(self, entries: list[dict]) -> None:
        self.recent_flow.remove_all()
        self._first_card = None
        for entry in entries:
            path = Path(entry["path"])
            meta = f"Opened {format_age(entry['opened'])}"
            read_time = read_script_time(path)
            if read_time is not None:
                meta = f"{read_time} · {meta}"
            else:
                meta = f"needs timing · {meta}"
            card = self._script_card(
                title=path.stem or path.name,
                subtitle=display_path(path.parent),
                meta=meta,
                tooltip=str(path),
                on_click=lambda p=path: self._on_open(p),
            )
            self.recent_flow.append(card)
            if self._first_card is None:
                self._first_card = card
        self.recent_flow.set_visible(bool(entries))
        self.empty.set_visible(not entries)

        sample_text = sample_script_text()
        sample_time = script_read_time(sample_text) or "needs timing"
        self.sample_flow.remove_all()
        self.sample_flow.append(self._script_card(
            title="Test script",
            subtitle="Built-in demo score · read-only",
            meta=f"{sample_time} · Always available",
            tooltip="Open the built-in test script",
            on_click=self._on_test,
            sample=True,
        ))

    def _script_card(self, *, title: str, subtitle: str, meta: str,
                     tooltip: str, on_click, sample: bool = False) -> Gtk.Button:
        button = Gtk.Button()
        button.add_css_class("script-card")
        if sample:
            button.add_css_class("sample-card")
        button.set_tooltip_text(tooltip)
        button.set_can_focus(True)
        button.set_size_request(300, -1)
        button.connect("clicked", lambda *_: on_click())

        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=5)
        top = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        icon = Gtk.Image.new_from_icon_name(
            "starred-symbolic" if sample else "text-x-generic-symbolic"
        )
        icon.set_valign(Gtk.Align.CENTER)
        top.append(icon)
        spacer = Gtk.Box()
        spacer.set_hexpand(True)
        top.append(spacer)
        if sample:
            badge = Gtk.Label(label="SAMPLE")
            badge.add_css_class("sample-badge")
            badge.set_valign(Gtk.Align.CENTER)
            top.append(badge)

        title_label = Gtk.Label(label=title)
        title_label.add_css_class("card-title")
        title_label.set_xalign(0.0)
        title_label.set_ellipsize(Pango.EllipsizeMode.END)
        title_label.set_max_width_chars(32)

        path_label = Gtk.Label(label=subtitle)
        path_label.add_css_class("card-path")
        path_label.set_xalign(0.0)
        path_label.set_ellipsize(Pango.EllipsizeMode.MIDDLE)
        path_label.set_max_width_chars(40)

        meta_label = Gtk.Label(label=meta)
        meta_label.add_css_class("card-meta")
        meta_label.set_xalign(0.0)

        box.append(top)
        box.append(title_label)
        box.append(path_label)
        box.append(meta_label)
        button.set_child(box)
        return button

    def focus_default(self) -> None:
        if self._first_card is not None:
            self._first_card.grab_focus()
        else:
            self.open_btn.grab_focus()

    def show_message(self, text: str, *, error: bool = False, seconds: int = 6) -> None:
        if self._msg_source:
            GLib.source_remove(self._msg_source)
            self._msg_source = 0
        self.msg.set_text(text)
        self.msg.set_tooltip_text(text)
        self.msg.set_visible(bool(text))
        if error:
            self.msg.add_css_class("error")
        else:
            self.msg.remove_css_class("error")
        if not text:
            return

        def clear():
            self._msg_source = 0
            self.msg.set_text("")
            self.msg.set_tooltip_text("")
            self.msg.set_visible(False)
            return GLib.SOURCE_REMOVE

        self._msg_source = GLib.timeout_add_seconds(seconds, clear)

    def clear_message(self) -> None:
        if self._msg_source:
            GLib.source_remove(self._msg_source)
            self._msg_source = 0
        self.msg.set_text("")
        self.msg.set_tooltip_text("")
        self.msg.set_visible(False)
        self.msg.remove_css_class("error")


class PrompterWindow(Gtk.ApplicationWindow):
    def __init__(self, *, application, script_path, cue, recorder_args, windowed=False):
        title = f"Teleprompter — {Path(script_path).name}" if script_path else "Teleprompter"
        super().__init__(application=application, title=title)
        self.set_default_size(1180, 760)

        self._windowed = bool(windowed)
        self.script_path: Path | None = Path(script_path) if script_path else None
        self.is_sample = False
        self._load_error = ""
        self.duration = 0.0  # the sum of the script's word times
        self.cue_fraction = max(CUE_MIN, min(CUE_MAX, float(cue)))
        self.pos = 0.0
        self.playing = False
        self.centered = True
        self.plan: list[Paragraph] = []
        self.paragraph_starts: list[float] = []
        self.timeline = Timeline()
        self.total_weight = 0.0
        self._w = 0.0
        self._anchor_w = 0.0
        self._anchor_t = time.monotonic()
        self._bands_dirty = True
        self._layout_dirty = True
        self._layout_size = (0, 0)
        self._adj_written = 0.0
        self._last_press: dict[int, float] = {}
        self._inhibit_cookie: int | None = None
        self._msg_source = 0
        self._file_dialog = None
        self._rec_phase = "idle"  # idle → recording → finalizing → idle
        self._at_end = False
        self._status_text = ""
        self._hud_dim_source = 0
        self.help_visible = False

        # Karaoke fill: unread words are grey, the active word sweeps from grey to its
        # final colour over exactly its authored duration, and finished words stay lit.
        self.script_text = ""
        self.karaoke_on = True
        self.mood_on = True
        self._active_word = -1
        self._last_paint_w: float | None = None
        self._karaoke_dirty = True
        self.flat_words: list[Word] = []
        self.flat_word_labels: list[Gtk.Widget] = []
        self._label_indices: dict[Gtk.Widget, list[int]] = {}
        self._direction_labels: set[Gtk.Widget] = set()
        self.flat_word_clocks: list[float] = []
        self.has_themes = False
        self._theme_text = ""
        self._section_index = -1
        self._section_text = ""

        self.recorder = Recorder(out_dir=RECORDINGS_DIR, **recorder_args)

        # The library is the start screen. The old script.txt becomes the first
        # recent on a fresh install so the file the app used to open is not lost.
        if RECENTS_FILE.exists():
            self.recents = load_recents()
        else:
            self.recents = []
            if DEFAULT_SCRIPT.is_file():
                self.recents = remember_recent([], DEFAULT_SCRIPT)
                save_recents(self.recents)

        self._build_ui()
        self._apply_css()
        self._sync_rec_ui()

        if self.script_path is not None and not self.open_script(self.script_path):
            self.script_path = None
            self.set_title("Teleprompter")

        motion = Gtk.EventControllerMotion()
        motion.set_propagation_phase(Gtk.PropagationPhase.CAPTURE)
        motion.connect("motion", self._on_pointer_motion)
        self.add_controller(motion)

        self.connect("close-request", self._on_close_request)
        self._tick_source = GLib.timeout_add(TICK_MS, self.tick)

    # ------------------------------------------------------------------ UI

    def _build_ui(self) -> None:
        font_px = self.font_px = self._compute_font_px()
        margin_x = self.margin_x = round(font_px * 1.0)
        margin_y = round(font_px * 1.1)  # stand-in until _apply_layout() knows the viewport
        self.side_margin = margin_x
        self.para_spacing = round(font_px * PARA_GAP_RATIO)
        self.half_line = font_px * LINE_HEIGHT_RATIO / 2  # eyes rest on the line's centre

        # One label per paragraph: the scroll engine needs each paragraph's pixel band
        # to give spoken text its speaking time and directions just a beat.
        self.para_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=self.para_spacing)
        self.para_box.set_margin_start(margin_x)
        self.para_box.set_margin_end(margin_x)
        self.para_box.set_margin_top(margin_y)
        self.para_box.set_margin_bottom(margin_y + round(font_px * 0.6))

        self.scrolled = Gtk.ScrolledWindow()
        self.scrolled.set_policy(Gtk.PolicyType.NEVER, Gtk.PolicyType.AUTOMATIC)
        self.scrolled.set_child(self.para_box)
        self.scrolled.get_vscrollbar().set_visible(False)
        self.scrolled.set_hexpand(True)
        self.scrolled.set_vexpand(True)
        self.adj = self.scrolled.get_vadjustment()
        self.adj.connect("changed", self._on_adj_changed)

        self._build_hud()
        self._build_section_card()
        self._build_cue_line(font_px, margin_x)

        # Read progress: a hairline along the bottom edge, clear of every control.
        self.progress = Gtk.ProgressBar()
        self.progress.add_css_class("read-progress")
        self.progress.set_can_target(False)
        self.progress.set_halign(Gtk.Align.FILL)
        self.progress.set_valign(Gtk.Align.END)

        # While recording, a red frame makes it impossible to forget you are live.
        self.rec_frame = Gtk.Box()
        self.rec_frame.add_css_class("rec-frame")
        self.rec_frame.set_can_target(False)
        self.rec_frame.set_visible(False)

        # The help card: the whole key map, without leaving the app.
        # A CenterBox, so the card is centred on both axes (a plain Box would pack it
        # against the leading edge and ignore the card's own halign).
        self.help_backdrop = Gtk.CenterBox()
        self.help_backdrop.add_css_class("help-backdrop")
        self.help_backdrop.set_visible(False)
        self.help_backdrop.set_center_widget(self._build_help_card())
        click = Gtk.GestureClick()
        click.connect("pressed", lambda *_: self.hide_help())
        self.help_backdrop.add_controller(click)

        self.overlay = overlay = Gtk.Overlay()
        overlay.set_child(self.scrolled)
        overlay.add_overlay(self.cue_marker)
        overlay.add_overlay(self.progress)
        overlay.add_overlay(self.hud)
        overlay.add_overlay(self.section_card)
        overlay.add_overlay(self.rec_frame)
        overlay.add_overlay(self.help_backdrop)

        self.library = LibraryPage(
            on_open=self.open_script,
            on_test=self.open_test_script,
            on_choose=self.choose_script,
        )
        self.library.set_recents(self.recents)

        self.stack = Gtk.Stack()
        self.stack.add_named(overlay, "prompter")
        self.stack.add_named(self.library, "library")
        self.stack.set_visible_child_name("library")
        self.set_child(self.stack)

    def _build_hud(self) -> None:
        self.hud = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=12)
        self.hud.add_css_class("hud")
        self.hud.set_halign(Gtk.Align.FILL)
        self.hud.set_valign(Gtk.Align.START)
        self.hud.set_margin_top(10)
        self.hud.set_margin_start(10)
        self.hud.set_margin_end(10)

        left = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=10)
        left.set_hexpand(True)
        left.set_halign(Gtk.Align.START)

        home_content = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=6)
        home_content.append(Gtk.Image.new_from_icon_name("view-grid-symbolic"))
        home_content.append(Gtk.Label(label="Scripts"))
        self.home_btn = Gtk.Button()
        self.home_btn.set_child(home_content)
        self.home_btn.set_can_focus(False)
        self.home_btn.add_css_class("flat")
        self.home_btn.set_tooltip_text("Back to the script library (Ctrl+R)")
        self.home_btn.connect("clicked", lambda *_: self.go_home())

        self.rec_icon = Gtk.Image.new_from_icon_name("media-record-symbolic")
        self.rec_label = Gtk.Label(label="Record")
        rec_content = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=6)
        rec_content.append(self.rec_icon)
        rec_content.append(self.rec_label)
        self.rec_btn = Gtk.Button()
        self.rec_btn.set_child(rec_content)
        self.rec_btn.set_can_focus(False)
        self.rec_btn.add_css_class("flat")
        self.rec_btn.set_tooltip_text("Start / stop recording (R)")
        self.rec_btn.connect("clicked", lambda *_: self.toggle_recording())

        self.state_label = Gtk.Label(label="Ready")
        self.state_label.add_css_class("state")
        self.state_label.set_valign(Gtk.Align.CENTER)
        self.state_label.set_tooltip_text("Scroll state: Ready → Playing → Paused → Done")

        self.mood_label = Gtk.Label(label="")
        self.mood_label.add_css_class("state")
        self.mood_label.set_valign(Gtk.Align.CENTER)
        self.mood_label.set_visible(False)
        self.mood_label.set_ellipsize(Pango.EllipsizeMode.END)
        self.mood_label.set_max_width_chars(32)
        self.mood_label.set_tooltip_text(
            "Current mood and theme — and the time left in this theme"
        )

        self.rec_dot = Gtk.Box()
        self.rec_dot.add_css_class("rec-dot")
        self.rec_dot.set_valign(Gtk.Align.CENTER)
        self.rec_dot.set_visible(False)

        self.rec_state = Gtk.Label(label="")
        self.rec_state.set_valign(Gtk.Align.CENTER)
        self.rec_state.set_visible(False)

        self.msg = Gtk.Label(label="")
        self.msg.set_valign(Gtk.Align.CENTER)
        self.msg.set_ellipsize(Pango.EllipsizeMode.END)

        for widget in (self.home_btn, self.rec_btn, self.state_label, self.mood_label,
                       self.rec_dot, self.rec_state, self.msg):
            left.append(widget)

        right = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=10)
        right.set_valign(Gtk.Align.CENTER)
        self.dur_label = Gtk.Label(label=format_time(self.duration))
        self.dur_label.set_valign(Gtk.Align.CENTER)
        self.dur_label.set_tooltip_text("Calculated from the word times in the script")
        self.cue_btn = Gtk.Button(label=self._cue_label())
        self.cue_btn.set_can_focus(False)
        self.cue_btn.add_css_class("flat")
        self.cue_btn.set_tooltip_text(
            "Reading line — rest your eyes here, level with the camera. "
            "Click or press C to cycle positions, Shift+↑/↓ to fine-tune."
        )
        self.cue_btn.connect("clicked", lambda *_: self.cycle_cue())
        self.rem_label = Gtk.Label(label="")
        self.rem_label.set_valign(Gtk.Align.CENTER)
        right.append(self.rem_label)
        right.append(self.cue_btn)
        right.append(self.dur_label)

        self.hud.append(left)
        self.hud.append(right)

    def _build_section_card(self) -> None:
        """The floating countdown for a standalone comment or silent section."""
        card = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=5)
        card.add_css_class("section-card")
        card.set_halign(Gtk.Align.CENTER)
        card.set_valign(Gtk.Align.START)
        card.set_margin_top(58)
        card.set_can_target(False)
        card.set_visible(False)

        row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=12)
        self.section_title_label = Gtk.Label(label="")
        self.section_title_label.add_css_class("section-title")
        self.section_title_label.set_xalign(0.0)
        self.section_title_label.set_ellipsize(Pango.EllipsizeMode.END)
        self.section_title_label.set_max_width_chars(34)
        self.section_title_label.set_hexpand(True)
        self.section_time_label = Gtk.Label(label="")
        self.section_time_label.add_css_class("section-time")
        self.section_time_label.set_xalign(1.0)
        row.append(self.section_title_label)
        row.append(self.section_time_label)

        self.section_bar = Gtk.ProgressBar()
        self.section_bar.add_css_class("section-progress")
        self.section_bar.set_can_target(False)
        card.append(row)
        card.append(self.section_bar)
        self.section_card = card

    def _build_cue_line(self, font_px: int, margin_x: int) -> None:
        """Two bars in the side margins marking the reading line, clear of the text column."""
        self.cue_bar_h = bar_h = max(3, round(font_px * 0.09))
        bar_w = max(12, margin_x - max(6, round(font_px * 0.18)))
        left_bar, right_bar = Gtk.Box(), Gtk.Box()
        for bar in (left_bar, right_bar):
            bar.set_size_request(bar_w, bar_h)
            bar.add_css_class("cue-bar")
        gutter = Gtk.Box()
        gutter.set_hexpand(True)
        self.cue_marker = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL)
        self.cue_marker.set_halign(Gtk.Align.FILL)
        self.cue_marker.set_valign(Gtk.Align.START)
        self.cue_marker.set_can_target(False)  # never swallows wheel or click events
        for child in (left_bar, gutter, right_bar):
            self.cue_marker.append(child)

    def _build_help_card(self) -> Gtk.Widget:
        card = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=16)
        card.add_css_class("help-card")
        card.set_halign(Gtk.Align.CENTER)
        card.set_valign(Gtk.Align.CENTER)

        title = Gtk.Label(label="Keyboard shortcuts")
        title.add_css_class("help-title")
        title.set_xalign(0.0)
        card.append(title)

        grid = Gtk.Grid(column_spacing=28, row_spacing=9)
        for row, (keys, description) in enumerate(KEY_HELP):
            key_label = Gtk.Label(label=keys)
            key_label.add_css_class("help-key")
            key_label.set_xalign(1.0)
            desc_label = Gtk.Label(label=description)
            desc_label.add_css_class("help-desc")
            desc_label.set_xalign(0.0)
            grid.attach(key_label, 0, row, 1, 1)
            grid.attach(desc_label, 1, row, 1, 1)
        card.append(grid)

        footer = Gtk.Label(label="Esc or ? closes this card")
        footer.add_css_class("help-footer")
        footer.set_xalign(0.0)
        card.append(footer)
        return card

    def _compute_font_px(self) -> int:
        display = Gdk.Display.get_default()
        if display is not None:
            monitors = display.get_monitors()
            if monitors.get_n_items() > 0:
                height = monitors.get_item(0).get_geometry().height
                return max(28, min(120, round(height * FONT_HEIGHT_RATIO)))
        return 56

    def _apply_css(self) -> None:
        provider = Gtk.CssProvider()
        provider.load_from_string(build_css(self.font_px))
        display = Gdk.Display.get_default()
        if display is not None:
            Gtk.StyleContext.add_provider_for_display(
                display, provider, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION
            )

    # --------------------------------------------------------- script text

    @property
    def script_name(self) -> str:
        if self.is_sample:
            return "Test script"
        return self.script_path.name if self.script_path is not None else ""

    def on_prompter(self) -> bool:
        return self.stack.get_visible_child_name() == "prompter"

    def _read_script_text(self) -> tuple[str | None, str]:
        if self.is_sample:
            text = sample_script_text()
            if text.strip():
                return text, ""
            return None, "The built-in test script is unavailable"
        if self.script_path is None:
            return None, "No script selected"
        try:
            return self.script_path.read_text(encoding="utf-8"), ""
        except FileNotFoundError:
            return None, f"Script not found: {self.script_path}"
        except (OSError, UnicodeError) as exc:
            return None, f"Cannot read script: {exc}"

    def _apply_script(self, text: str) -> list[str]:
        """Validate *text*, replace the current score and reset the read to the top.

        Parsing happens before any widget or state is touched, so a score with
        incomplete timing leaves the previous script exactly where it was.
        """
        warnings: list[str] = []
        plan = paragraph_plan(text, warnings)
        self.script_text = text
        self.plan = plan
        self.paragraph_starts = [p.start for p in self.plan]
        self._section_index = -1
        self._section_text = ""
        self.section_card.set_visible(False)
        self.duration = sum(p.seconds for p in self.plan)
        self.dur_label.set_text(format_time(self.duration))
        self.flat_words = [word for p in self.plan for word in p.words]
        self.flat_word_clocks = [word.clock for word in self.flat_words]
        self.has_themes = any(word.theme for word in self.flat_words)
        self._theme_text = ""
        self._active_word = -1
        self._last_paint_w = None
        self._karaoke_dirty = True

        child = self.para_box.get_first_child()
        while child is not None:
            self.para_box.remove(child)
            child = self.para_box.get_first_child()
        labels = []
        for paragraph in self.plan:
            label = self._paragraph_label(paragraph)
            labels.append(label)
            self.para_box.append(label)
        self.flat_word_labels = []
        self._label_indices = {}
        self._direction_labels = set()
        base = 0
        for paragraph, label in zip(self.plan, labels):
            indices = list(range(base, base + len(paragraph.words)))
            self._label_indices[label] = indices
            if paragraph.is_direction:
                self._direction_labels.add(label)
            self.flat_word_labels.extend([label] * len(paragraph.words))
            base += len(paragraph.words)

        self.playing = False
        self.total_weight = sum(p.seconds for p in self.plan)
        self.timeline.clear(self.total_weight)
        self._bands_dirty = True
        self._layout_dirty = True
        self._at_end = False
        self._status_text = ""
        self.pos = 0.0
        self._w = 0.0
        self.adj.set_value(0.0)
        self._adj_written = 0.0
        self._last_press.clear()
        self.hide_help()
        self.progress.set_visible(bool(self.plan))
        self.progress.set_fraction(0.0)
        self._undim_hud()
        self._update_inhibit()
        self._sync_karaoke(force=True)
        return warnings

    def _show_warnings(self, warnings: list[str]) -> None:
        if warnings:
            extra = f" (+{len(warnings) - 1} more)" if len(warnings) > 1 else ""
            self.show_message(f"{warnings[0]}{extra}", error=True, seconds=8)

    def _open_error(self, text: str) -> None:
        if self.on_prompter():
            self._undim_hud()
            self.show_message(text, error=True)
        else:
            self.library.show_message(text, error=True)

    def open_script(self, script_path: Path, *, remember: bool = True) -> bool:
        """Open a script from the library, the file picker or the command line."""
        if self._rec_phase != "idle":
            self._open_error("Stop recording before changing scripts")
            return False

        path = Path(script_path).expanduser()
        try:
            path = path.resolve()
        except (OSError, RuntimeError):
            path = path.absolute()
        try:
            text = path.read_text(encoding="utf-8")
        except FileNotFoundError:
            self._open_error(f"Script not found: {path}")
            self._forget_recent(path)
            return False
        except (OSError, UnicodeError) as exc:
            self._open_error(f"Cannot read script: {exc}")
            return False

        try:
            warnings = self._apply_script(text)
        except ScriptTimingError as exc:
            self._open_error(f"{path.name}: {exc.summary}")
            if not self.on_prompter():
                self.set_title("Teleprompter")
            return False
        self.script_path = path
        self.is_sample = False
        self.set_title(f"Teleprompter — {path.name}")
        self._show_prompter()
        self._show_warnings(warnings)
        if remember:
            self.recents = remember_recent(self.recents, path)
            save_recents(self.recents)
            self.library.set_recents(self.recents)
        return True

    def open_test_script(self) -> bool:
        if self._rec_phase != "idle":
            self._open_error("Stop recording before changing scripts")
            return False
        text = sample_script_text()
        if not text.strip():
            self._open_error("The built-in test script is unavailable")
            return False
        try:
            warnings = self._apply_script(text)
        except ScriptTimingError as exc:
            self._open_error(f"Test script: {exc.summary}")
            return False
        self.script_path = None
        self.is_sample = True
        self.set_title("Teleprompter — Test script")
        self._show_prompter()
        self._show_warnings(warnings)
        return True

    def load_script(self, *, notify: bool = True) -> bool:
        """Re-read the current script (F5 / L) without leaving the prompter."""
        text, error = self._read_script_text()
        if text is None:
            self.show_message(error, error=True)
            return False
        try:
            warnings = self._apply_script(text)
        except ScriptTimingError as exc:
            self.show_message(exc.summary, error=True, seconds=10)
            return False
        self._show_warnings(warnings)
        if not warnings and notify:
            words = sum(1 for word in self.flat_words
                        if word.text and not word.is_comment)
            comments = sum(1 for word in self.flat_words if word.is_comment)
            self.show_message(
                f"Reloaded {self.script_name} ({words} words, "
                f"{comments} comments, {format_time(self.duration)})"
            )
        return True

    def _show_prompter(self) -> None:
        self.stack.set_visible_child_name("prompter")
        self._layout_dirty = True
        self._bands_dirty = True
        self._undim_hud()
        if not self._windowed and self.get_visible() and not self.is_fullscreen():
            self.fullscreen()

    def go_home(self) -> bool:
        """Return to the script library, pausing the read and leaving fullscreen."""
        if self._rec_phase != "idle":
            self._undim_hud()
            self.show_message("Stop recording before returning to the library", error=True)
            return False
        if self.playing:
            self.toggle_play()
        self.hide_help()
        before = len(self.recents)
        self.recents = [entry for entry in self.recents
                        if Path(entry["path"]).is_file()]
        if len(self.recents) != before:
            save_recents(self.recents)
        self.library.set_recents(self.recents)
        self.library.clear_message()
        self.stack.set_visible_child_name("library")
        self.set_title("Teleprompter")
        if self.is_fullscreen():
            self.unfullscreen()
        self._update_inhibit()
        self.library.focus_default()
        return True

    def refresh_library(self) -> None:
        self.recents = load_recents()
        self.library.set_recents(self.recents)
        self.library.show_message("Recent scripts refreshed")

    def _forget_recent(self, path: Path) -> None:
        key = _path_key(path)
        before = len(self.recents)
        self.recents = [entry for entry in self.recents if entry["path"] != key]
        if len(self.recents) != before:
            save_recents(self.recents)
            self.library.set_recents(self.recents)

    def choose_script(self) -> None:
        if self._rec_phase != "idle":
            self._open_error("Stop recording before opening another script")
            return
        if self.playing:
            self.toggle_play()

        dialog = Gtk.FileDialog()
        dialog.set_title("Open a script")
        dialog.set_modal(True)
        scripts = Gtk.FileFilter()
        scripts.set_name("Script files")
        for pattern in ("*.txt", "*.md", "*.text"):
            scripts.add_pattern(pattern)
        every = Gtk.FileFilter()
        every.set_name("All files")
        every.add_pattern("*")
        filters = Gio.ListStore.new(Gtk.FileFilter)
        filters.append(scripts)
        filters.append(every)
        dialog.set_filters(filters)
        dialog.set_default_filter(scripts)

        folder = None
        if self.script_path is not None:
            folder = self.script_path.parent
        elif self.recents:
            folder = Path(self.recents[0]["path"]).parent
        if folder is not None and folder.is_dir():
            dialog.set_initial_folder(Gio.File.new_for_path(str(folder)))
        self._file_dialog = dialog
        dialog.open(self, None, self._on_script_chosen)

    def _on_script_chosen(self, dialog, result) -> None:
        self._file_dialog = None
        try:
            file = dialog.open_finish(result)
        except GLib.Error:
            return  # dismissed or cancelled
        path = file.get_path() if file is not None else None
        if path is None:
            self._open_error("That file is not on the local disk")
            return
        self.open_script(Path(path))

    def _label_alignment(self, label: Gtk.Label) -> None:
        """Line alignment inside the column: centred by default, flush left on request."""
        centered = self.centered
        label.set_xalign(0.5 if centered else 0.0)
        label.set_justify(Gtk.Justification.CENTER if centered else Gtk.Justification.LEFT)

    def _apply_alignment(self) -> None:
        child = self.para_box.get_first_child()
        while child is not None:
            self._label_alignment(child)
            child = child.get_next_sibling()

    def toggle_alignment(self) -> None:
        self.centered = not self.centered
        self._apply_alignment()
        self.show_message("Text: centred" if self.centered else "Text: left-aligned")

    def _paragraph_label(self, paragraph: Paragraph) -> Gtk.Label:
        label = Gtk.Label()
        self._label_alignment(label)
        label.set_wrap(True)
        label.set_wrap_mode(Pango.WrapMode.WORD_CHAR)
        label.set_selectable(False)
        label.add_css_class("script")
        label.set_markup(paragraph.markup or " ")
        if paragraph.is_direction:
            label.add_css_class("direction")
        return label

    def _lit_colour(self, word: Word) -> str:
        """The colour a word reaches once it has been read completely.

        Comments are annotations, not lyrics: they always use the amber family and
        never inherit a mood colour. `M` therefore affects spoken words only.
        """
        if word.is_comment:
            return COMMENT_LIT
        return mood_colour(word.mood) if self.mood_on else KARAOKE_NEUTRAL

    @staticmethod
    def _insert_foreground(attrs: Pango.AttrList, start: int, end: int,
                           colour: str) -> None:
        if end <= start:
            return
        red, green, blue = (int(colour[i:i + 2], 16) for i in (1, 3, 5))
        attr = Pango.attr_foreground_new(red * 257, green * 257, blue * 257)
        attr.start_index = start
        attr.end_index = end
        attrs.insert(attr)

    @staticmethod
    def _insert_background(attrs: Pango.AttrList, start: int, end: int,
                           colour: str) -> None:
        if end <= start:
            return
        red, green, blue = (int(colour[i:i + 2], 16) for i in (1, 3, 5))
        attr = Pango.attr_background_new(red * 257, green * 257, blue * 257)
        attr.start_index = start
        attr.end_index = end
        attrs.insert(attr)

    def _paint_word(self, attrs: Pango.AttrList, index: int) -> None:
        """Give one word its current fill state, using Pango's byte offsets."""
        word = self.flat_words[index]
        if not word.text:
            return
        start = word.byte_start
        end = start + len(word.text.encode("utf-8"))

        if word.is_comment:
            # Annotations never look like lyrics: always amber and italic, with a
            # uniform dim-to-bright beat instead of the spoken word's left-to-right
            # sweep. Inline comments also get a quiet chip behind the brackets.
            progress = word_fill(word.clock, word.seconds, self._w)
            colour = comment_colour(progress) if self.karaoke_on else COMMENT_LIT
            self._insert_foreground(attrs, start, end, colour)
            italic = Pango.attr_style_new(Pango.Style.ITALIC)
            italic.start_index = start
            italic.end_index = end
            attrs.insert(italic)
            if self.flat_word_labels[index] not in self._direction_labels:
                self._insert_background(attrs, start, end, COMMENT_BG)
            return

        lit = self._lit_colour(word)
        if not self.karaoke_on:
            self._insert_foreground(attrs, start, end, lit)
        else:
            progress = word_fill(word.clock, word.seconds, self._w)
            if progress <= 0.0:
                self._insert_foreground(attrs, start, end, KARAOKE_GRAY)
            elif progress >= 1.0:
                self._insert_foreground(attrs, start, end, lit)
            else:
                offset = start
                count = len(word.text)
                for number, character in enumerate(word.text):
                    next_offset = offset + len(character.encode("utf-8"))
                    cover = character_fill(progress, number, count)
                    self._insert_foreground(
                        attrs, offset, next_offset,
                        mix_hex(KARAOKE_GRAY, lit, cover),
                    )
                    offset = next_offset

    def _paint_label(self, label: Gtk.Widget) -> None:
        attrs = Pango.AttrList()
        for index in self._label_indices.get(label, ()):
            self._paint_word(attrs, index)
        label.set_attributes(attrs)

    def _paint_all(self) -> None:
        for label in self._label_indices:
            self._paint_label(label)
        self._last_paint_w = self._w

    def _sync_karaoke(self, *, force: bool = False) -> None:
        """Repaint only what changed: the active word and its neighbours.

        The active label gets a fresh sweep every tick; past/future labels are only
        repainted when a word boundary, seek or toggle changes their state.
        """
        if not self.flat_words:
            return
        if not self.karaoke_on:
            if force or self._karaoke_dirty:
                self._paint_all()
                self._karaoke_dirty = False
            return

        active = bisect_right(self.flat_word_clocks, self._w) - 1
        active = max(0, min(active, len(self.flat_words) - 1))
        dirty = force or self._karaoke_dirty

        if dirty:
            self._paint_all()
        elif active != self._active_word:
            if self._active_word < 0 or abs(active - self._active_word) > 2:
                self._paint_all()
            else:
                labels = {
                    self.flat_word_labels[self._active_word],
                    self.flat_word_labels[active],
                }
                for label in labels:
                    self._paint_label(label)
        elif (self._last_paint_w is None
              or abs(self._w - self._last_paint_w) > 1e-9):
            self._paint_label(self.flat_word_labels[active])

        self._active_word = active
        self._last_paint_w = self._w
        self._karaoke_dirty = False

    # ------------------------------------------------------------- scrolling

    def max_scroll(self) -> float:
        return max(0.0, self.adj.get_upper() - self.adj.get_page_size())

    def page_step(self) -> float:
        return max(60.0, self.adj.get_page_size() * 0.15)

    def _rate(self) -> float:
        """Reading weight consumed per second — the one speed that makes it fit."""
        if self.total_weight <= 0 or self.duration <= 0:
            return 0.0
        return self.total_weight / self.duration

    def _reanchor(self, now: float | None = None) -> None:
        self._anchor_w = self._w
        self._anchor_t = time.monotonic() if now is None else now

    def _advance(self, now: float) -> None:
        if self.playing:
            self._w = self._anchor_w + (now - self._anchor_t) * self._rate()

    def _on_adj_changed(self, _adj) -> None:
        self._bands_dirty = True

    def _rebuild_bands(self) -> bool:
        """Measure the labels and rebuild the word-level scroll timeline.

        Every word gets a knot at the y of the visual line where it starts, so words
        sharing a line share a scroll position and the line dwells for the sum of
        their author-given times. Highlighting is driven by the word clock instead of
        these pixel bands, so a wrapped comment cannot steal or leak time.
        """
        if not self.plan:
            return False
        first = self.para_box.get_first_child()
        if first is None:
            return False

        width = first.get_width()
        if width <= 0:
            width = self.scrolled.get_width() - 2 * self.side_margin
        if width <= 0:
            return False

        cue_y = float(self.para_box.get_margin_top()) + self.half_line
        tops = []
        heights = []
        y = float(self.para_box.get_margin_top())
        child = first
        labels: list[Gtk.Widget] = []
        for _ in self.plan:
            if child is None:
                return False
            labels.append(child)
            tops.append(y)
            height = child.measure(Gtk.Orientation.VERTICAL, width)[1]
            heights.append(height)
            y += height + self.para_spacing
            child = child.get_next_sibling()

        entries: list[tuple[float, float]] = []
        next_line = 0
        first_line: int | None = None
        for paragraph, label, top, height in zip(self.plan, labels, tops, heights):
            layout = label.get_layout()
            ranges: list[tuple[int, int]] = []
            if layout is not None:
                for line in layout.get_lines():
                    start = line.start_index
                    ranges.append((start, start + line.length))
            if not ranges:
                text = layout.get_text() if layout is not None else paragraph.text
                ranges = [(0, len(text.encode("utf-8")))]
            line_h = height / len(ranges) if ranges else 0.0
            centers = [top + (number + 0.5) * line_h
                       for number in range(len(ranges))]
            line_start = next_line
            next_line += len(ranges)
            line_starts = [start for start, _end in ranges]
            for word in paragraph.words:
                if not line_starts:
                    line_number = 0
                else:
                    # Pango line ranges and attributes are UTF-8 byte offsets.
                    line_number = bisect_right(line_starts, word.byte_start) - 1
                    line_number = max(0, min(line_number, len(ranges) - 1))
                line_index = line_start + line_number
                if first_line is None:
                    first_line = line_index
                position = centers[line_number] - cue_y
                if line_index == first_line:
                    position = 0.0  # the first line starts exactly on the reading line
                entries.append((position, word.seconds))

        self.timeline.rebuild_entries(entries, max_scroll=self.max_scroll())
        self.total_weight = self.timeline.total_weight
        self.pos = self.pos_from_weight(self._w)
        return True

    # ------------------------------------------------------- karaoke fill

    def toggle_spotlight(self) -> None:
        self.karaoke_on = not self.karaoke_on
        self._active_word = -1
        self._last_paint_w = None
        self._karaoke_dirty = True
        self._sync_karaoke(force=True)
        self.show_message("Word karaoke on" if self.karaoke_on
                          else "Word karaoke off")

    def toggle_mood(self) -> None:
        self.mood_on = not self.mood_on
        self._active_word = -1
        self._last_paint_w = None
        self._karaoke_dirty = True
        self._sync_karaoke(force=True)
        self.show_message("Mood colours on" if self.mood_on else "Mood colours off")

    def _sync_theme_chip(self) -> None:
        """Mood, theme and the time left in that theme — one label, no churn."""
        if not self.has_themes or not self.flat_words:
            if self.mood_label.get_visible():
                self.mood_label.set_visible(False)
                self._theme_text = ""
            return
        index = bisect_right(self.flat_word_clocks, self._w) - 1
        index = max(0, min(index, len(self.flat_words) - 1))
        word = self.flat_words[index]
        if not word.theme:
            if self.mood_label.get_visible():
                self.mood_label.set_visible(False)
                self._theme_text = ""
            return
        end = self.total_weight
        for later in self.flat_words[index:]:
            if later.theme != word.theme:
                end = later.clock
                break
        remaining = max(0.0, end - self._w)
        text = " · ".join((word.mood, word.theme, format_time(remaining)))
        if text != self._theme_text:
            self._theme_text = text
            self.mood_label.set_text(text)
        if not self.mood_label.get_visible():
            self.mood_label.set_visible(True)

    def _hide_section(self) -> None:
        if self.section_card.get_visible():
            self.section_card.set_visible(False)
        self._section_index = -1
        self._section_text = ""

    def _sync_section(self, *, force: bool = False) -> None:
        """Show a countdown while a standalone comment/silent beat is active."""
        if (self._at_end or not self.hud.get_visible()
                or self.total_weight <= 0):
            self._hide_section()
            return
        state = section_state(self.plan, self._w, self.paragraph_starts)
        if state is None or state.remaining <= 1e-9:
            self._hide_section()
            return

        title = section_title(state.paragraph)
        text = (f"{format_time(state.remaining)} left of "
                f"{format_time(state.paragraph.seconds)}")
        if force or state.index != self._section_index:
            self._section_index = state.index
            self._section_text = text
            self.section_title_label.set_text(title)
            self.section_time_label.set_text(text)
            self.section_bar.set_fraction(state.progress)
            if not self.section_card.get_visible():
                self.section_card.set_visible(True)
        else:
            if text != self._section_text:
                self._section_text = text
                self.section_time_label.set_text(text)
            self.section_bar.set_fraction(state.progress)

    def pos_from_weight(self, weight: float) -> float:
        return self.timeline.pos_from_weight(weight)

    def weight_from_pos(self, pos: float) -> float:
        return self.timeline.weight_from_pos(pos)

    def seek(self, value: float) -> None:
        self.pos = max(0.0, min(value, self.max_scroll()))
        self._w = self.weight_from_pos(self.pos)
        self._karaoke_dirty = True
        self._reanchor()

    def nudge(self, delta: float) -> None:
        self.seek(self.pos + delta)

    def _adopt_scrolled_change(self, now: float) -> None:
        """Take over a wheel / trackpad scroll: the new place becomes the timeline anchor.

        The check compares against `_adj_written` — the last value the app itself wrote —
        so the programmatic seeks from arrows and Home/End are never mistaken for a scrub.
        """
        value = self.adj.get_value()
        if abs(value - self._adj_written) <= 0.5:
            return
        self.pos = max(0.0, min(value, self.max_scroll()))
        self._w = self.weight_from_pos(self.pos)
        self._karaoke_dirty = True
        self._reanchor(now)

    def toggle_play(self) -> None:
        now = time.monotonic()
        if self.playing:
            self._advance(now)
            self.playing = False
            self._reanchor(now)
            self._undim_hud()
            self._update_inhibit()
            return
        max_scroll = self.max_scroll()
        if max_scroll <= 0 or self.total_weight <= 0:
            self.show_message("Text fits on screen — nothing to scroll")
            return
        if self._w >= self.total_weight - 1e-6:
            self._w = 0.0
            self.pos = 0.0
            self._karaoke_dirty = True
        self._at_end = False
        self.playing = True
        self._reanchor(now)
        self._schedule_hud_dim()
        self._update_inhibit()

    # ------------------------------------------------------------ reading line

    def _cue_label(self) -> str:
        return f"─ {round(self.cue_fraction * 100)}%"

    def set_cue(self, fraction: float) -> None:
        """Move the reading line — fraction of the viewport height from the top."""
        self.cue_fraction = max(CUE_MIN, min(CUE_MAX, fraction))
        self.cue_btn.set_label(self._cue_label())
        self._layout_dirty = True

    def cycle_cue(self) -> None:
        nxt = next((p for p in CUE_PRESETS if p > self.cue_fraction + 1e-6), CUE_PRESETS[0])
        self.set_cue(nxt)

    def _apply_layout(self) -> bool:
        """Place the centred text column and the reading line for the current viewport.

        The column is centred and at most COLUMN_RATIO of the width, so the words stay near
        the middle of the screen — where the camera is — instead of running edge to edge.
        The top padding then puts the middle of the first text line on the reading line when
        the scroll sits at zero; the matching bottom padding is what lets the middle of the
        last line climb up to the same line by the time the read ends.
        """
        page = int(self.adj.get_page_size())
        width = self.scrolled.get_width()
        if page <= 0 or width <= 0:
            return False
        cue_y = round(page * self.cue_fraction)
        top = max(0, cue_y - round(self.half_line))
        self.para_box.set_margin_top(top)
        self.para_box.set_margin_bottom(max(0, page - top))
        self.cue_marker.set_margin_top(max(0, cue_y - self.cue_bar_h // 2))
        self.side_margin = max(self.margin_x, round(width * (1 - COLUMN_RATIO) / 2))
        self.para_box.set_margin_start(self.side_margin)
        self.para_box.set_margin_end(self.side_margin)
        self._layout_size = (page, width)
        self._layout_dirty = False
        return True

    # ------------------------------------------------------------- recording
    #
    # A take moves through three phases: recording → finalizing → idle. While ffmpeg
    # flushes the file (writing `q` to its stdin, normally well under a second) the
    # button is disabled, so a quick second press cannot start a take whose live UI
    # would then be reset by the previous take's callback.

    def toggle_recording(self) -> None:
        if self._rec_phase == "recording":
            self._stop_recording()
        elif self._rec_phase == "finalizing":
            self.show_message("Finalizing the previous take…")
        else:
            self._start_recording()

    def _start_recording(self) -> None:
        try:
            path = self.recorder.start()
        except (RuntimeError, OSError) as exc:
            self.show_message(f"Recording failed: {exc}", error=True)
            return
        self._rec_phase = "recording"
        self._sync_rec_ui()
        self.show_message(f"Recording → {path.name}")
        self._update_inhibit()

    def _stop_recording(self) -> None:
        self._rec_phase = "finalizing"
        self._sync_rec_ui()
        self.recorder.stop(self._finish_recording)
        self._update_inhibit()

    def _finish_recording(self, path, code) -> None:
        self._rec_phase = "idle"
        self._sync_rec_ui()
        if code == 0 and path is not None and path.exists() and path.stat().st_size > 0:
            self.show_message(f"Saved {path.name} ({human_size(path.stat().st_size)})")
        else:
            tail = self.recorder.last_error()
            self.show_message(f"Recording failed: {tail or f'ffmpeg exit code {code}'}", error=True)
        self._update_inhibit()

    def _sync_rec_ui(self) -> None:
        """Drive button, dot, timer and frame from the one record phase."""
        phase = self._rec_phase
        if phase == "recording":
            icon, text = "media-playback-stop-symbolic", "Stop"
        elif phase == "finalizing":
            icon, text = "media-record-symbolic", "Finalizing…"
        else:
            icon, text = "media-record-symbolic", "Record"
        self.rec_icon.set_from_icon_name(icon)
        self.rec_label.set_text(text)
        self.rec_btn.set_sensitive(phase != "finalizing")
        if phase == "recording":
            self.rec_btn.add_css_class("rec-on")
            self.rec_state.add_css_class("rec-on")
            self.rec_state.set_text("REC 0:00")
        else:
            self.rec_btn.remove_css_class("rec-on")
            self.rec_state.remove_css_class("rec-on")
            self.rec_state.set_text("")
        self.rec_dot.set_visible(phase == "recording")
        self.rec_state.set_visible(phase == "recording")
        self.rec_frame.set_visible(phase != "idle")

    # ------------------------------------------------------------ tick loop

    def tick(self) -> bool:
        now = time.monotonic()

        if self._rec_phase == "recording":
            code = self.recorder.poll()
            if code is not None:
                path = self.recorder.path
                self.recorder.take_proc()
                self._finish_recording(path, code)
            else:
                self.rec_state.set_text(f"REC {format_time(now - self.recorder.started_at)}")

        # The library has no scroll engine; leave the prompter's state untouched while it
        # is hidden (its viewport is not measurable then anyway).
        if not self.on_prompter():
            return GLib.SOURCE_CONTINUE

        # The first ticks land before the window has a viewport; place the reading line and
        # the text column as soon as there is one, and again whenever the window is resized.
        # Skip the band rebuild that same tick: the adjustment still carries the old
        # content height, so the knots would be computed against a stale scroll range.
        size = (int(self.adj.get_page_size()), self.scrolled.get_width())
        applied = self._apply_layout() if (
            self._layout_dirty or size != self._layout_size
        ) else False
        if applied:
            self._bands_dirty = True

        max_scroll = self.max_scroll()
        if not applied and self._bands_dirty and max_scroll > 0 and self._rebuild_bands():
            self._bands_dirty = False

        # A wheel or trackpad move is adopted in both states — while playing it scrubs the
        # clock forward and the read continues from there. Layout changes are skipped: the
        # adjustment shifting under a resize is not a scrub.
        if not applied:
            self._adopt_scrolled_change(now)

        if self.playing:
            self._advance(now)
            if self.total_weight > 0 and self._w >= self.total_weight:
                self._w = self.total_weight
                self.playing = False
                self._at_end = True
                self._undim_hud()
                self._update_inhibit()
                self.show_message("Done")
            self.pos = self.pos_from_weight(self._w)
        elif self._at_end and self._w < self.total_weight - 1e-6:
            self._at_end = False

        self.adj.set_value(self.pos)
        self._adj_written = self.adj.get_value()
        self.progress.set_fraction(
            min(1.0, self._w / self.total_weight) if self.total_weight > 0 else 0.0
        )
        self._update_remaining(max_scroll)
        self._sync_karaoke()
        self._sync_theme_chip()
        self._sync_section()
        self._sync_status()
        return GLib.SOURCE_CONTINUE

    def _update_remaining(self, max_scroll: float) -> None:
        if max_scroll <= 0 or self.total_weight <= 0 or (not self.playing and self.pos < 0.5):
            self.rem_label.set_text("")
            self.rem_label.remove_css_class("warn")
            self.rem_label.remove_css_class("danger")
            return
        remaining = max(0.0, (1.0 - self._w / self.total_weight) * self.duration)
        self.rem_label.set_text(f"{format_time(remaining)} left")
        danger = remaining <= REMAIN_DANGER_S
        warn = not danger and remaining <= REMAIN_WARN_S
        self._set_class(self.rem_label, "danger", danger)
        self._set_class(self.rem_label, "warn", warn)

    def _sync_status(self) -> None:
        """Keep the state chip in step with the scroll engine, without churning labels."""
        if not self.plan:
            text = "No script"
        elif self.playing:
            text = "Playing"
        elif self._at_end:
            text = "Done"
        elif self._w > 1e-6:
            text = "Paused"
        else:
            text = "Ready"
        if text != self._status_text:
            self._status_text = text
            self.state_label.set_text(text)

    @staticmethod
    def _set_class(widget: Gtk.Widget, name: str, wanted: bool) -> None:
        if wanted:
            widget.add_css_class(name)
        else:
            widget.remove_css_class(name)

    # ------------------------------------------------------------ hud dimming

    def _schedule_hud_dim(self) -> None:
        if self._hud_dim_source == 0 and self.playing and self.hud.get_visible():
            self._hud_dim_source = GLib.timeout_add_seconds(HUD_DIM_AFTER_S, self._dim_hud)

    def _dim_hud(self) -> bool:
        self._hud_dim_source = 0
        if self.playing:
            self.hud.add_css_class("dimmed")
        return GLib.SOURCE_REMOVE

    def _undim_hud(self) -> None:
        if self._hud_dim_source:
            GLib.source_remove(self._hud_dim_source)
            self._hud_dim_source = 0
        self.hud.remove_css_class("dimmed")

    def _on_pointer_motion(self, _controller, _x, _y) -> None:
        if self._hud_dim_source:
            return  # a countdown is already running
        self.hud.remove_css_class("dimmed")
        self._schedule_hud_dim()

    # -------------------------------------------------------------------- help

    def show_help(self) -> None:
        self.help_visible = True
        self.help_backdrop.set_visible(True)

    def hide_help(self) -> None:
        self.help_visible = False
        self.help_backdrop.set_visible(False)

    def toggle_help(self) -> None:
        self.hide_help() if self.help_visible else self.show_help()

    # ------------------------------------------------------------- messages

    def show_message(self, text: str, *, error: bool = False, seconds: int = 6) -> None:
        if self._msg_source:
            GLib.source_remove(self._msg_source)
            self._msg_source = 0
        self.msg.set_text(text)
        self.msg.set_tooltip_text(text)
        if error:
            self.msg.add_css_class("error")
        else:
            self.msg.remove_css_class("error")

        def clear():
            self._msg_source = 0
            self.msg.set_text("")
            self.msg.set_tooltip_text("")
            return GLib.SOURCE_REMOVE

        self._msg_source = GLib.timeout_add_seconds(seconds, clear)

    # ------------------------------------------------------------------ keys

    def _is_repeat(self, keyval: int) -> bool:
        now = time.monotonic()
        last = self._last_press.get(keyval, 0.0)
        self._last_press[keyval] = now
        return now - last < REPEAT_GUARD_S

    def on_key(self, _controller, keyval, _keycode, state) -> bool:
        ctrl = bool(state & Gdk.ModifierType.CONTROL_MASK)

        if ctrl and keyval in (Gdk.KEY_q, Gdk.KEY_Q):
            self.close()
            return True

        if self.help_visible:
            self.hide_help()  # any key dismisses the card and is swallowed
            return True

        if ctrl and keyval in (Gdk.KEY_r, Gdk.KEY_R):
            if self._is_repeat(keyval):
                return True
            if self.on_prompter():
                self.go_home()
            else:
                self.refresh_library()
            return True
        if ctrl and keyval in (Gdk.KEY_o, Gdk.KEY_O):
            if self._is_repeat(keyval):
                return True
            self.choose_script()
            return True

        if not self.on_prompter():
            return False  # let the library buttons handle Space/Enter/Tab

        toggle_keys = (
            Gdk.KEY_space, Gdk.KEY_r, Gdk.KEY_R, Gdk.KEY_f, Gdk.KEY_F,
            Gdk.KEY_h, Gdk.KEY_H, Gdk.KEY_l, Gdk.KEY_L,
            Gdk.KEY_c, Gdk.KEY_C, Gdk.KEY_a, Gdk.KEY_A, Gdk.KEY_F5,
            Gdk.KEY_k, Gdk.KEY_K, Gdk.KEY_m, Gdk.KEY_M,
            Gdk.KEY_F1, Gdk.KEY_F11, Gdk.KEY_question,
        )
        if keyval in toggle_keys and self._is_repeat(keyval):
            return True

        if keyval == Gdk.KEY_space:
            self.toggle_play()
        elif keyval in (Gdk.KEY_r, Gdk.KEY_R):
            self.toggle_recording()
        elif keyval in (Gdk.KEY_f, Gdk.KEY_F, Gdk.KEY_F11):
            if self.is_fullscreen():
                self.unfullscreen()
            else:
                self.fullscreen()
        elif keyval == Gdk.KEY_Escape:
            if self.is_fullscreen():
                self.unfullscreen()
        elif keyval in (Gdk.KEY_question, Gdk.KEY_F1):
            self.show_help()
        elif keyval in (Gdk.KEY_h, Gdk.KEY_H):
            visible = not self.hud.get_visible()
            self.hud.set_visible(visible)
            if visible:
                self._schedule_hud_dim()
            else:
                self._undim_hud()
            self._sync_section(force=True)
        elif keyval in (Gdk.KEY_c, Gdk.KEY_C):
            self.cycle_cue()
        elif keyval in (Gdk.KEY_a, Gdk.KEY_A):
            self.toggle_alignment()
        elif keyval in (Gdk.KEY_k, Gdk.KEY_K):
            self.toggle_spotlight()
        elif keyval in (Gdk.KEY_m, Gdk.KEY_M):
            self.toggle_mood()
        elif keyval in (Gdk.KEY_l, Gdk.KEY_L, Gdk.KEY_F5):
            self.load_script(notify=True)
        elif keyval == Gdk.KEY_Up:
            self.set_cue(self.cue_fraction - CUE_STEP)
        elif keyval == Gdk.KEY_Down:
            self.set_cue(self.cue_fraction + CUE_STEP)
        elif keyval == Gdk.KEY_Left:
            self.nudge(-self.page_step())
        elif keyval == Gdk.KEY_Right:
            self.nudge(self.page_step())
        elif keyval == Gdk.KEY_Home:
            self.seek(0.0)
        elif keyval == Gdk.KEY_End:
            self.seek(self.max_scroll())
        else:
            return False
        return True

    # -------------------------------------------------------------- lifecycle

    def _update_inhibit(self) -> None:
        # On Wayland, GTK turns the IDLE flag into zwp_idle_inhibit_manager_v1 on the
        # window's surface; neither the desktop portal nor a systemd-inhibit lock is
        # involved (swayidle does not watch the latter).
        want = self.playing or self.recorder.active
        app = self.get_application()
        if app is None:
            return
        if want and self._inhibit_cookie is None:
            try:
                self._inhibit_cookie = app.inhibit(
                    self, Gtk.ApplicationInhibitFlags.IDLE, "Teleprompter"
                )
            except Exception:
                self._inhibit_cookie = 0  # best effort; don't retry
        elif not want and self._inhibit_cookie is not None:
            if self._inhibit_cookie:
                try:
                    app.uninhibit(self._inhibit_cookie)
                except Exception:
                    pass
            self._inhibit_cookie = None

    def _on_close_request(self, _window) -> bool:
        GLib.source_remove(self._tick_source)
        if self._hud_dim_source:
            GLib.source_remove(self._hud_dim_source)
            self._hud_dim_source = 0
        self.recorder.stop_blocking()
        return False


class TeleprompterApp(Gtk.Application):
    def __init__(self, args):
        super().__init__(application_id=APP_ID, flags=Gio.ApplicationFlags.NON_UNIQUE)
        self.args = args
        self.window: PrompterWindow | None = None
        self.connect("activate", self._on_activate)

    def _on_activate(self, _app) -> None:
        if self.window is None:
            self.window = PrompterWindow(
                application=self,
                script_path=self.args.script,
                cue=self.args.line / 100.0,
                windowed=self.args.windowed,
                recorder_args={
                    "device": self.args.device,
                    "audio": self.args.audio,
                    "size": self.args.size,
                    "fps": self.args.fps,
                    "crf": CRF,
                },
            )
            key_controller = Gtk.EventControllerKey()
            key_controller.set_propagation_phase(Gtk.PropagationPhase.CAPTURE)
            key_controller.connect("key-pressed", self.window.on_key)
            self.window.add_controller(key_controller)
        self.window.present()
        if self.window.on_prompter() and not self.args.windowed:
            self.window.fullscreen()


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description="Fullscreen teleprompter with webcam recording.")
    parser.add_argument("script", nargs="?", default=None,
                        help="path to a script to open directly; omit it to show the "
                             "script library")
    parser.add_argument("--line", type=int, default=round(CUE_DEFAULT * 100), metavar="PCT",
                        help="reading line height, as a percentage of the screen from the "
                             "top — e.g. 15 with a camera above the screen, 50 with a "
                             "camera behind its centre (default: %(default)s)")
    parser.add_argument("--device", default=VIDEO_DEVICE, help="v4l2 camera device")
    parser.add_argument("--audio", default=AUDIO_DEVICE, help="PulseAudio/PipeWire source")
    parser.add_argument("--size", type=parse_size, default=VIDEO_SIZE,
                        help=f"capture size, e.g. 1280x720 (default: {VIDEO_SIZE}; even numbers only)")
    parser.add_argument("--fps", type=int, default=FPS, help="capture frame rate")
    parser.add_argument("--windowed", action="store_true",
                        help="keep the prompter windowed instead of fullscreen")
    parser.add_argument("--print-timing", action="store_true",
                        help="print the summed word-time score for the script and exit")
    parser.add_argument("--words", action="store_true",
                        help="with --print-timing, list every timed word")
    parser.add_argument("--check", action="store_true",
                        help="open the camera and microphone for a second to catch busy "
                             "devices before a take, then exit")
    return parser.parse_args(argv)


def main(argv=None) -> int:
    args = parse_args(argv)
    if args.print_timing:
        return print_timing(Path(args.script) if args.script else DEFAULT_SCRIPT, args.words)
    if args.check:
        return check_devices(device=args.device, audio=args.audio, size=args.size, fps=args.fps)
    app = TeleprompterApp(args)
    return app.run([])


if __name__ == "__main__":
    raise SystemExit(main())
