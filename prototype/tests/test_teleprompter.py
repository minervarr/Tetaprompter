#!/usr/bin/env python3
"""Unit tests for teleprompter.py — the pure logic only.

No window and no display are needed: the tests cover strict word/comment timing
(errors and source lines), UTF-8 byte offsets, comments and silent beats, mood tags,
markup, karaoke fill math, formatting, ffmpeg error picking, argument validation and
both weight ↔ pixel timelines.

    python3 -m unittest discover -s tests -t . -v
"""

from __future__ import annotations

import argparse
import contextlib
import io
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path

# The app stays at the repository root; tests live in tests/.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import teleprompter as tp


class StrictTimingTests(unittest.TestCase):
    def timing_error(self, text: str) -> tp.ScriptTimingError:
        with self.assertRaises(tp.ScriptTimingError) as raised:
            tp.paragraph_plan(text)
        return raised.exception

    def test_missing_word_time_is_an_error_with_its_line(self):
        error = self.timing_error("[0.4]One two.")
        self.assertEqual(len(error.issues), 1)
        issue = error.issues[0]
        self.assertEqual((issue.line, issue.paragraph, issue.kind),
                         (1, 1, "word"))
        self.assertEqual(issue.text, "two.")
        self.assertIn("no [seconds]", issue.reason)
        self.assertIn("line 1", error.summary)

    def test_missing_comment_time_is_an_error(self):
        error = self.timing_error("[0.4]One.\n\n[smile — wait]")
        self.assertEqual(error.issues[0].line, 3)
        self.assertEqual(error.issues[0].kind, "comment")
        self.assertIn("[smile", error.issues[0].text)

    def test_zero_duration_is_rejected_for_words_and_comments(self):
        word_error = self.timing_error("[0]One.")
        self.assertIn("zero duration", word_error.issues[0].reason)

        comment_error = self.timing_error("[0.0] [smile]")
        self.assertIn("zero duration", comment_error.issues[0].reason)

    def test_all_missing_tokens_are_reported_together(self):
        error = self.timing_error("[0.2]One two three.")
        self.assertEqual([issue.text for issue in error.issues],
                         ["two", "three."])
        self.assertIn("+1 more", error.summary)

    def test_multiline_paragraph_reports_the_token_line(self):
        error = self.timing_error(
            "[0.4]One.\n\n[0.5]Two.\n\n[0.6]Three four."
        )
        self.assertEqual(error.issues[0].line, 5)
        self.assertEqual(error.issues[0].text, "four.")

    def test_legacy_anchors_are_timing_errors(self):
        error = self.timing_error("[0:00] [0.4]One.")
        self.assertEqual(error.issues[0].kind, "anchor")
        self.assertEqual(error.issues[0].line, 1)

        bare = self.timing_error("[0:20]\n\n[0.4]Next.")
        self.assertTrue(any(issue.kind == "anchor" for issue in bare.issues))

    def test_mood_header_needs_no_time(self):
        plan = tp.paragraph_plan(
            "[0.4]Intro.\n\n{warm: Opening}\n\n[0.5]Body."
        )
        self.assertEqual(len(plan), 2)
        self.assertEqual((plan[1].mood, plan[1].theme), ("warm", "Opening"))
        self.assertEqual(plan[1].start, 0.4)

    def test_word_prefixes_are_stripped_and_summed(self):
        plan = tp.paragraph_plan(
            "{warm}[0.45]Good [0.30]morning, [0.55]everyone!"
        )
        self.assertEqual(len(plan), 1)
        paragraph = plan[0]
        self.assertEqual(paragraph.text, "Good morning, everyone!")
        self.assertAlmostEqual(paragraph.seconds, 1.30)
        self.assertEqual(paragraph.start, 0.0)
        self.assertEqual((paragraph.mood, paragraph.theme), ("warm", ""))
        self.assertFalse(paragraph.is_direction)
        self.assertEqual([word.text for word in paragraph.words],
                         ["Good", "morning,", "everyone!"])
        self.assertEqual([round(word.seconds, 2) for word in paragraph.words],
                         [0.45, 0.30, 0.55])
        self.assertEqual([word.clock for word in paragraph.words],
                         [0.0, 0.45, 0.75])
        self.assertEqual([word.char_start for word in paragraph.words], [0, 5, 14])
        self.assertEqual([word.byte_start for word in paragraph.words], [0, 5, 14])

    def test_paragraph_starts_follow_the_summed_word_times(self):
        plan = tp.paragraph_plan("[0.4]One [0.6]two.\n\n[0.8]Three.")
        self.assertEqual([round(p.start, 2) for p in plan], [0.0, 1.0])
        self.assertEqual([round(p.seconds, 2) for p in plan], [1.0, 0.8])
        self.assertAlmostEqual(sum(p.seconds for p in plan), 1.8)

    def test_comments_are_timed_direction_paragraphs(self):
        plan = tp.paragraph_plan("[0.4]One [0.6]two.\n\n[1.0] [smile]\n\n[0.8]Three.")
        self.assertEqual(len(plan), 3)
        comment = plan[1]
        self.assertTrue(comment.is_direction)
        self.assertEqual(comment.text, "[smile]")
        self.assertEqual(comment.seconds, 1.0)
        self.assertEqual(plan[2].start, 2.0)

    def test_timed_comment_and_silent_beat(self):
        comment = tp.paragraph_plan("[1.5] [look at the camera]")[0]
        self.assertTrue(comment.is_direction)
        self.assertEqual(comment.text, "[look at the camera]")
        self.assertAlmostEqual(comment.seconds, 1.5)

        silence = tp.paragraph_plan("[2.0]")[0]
        self.assertTrue(silence.is_direction)
        self.assertEqual(silence.text, " ")
        self.assertAlmostEqual(silence.seconds, 2.0)
        self.assertEqual(silence.words[0].text, "")

    def test_timed_comment_owns_its_full_clock(self):
        plan = tp.paragraph_plan(
            "[0.4]Before.\n\n[1.5] [look]\n\n[0.3]After."
        )
        flat = [word for paragraph in plan for word in paragraph.words]
        comment_index = next(i for i, word in enumerate(flat) if word.is_comment)
        comment = flat[comment_index]
        self.assertAlmostEqual(comment.clock, 0.4)
        self.assertAlmostEqual(comment.seconds, 1.5)
        self.assertAlmostEqual(flat[comment_index + 1].clock, 1.9)

    def test_inline_comment_has_its_own_time(self):
        paragraph = tp.paragraph_plan("[0.2]Hi [0.3][smile] [0.3]there.")[0]
        self.assertFalse(paragraph.is_direction)
        self.assertEqual(paragraph.text, "Hi [smile] there.")
        self.assertAlmostEqual(paragraph.seconds, 0.8)
        self.assertEqual([word.text for word in paragraph.words],
                         ["Hi", "[smile]", "there."])
        self.assertEqual([word.char_start for word in paragraph.words], [0, 3, 11])
        self.assertAlmostEqual(paragraph.words[1].seconds, 0.3)

    def test_wrapped_old_directions_need_a_time(self):
        with self.assertRaises(tp.ScriptTimingError):
            tp.paragraph_plan("**[Smile.]**")

        timed = tp.paragraph_plan("[1.5] **[Look.]**")[0]
        self.assertEqual(timed.text, "[Look.]")
        self.assertAlmostEqual(timed.seconds, 1.5)

    def test_mood_and_theme_persist_across_words_and_paragraphs(self):
        plan = tp.paragraph_plan(
            "{warm: Opening}[0.1]One.\n\n[0.2]Two.\n\n{somber}[0.3]Three."
        )
        self.assertEqual((plan[0].mood, plan[0].theme), ("warm", "Opening"))
        self.assertEqual((plan[1].mood, plan[1].theme), ("warm", "Opening"))
        self.assertEqual((plan[2].mood, plan[2].theme), ("somber", "Opening"))

    def test_mood_can_change_between_words(self):
        paragraph = tp.paragraph_plan(
            "{warm}[0.2]One {somber}[0.3]Two."
        )[0]
        self.assertEqual(paragraph.mood, "warm")
        self.assertEqual([word.mood for word in paragraph.words], ["warm", "somber"])

    def test_unknown_mood_stays_literal_and_warns(self):
        warnings: list[str] = []
        paragraph = tp.paragraph_plan("[0.3]{happy} [0.4]Hello.", warnings)[0]
        self.assertEqual(paragraph.text, "{happy} Hello.")
        self.assertEqual(paragraph.mood, tp.MOOD_DEFAULT)
        self.assertTrue(any("unknown mood" in warning for warning in warnings))

    def test_custom_hex_mood(self):
        paragraph = tp.paragraph_plan("{#ff8800}[0.4]Hi.")[0]
        self.assertEqual(paragraph.mood, "#ff8800")
        self.assertEqual(tp.mood_colour("#ff8800"), "#ff8800")
        self.assertEqual(tp.mood_colour("not-a-colour"), tp.MOODS[tp.MOOD_DEFAULT])

    def test_markup_is_kept_out_of_the_plain_offsets(self):
        paragraph = tp.paragraph_plan("[0.4]**Bold** [0.3]*Italic*")[0]
        self.assertEqual(paragraph.text, "Bold Italic")
        self.assertEqual(paragraph.markup, "<b>Bold</b> <i>Italic</i>")
        self.assertEqual([word.text for word in paragraph.words], ["Bold", "Italic"])
        self.assertEqual([word.char_start for word in paragraph.words], [0, 5])

    def test_utf8_byte_offsets_differ_from_character_offsets(self):
        paragraph = tp.paragraph_plan("[0.3]café [0.2]— [0.3]lens.")[0]
        self.assertEqual([word.char_start for word in paragraph.words], [0, 5, 7])
        self.assertEqual([word.byte_start for word in paragraph.words], [0, 6, 10])

    def test_split_paragraphs_ignores_blank_runs(self):
        self.assertEqual(tp.split_paragraphs("\n\na\n\n\n\nb\n\n"), ["a", "b"])

    def test_defaults_keep_old_paragraph_construction_working(self):
        paragraph = tp.Paragraph("text", False, 1.0)
        self.assertEqual(paragraph.mood, tp.MOOD_DEFAULT)
        self.assertEqual(paragraph.theme, "")
        self.assertEqual(paragraph.words, ())
        self.assertEqual(paragraph.start, 0.0)
        self.assertEqual(paragraph.markup, "")


