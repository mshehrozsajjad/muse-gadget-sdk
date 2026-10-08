#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""A reply as the card shows it and the speech says it (muse_hatch_plain_text in
muse_chat_text.c): Markdown marks go, and so do code and Muse's widget markers,
which aren't for reading aloud. A short inline word stays."""

from __future__ import annotations

import os
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class PlainTextTest(unittest.TestCase):
    binary: Path

    @classmethod
    def setUpClass(cls) -> None:
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or shutil.which(cc[0]) is None:
            raise unittest.SkipTest("C compiler not available")
        cls.tmp = tempfile.TemporaryDirectory()
        cls.binary = Path(cls.tmp.name) / "muse_serial_chat_harness"
        proc = subprocess.run(
            [
                *cc,
                "-include",
                str(ROOT / "tests" / "host_compat.h"),
                "-std=gnu11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(ROOT / "components" / "muse"),
                str(ROOT / "tests" / "muse_serial_chat_harness.c"),
                str(ROOT / "components" / "muse" / "muse_chat_text.c"),
                str(ROOT / "components" / "muse" / "muse_text.c"),
                "-o",
                str(cls.binary),
            ],
            cwd=ROOT,
            text=True,
            capture_output=True,
        )
        if proc.returncode:
            raise AssertionError(proc.stdout + proc.stderr)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def plain(self, text: str) -> str:
        proc = subprocess.run(
            [str(self.binary), "plain"], input=text.encode(), capture_output=True, check=True
        )
        return proc.stdout.decode()

    def test_markdown_marks_go(self) -> None:
        self.assertEqual(self.plain("# Title\n**Bold** and [the site](https://x.y)."),
                         "Title\nBold and the site.")

    def test_code_block_goes(self) -> None:
        reply = 'Sending it now.\n```json\n{"command": "display.show_note"}\n```\nDone.'
        self.assertEqual(self.plain(reply), "Sending it now.\nDone.")

    def test_unclosed_code_block_goes_to_the_end(self) -> None:
        # A reply still arriving, cut off inside its block.
        self.assertEqual(self.plain("Here it is:\n```python\nprint(1)\n"), "Here it is:")

    def test_reply_of_only_code_is_empty(self) -> None:
        self.assertEqual(self.plain("```\nls -la\n```\n"), "")

    def test_code_like_inline_span_goes(self) -> None:
        self.assertEqual(self.plain("Call `display.show_note(text)` to show it."),
                         "Call to show it.")
        self.assertEqual(self.plain("It's in `main/app.c`."), "It's in.")

    def test_inline_word_stays(self) -> None:
        self.assertEqual(self.plain("Run `npm` first."), "Run npm first.")

    def test_triple_backticks_inside_a_line_go(self) -> None:
        self.assertEqual(self.plain("Use ```x = 1``` here."), "Use here.")

    def test_widget_marker_goes(self) -> None:
        # Muse's interactive widgets (multi-select options and the like), which
        # the app draws as buttons, ride in the text as [[hatch_widget:...]].
        self.assertEqual(self.plain("Pick one:\n[[hatch_widget:widget_7f3a]]\nOr say it."),
                         "Pick one:\nOr say it.")

    def test_widget_marker_with_nested_options_goes(self) -> None:
        reply = ('Which do you want? [[hatch_widget:{"type": "multi_select", '
                 '"options": [["a", "Tea"], ["b", "Coffee ]] x"]]}]] Tell me.')
        self.assertEqual(self.plain(reply), "Which do you want? Tell me.")

    def test_unclosed_widget_marker_goes_to_the_end(self) -> None:
        self.assertEqual(self.plain('Choose:\n[[hatch_widget:{"options": ["Tea", "Cof'),
                         "Choose:")

    def test_lone_backtick_goes(self) -> None:
        self.assertEqual(self.plain("it`s fine"), "its fine")


if __name__ == "__main__":
    unittest.main()
