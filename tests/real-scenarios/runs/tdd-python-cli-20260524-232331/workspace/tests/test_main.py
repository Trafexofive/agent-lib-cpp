import subprocess
import sys
import unittest

from main import slugify, titlecase


class TextUtilTests(unittest.TestCase):
    def test_slugify_normalizes_spaces_punctuation_and_underscores(self):
        self.assertEqual(slugify(" Hello, Weird_WORLD!! "), "hello-weird-world")

    def test_titlecase_keeps_cli_existing_behavior(self):
        self.assertEqual(titlecase("hello world"), "Hello World")

    def test_cli_slug_mode_outputs_slug(self):
        out = subprocess.check_output(
            [sys.executable, "main.py", "slug", "Two Words_and Stuff"],
            text=True,
        ).strip()
        self.assertEqual(out, "two-words-and-stuff")


if __name__ == "__main__":
    unittest.main()