class SectionTimerTests(unittest.TestCase):
    def test_standalone_comment_is_a_section_with_its_full_clock(self):
        plan = tp.paragraph_plan(
            "[0.5]Start.\n\n[45.0] [Slide 1 is up. Wait for students.]\n\n[0.5]Next."
        )
        starts = [paragraph.start for paragraph in plan]
        start = tp.section_state(plan, 0.5, starts)
        self.assertIsNotNone(start)
        self.assertEqual(start.index, 1)
        self.assertAlmostEqual(start.remaining, 45.0)
        self.assertEqual(start.progress, 0.0)
        self.assertIn("Slide 1 is up", tp.section_title(start.paragraph))

        middle = tp.section_state(plan, 23.0, starts)
        self.assertIsNotNone(middle)
        self.assertAlmostEqual(middle.elapsed, 22.5)
        self.assertAlmostEqual(middle.remaining, 22.5)
        self.assertAlmostEqual(middle.progress, 0.5)

    def test_spoken_paragraph_has_no_section_timer(self):
        plan = tp.paragraph_plan(
            "[0.5]Start.\n\n[45.0] [Wait.]\n\n[0.5]Next."
        )
        self.assertIsNone(tp.section_state(plan, 45.5))

    def test_silent_beat_is_a_section_named_hold(self):
        plan = tp.paragraph_plan("[0.5]One.\n\n[15.0]\n\n[0.5]Two.")
        state = tp.section_state(plan, 0.5)
        self.assertIsNotNone(state)
        self.assertTrue(state.paragraph.is_direction)
        self.assertAlmostEqual(state.remaining, 15.0)
        self.assertEqual(tp.section_title(state.paragraph), "Hold")

    def test_inline_comment_does_not_start_a_section(self):
        plan = tp.paragraph_plan("[0.2]Hi [0.3][smile] [0.3]there.")
        self.assertIsNone(tp.section_state(plan, 0.0))

    def test_empty_plan_has_no_section(self):
        self.assertIsNone(tp.section_state([], 0.0))


