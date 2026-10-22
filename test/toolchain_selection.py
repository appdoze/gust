# Modified for Gust (https://github.com/appdoze/gust); see MODIFICATIONS.md.
"""Compiler discovery prefers speed, while --exe keeps assertion builds explicit."""

import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import toolchain as tc


class CompilerSelection(unittest.TestCase):
    def test_preference_and_fallback(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(tc, "REPO_ROOT", Path(tmp)):
            paths = [Path(tmp) / "build" / sub / ("gust" + tc.EXE_SUFFIX)
                     for sub in ("Release", "RelWithDebInfo", "", "Debug")]
            for path in paths:
                path.parent.mkdir(parents=True, exist_ok=True)
                path.touch()
            self.assertEqual(tc.find_goose(paths[-1]), paths[-1].resolve())
            for path in paths:
                self.assertEqual(tc.find_goose(), path)
                path.unlink()
            with self.assertRaises(SystemExit):
                tc.find_goose()
            with self.assertRaises(SystemExit):
                tc.find_goose(paths[0])


if __name__ == "__main__":
    unittest.main()