class MarkupTests(unittest.TestCase):
    def test_escapes_and_styles(self):
        self.assertEqual(tp.to_markup("a < b & c"), "a &lt; b &amp; c")
        self.assertEqual(tp.to_markup("**bold** and *italic*"),
                         "<b>bold</b> and <i>italic</i>")

    def test_bracketed_text_stays_literal(self):
        self.assertEqual(tp.to_markup("[Count 2 seconds.]"), "[Count 2 seconds.]")
        self.assertEqual(tp.to_markup("Call me [later]"), "Call me [later]")


class FormatTests(unittest.TestCase):
    def test_format_time(self):
        self.assertEqual(tp.format_time(0), "0:00")
        self.assertEqual(tp.format_time(59.6), "1:00")
        self.assertEqual(tp.format_time(119.4), "1:59")
        self.assertEqual(tp.format_time(-3), "0:00")

    def test_human_size(self):
        self.assertEqual(tp.human_size(999), "999 B")
        self.assertEqual(tp.human_size(2048), "2.0 KB")
        self.assertEqual(tp.human_size(3 * 1024 ** 3), "3.0 GB")


class ScriptReadTimeTests(unittest.TestCase):
    def test_sums_words_and_timed_comments(self):
        text = "[1.0]Hello [1.0]world.\n\n[2.0] [look at the camera and breathe]"
        self.assertEqual(tp.script_read_time(text), "0:04")
        self.assertEqual(tp.script_read_time("[1.0]Hello [1.0]world."), "0:02")

    def test_incomplete_score_has_no_time(self):
        text = "[1.0]Hello.\n\n[smile — a comment is shown, but has no time.]"
        self.assertIsNone(tp.script_read_time(text))
        self.assertIsNone(tp.script_read_time("hello world"))

    def test_empty_script_is_zero(self):
        self.assertEqual(tp.script_read_time(""), "0:00")

    def test_sample_script_total(self):
        self.assertEqual(tp.script_read_time(tp.sample_script_text()), "0:23")

    def test_missing_unreadable_or_incomplete_file_has_no_time(self):
        with tempfile.TemporaryDirectory() as tmp:
            missing = Path(tmp) / "gone.txt"
            self.assertIsNone(tp.read_script_time(missing))
            broken = Path(tmp) / "broken.txt"
            broken.write_bytes(b"\xff\xfe\x00")
            self.assertIsNone(tp.read_script_time(broken))
            incomplete = Path(tmp) / "incomplete.txt"
            incomplete.write_text("[1.5]Hello. [smile]", encoding="utf-8")
            self.assertIsNone(tp.read_script_time(incomplete))
            good = Path(tmp) / "good.txt"
            good.write_text("[1.5]Hello.", encoding="utf-8")
            self.assertEqual(tp.read_script_time(good), "0:02")


class PrintTimingTests(unittest.TestCase):
    def test_valid_score_prints_total_and_returns_zero(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "good.txt"
            path.write_text("[1.0]Hello [1.0]world.\n\n[1.5] [smile]", encoding="utf-8")
            out, err = io.StringIO(), io.StringIO()
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                self.assertEqual(tp.print_timing(path), 0)
            self.assertIn("0:04", out.getvalue())
            self.assertNotIn("timing is incomplete", err.getvalue())

    def test_incomplete_score_exits_nonzero_and_names_the_line(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "bad.txt"
            path.write_text("[1.0]Hello.\n\n[smile]", encoding="utf-8")
            out, err = io.StringIO(), io.StringIO()
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                self.assertEqual(tp.print_timing(path), 1)
            self.assertIn("timing is incomplete", err.getvalue())
            self.assertIn("line 3: comment '[smile]' has no [seconds] time",
                          err.getvalue())


class ColourTests(unittest.TestCase):
    def test_scale_and_lighten(self):
        self.assertEqual(tp.scale_hex("#ffffff", 0.5), "#808080")
        self.assertEqual(tp.scale_hex("#ffffff", 0.0), "#000000")
        self.assertEqual(tp.lighten_hex("#000000", 0.5), "#808080")
        self.assertEqual(tp.lighten_hex("#000000", 0.0), "#000000")

    def test_mix_hex(self):
        self.assertEqual(tp.mix_hex("#000000", "#ffffff", 0.0), "#000000")
        self.assertEqual(tp.mix_hex("#000000", "#ffffff", 1.0), "#ffffff")
        self.assertEqual(tp.mix_hex("#000000", "#ffffff", 0.5), "#808080")
        self.assertEqual(tp.mix_hex("#ff0000", "#00ff00", 0.5), "#808000")

    def test_comments_use_their_own_uniform_amber_ramp(self):
        self.assertEqual(tp.comment_colour(0.0), tp.COMMENT_DIM)
        self.assertEqual(tp.comment_colour(1.0), tp.COMMENT_LIT)
        self.assertNotEqual(tp.comment_colour(0.0), tp.KARAOKE_GRAY)
        self.assertNotEqual(tp.comment_colour(0.5), tp.KARAOKE_GRAY)
        self.assertEqual(tp.comment_colour(-1.0), tp.COMMENT_DIM)
        self.assertEqual(tp.comment_colour(2.0), tp.COMMENT_LIT)

    def test_word_fill_progress(self):
        self.assertEqual(tp.word_fill(2.0, 1.5, 1.0), 0.0)
        self.assertEqual(tp.word_fill(2.0, 1.5, 2.0), 0.0)
        self.assertAlmostEqual(tp.word_fill(2.0, 1.5, 2.75), 0.5)
        self.assertEqual(tp.word_fill(2.0, 1.5, 3.5), 1.0)
        self.assertEqual(tp.word_fill(2.0, 1.5, 9.0), 1.0)
        self.assertEqual(tp.word_fill(2.0, 0.0, 1.9), 0.0)
        self.assertEqual(tp.word_fill(2.0, 0.0, 2.0), 1.0)

    def test_character_fill_sweeps_left_to_right(self):
        self.assertEqual([tp.character_fill(0.5, i, 4) for i in range(4)],
                         [1.0, 1.0, 0.0, 0.0])
        self.assertEqual([tp.character_fill(0.375, i, 4) for i in range(4)],
                         [1.0, 0.5, 0.0, 0.0])
        self.assertEqual([tp.character_fill(0.0, i, 4) for i in range(4)],
                         [0.0, 0.0, 0.0, 0.0])
        self.assertEqual([tp.character_fill(1.0, i, 4) for i in range(4)],
                         [1.0, 1.0, 1.0, 1.0])


class ErrorLineTests(unittest.TestCase):
    def test_prefers_the_specific_complaint(self):
        text = ("[in#0 @ 0x55bf] Cannot open video device /dev/video0: "
                "Device or resource busy\n"
                "[in#0 @ 0x55bf] Error opening input: Device or resource busy\n"
                "Error opening input file /dev/video0.\n")
        self.assertEqual(
            tp.pick_error_line(text),
            "Cannot open video device /dev/video0: Device or resource busy",
        )

    def test_falls_back_to_the_first_line(self):
        self.assertEqual(tp.pick_error_line("line one\nline two"), "line one")

    def test_empty_output(self):
        self.assertEqual(tp.pick_error_line("\n \n"), "")

    def test_clips_long_lines(self):
        self.assertEqual(len(tp.pick_error_line("error " + "x" * 500)), 200)


class TimelineTests(unittest.TestCase):
    """The paragraph-level timeline retained for the old layout math."""

    PAGE = 600.0
    HALF_LINE = 20.0
    CUE_Y = 150.0
    TOP = CUE_Y - HALF_LINE
    CONTENT = 1200.0
    TEXT_END = TOP + CONTENT
    MAX_SCROLL = CONTENT  # body height == scroll range in the real layout

    def _timeline(self, seconds):
        plan = [tp.Paragraph(f"p{i}", False, s) for i, s in enumerate(seconds)]
        tops = [self.TOP + i * 100.0 for i in range(len(plan))]
        timeline = tp.Timeline()
        timeline.rebuild(plan, tops=tops, text_end=self.TEXT_END, cue_y=self.CUE_Y,
                         max_scroll=self.MAX_SCROLL, half_line=self.HALF_LINE)
        return timeline

    def test_table_shape_and_monotonicity(self):
        timeline = self._timeline([3.0, 1.0, 5.0, 2.0])
        self.assertEqual(len(timeline.knots), len(timeline.cum))
        self.assertEqual(len(timeline.knots), 5)
        self.assertEqual(timeline.knots[0], 0.0)
        self.assertAlmostEqual(timeline.total_weight, 11.0)
        self.assertEqual(timeline.knots, sorted(timeline.knots))

    def test_last_knot_lands_the_final_line_on_the_reading_line(self):
        timeline = self._timeline([3.0, 1.0, 5.0, 2.0])
        self.assertAlmostEqual(timeline.knots[-1], self.MAX_SCROLL - 2 * self.HALF_LINE)

    def test_positions_are_exact_at_knots(self):
        timeline = self._timeline([3.0, 1.0, 5.0])
        for weight, knot in zip(timeline.cum, timeline.knots):
            self.assertAlmostEqual(timeline.pos_from_weight(weight), knot)

    def test_weight_and_position_round_trip(self):
        timeline = self._timeline([3.0, 1.0, 5.0])
        for weight in (0.0, 0.5, 2.9, 3.0, 4.25, 9.0):
            position = timeline.pos_from_weight(weight)
            self.assertAlmostEqual(timeline.weight_from_pos(position), weight, places=6)

    def test_positions_are_monotone_in_weight(self):
        timeline = self._timeline([3.0, 1.0, 5.0])
        positions = [timeline.pos_from_weight(i * 0.25) for i in range(37)]
        self.assertEqual(positions, sorted(positions))

    def test_last_paragraph_gets_real_travel(self):
        timeline = self._timeline([3.0, 4.0])
        self.assertGreater(timeline.knots[-1], timeline.knots[-2])

    def test_out_of_range_values_clamp(self):
        timeline = self._timeline([3.0, 1.0])
        self.assertEqual(timeline.pos_from_weight(-5.0), 0.0)
        self.assertEqual(timeline.pos_from_weight(99.0), timeline.knots[-1])
        self.assertEqual(timeline.weight_from_pos(-10.0), 0.0)
        self.assertEqual(timeline.weight_from_pos(1e9), timeline.total_weight)

    def test_empty_timeline_is_inert(self):
        timeline = tp.Timeline()
        self.assertEqual(timeline.pos_from_weight(3.0), 0.0)
        self.assertEqual(timeline.weight_from_pos(3.0), 0.0)
        timeline.rebuild([], tops=[], text_end=0.0, cue_y=0.0, max_scroll=0.0, half_line=0.0)
        self.assertEqual(timeline.knots, [0.0])


class WordTimelineTests(unittest.TestCase):
    """Word knots: several words can share one visual line and its dwell."""

    def _timeline(self, entries, max_scroll=20.0):
        timeline = tp.Timeline()
        timeline.rebuild_entries(entries, max_scroll=max_scroll)
        return timeline

    def test_repeated_positions_hold_a_line_for_its_words(self):
        timeline = self._timeline([(0.0, 1.0), (10.0, 1.0), (10.0, 2.0), (20.0, 1.0)])
        self.assertEqual(timeline.knots, [0.0, 0.0, 10.0, 10.0, 20.0])
        self.assertEqual(timeline.cum, [0.0, 1.0, 2.0, 4.0, 5.0])
        self.assertEqual(timeline.total_weight, 5.0)
        self.assertEqual(timeline.pos_from_weight(0.0), 0.0)
        self.assertEqual(timeline.pos_from_weight(0.5), 0.0)
        self.assertAlmostEqual(timeline.pos_from_weight(1.5), 5.0)
        self.assertEqual(timeline.pos_from_weight(2.0), 10.0)
        self.assertEqual(timeline.pos_from_weight(3.0), 10.0)
        self.assertEqual(timeline.pos_from_weight(4.0), 10.0)
        self.assertEqual(timeline.pos_from_weight(5.0), 20.0)

    def test_position_to_weight_at_a_repeated_knot_is_the_line_arrival(self):
        timeline = self._timeline([(0.0, 1.0), (10.0, 1.0), (10.0, 2.0), (20.0, 1.0)])
        self.assertEqual(timeline.weight_from_pos(0.0), 0.0)
        self.assertAlmostEqual(timeline.weight_from_pos(5.0), 1.5)
        self.assertEqual(timeline.weight_from_pos(10.0), 2.0)
        self.assertAlmostEqual(timeline.weight_from_pos(15.0), 4.5)
        self.assertEqual(timeline.weight_from_pos(20.0), 5.0)

    def test_entries_are_clamped_and_monotone(self):
        timeline = self._timeline([(0.0, 1.0), (50.0, 1.0), (10.0, 1.0)], max_scroll=30.0)
        self.assertEqual(timeline.knots, [0.0, 0.0, 30.0, 30.0])
        self.assertEqual(timeline.knots, sorted(timeline.knots))

    def test_empty_entries_clear_the_table(self):
        timeline = tp.Timeline()
        timeline.rebuild_entries([], max_scroll=10.0)
        self.assertEqual(timeline.knots, [0.0])
        self.assertEqual(timeline.total_weight, 0.0)


class ArgTests(unittest.TestCase):
    def test_parse_size_accepts_well_formed_sizes(self):
        self.assertEqual(tp.parse_size("1280x720"), "1280x720")
        self.assertEqual(tp.parse_size("1920x1200"), "1920x1200")

    def test_parse_size_rejects_bad_sizes(self):
        for bad in ("1280*720", "1280", "1280x", "1281x720", "1280x721", " 1280x720"):
            with self.assertRaises(argparse.ArgumentTypeError, msg=bad):
                tp.parse_size(bad)

    def test_parse_args_defaults_and_positional(self):
        args = tp.parse_args([])
        self.assertEqual(args.size, tp.VIDEO_SIZE)
        self.assertIsNone(args.script)  # no path: start on the script library
        self.assertFalse(args.check)
        self.assertFalse(args.print_timing)
        self.assertFalse(args.words)
        self.assertEqual(tp.parse_args(["myscript.txt"]).script, "myscript.txt")

    def test_parse_args_check_and_words_flags(self):
        self.assertTrue(tp.parse_args(["--check", "--device", "/dev/video2"]).check)
        self.assertTrue(tp.parse_args(["--print-timing", "--words"]).words)

    def test_duration_flag_is_gone(self):
        with contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit):
                tp.parse_args(["--duration", "60"])


class RecentsTests(unittest.TestCase):
    def test_missing_or_corrupt_cache_is_empty(self):
        with tempfile.TemporaryDirectory() as tmp:
            cache = Path(tmp) / "recents.json"
            self.assertEqual(tp.load_recents(cache), [])
            cache.write_text("{not json", encoding="utf-8")
            self.assertEqual(tp.load_recents(cache), [])

    def test_save_and_load_keep_newest_first(self):
        with tempfile.TemporaryDirectory() as tmp:
            first = Path(tmp) / "first.txt"
            second = Path(tmp) / "second.txt"
            first.write_text("x", encoding="utf-8")
            second.write_text("x", encoding="utf-8")
            cache = Path(tmp) / "recents.json"
            entries = [
                {"path": str(first), "opened": 10.0},
                {"path": str(second), "opened": 20.0},
            ]
            self.assertTrue(tp.save_recents(entries, cache))
            loaded = tp.load_recents(cache)
            self.assertEqual([Path(e["path"]).name for e in loaded],
                             ["second.txt", "first.txt"])

    def test_load_dedupes_and_drops_missing_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            script = Path(tmp) / "script.txt"
            script.write_text("x", encoding="utf-8")
            missing = Path(tmp) / "gone.txt"
            cache = Path(tmp) / "recents.json"
            cache.write_text(json.dumps({"version": 1, "scripts": [
                {"path": str(script), "opened": 1.0},
                {"path": str(script), "opened": 2.0},
                {"path": str(missing), "opened": 3.0},
                {"path": 42, "opened": 4.0},
            ]}), encoding="utf-8")
            loaded = tp.load_recents(cache)
            self.assertEqual(len(loaded), 1)
            self.assertEqual(Path(loaded[0]["path"]), script.resolve())

    def test_remember_moves_to_front_and_caps_the_list(self):
        with tempfile.TemporaryDirectory() as tmp:
            entries = [
                {"path": str(Path(tmp) / f"{i}.txt"), "opened": float(i)}
                for i in range(tp.RECENTS_MAX + 3)
            ]
            newest = Path(tmp) / "newest.txt"
            result = tp.remember_recent(entries, newest, now=999.0)
            self.assertEqual(result[0]["path"], str(newest.resolve()))
            self.assertEqual(result[0]["opened"], 999.0)
            self.assertEqual(len(result), tp.RECENTS_MAX)

            again = tp.remember_recent(result, newest, now=1000.0)
            self.assertEqual(len(again), tp.RECENTS_MAX)
            self.assertEqual(len([e for e in again if e["path"] == str(newest.resolve())]), 1)

    def test_format_age(self):
        base = 1_000_000.0
        self.assertEqual(tp.format_age(base, now=base + 10), "just now")
        self.assertEqual(tp.format_age(base, now=base + 120), "2 min ago")
        self.assertEqual(tp.format_age(base, now=base + 7200), "2 h ago")
        self.assertEqual(tp.format_age(base, now=base + 3 * 86400), "3 days ago")
        self.assertTrue(tp.format_age(base, now=base + 30 * 86400))

    def test_display_path_shortens_the_home_directory(self):
        path = Path.home() / "scripts" / "talk.txt"
        self.assertTrue(tp.display_path(path).startswith("~/"))


class SampleScriptTests(unittest.TestCase):
    def test_the_built_in_sample_is_present_and_fully_timed(self):
        self.assertTrue((tp.SAMPLES_DIR / tp.TEST_SCRIPT_NAME).is_file())
        text = tp.sample_script_text()
        plan = tp.paragraph_plan(text)
        self.assertGreaterEqual(len(plan), 3)
        self.assertGreater(sum(p.seconds for p in plan), 0)
        self.assertTrue(all(word.seconds > 0
                            for paragraph in plan for word in paragraph.words))
        self.assertEqual(tp.script_read_time(text), "0:23")

    def test_sample_timed_comment_owns_one_and_a_half_seconds(self):
        plan = tp.paragraph_plan(tp.sample_script_text())
        flat = [word for paragraph in plan for word in paragraph.words]
        index = next(i for i, word in enumerate(flat)
                     if word.is_comment and abs(word.seconds - 1.5) < 1e-9)
        comment = flat[index]
        self.assertAlmostEqual(comment.clock, 8.0)
        self.assertAlmostEqual(comment.clock + comment.seconds,
                               flat[index + 1].clock)


class StartupTests(unittest.TestCase):
    def test_portal_probe_is_disabled(self):
        # Keeps the harmless "Cannot get portal ... Inhibit" startup warning away.
        self.assertIn("no-portals", os.environ.get("GDK_DEBUG", "").split(","))


if __name__ == "__main__":
    unittest.main()
