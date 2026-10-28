"""Offline check that every fault control still applies to the current sources.

A control whose faulted text has moved would stop the controls run as stale;
this finds that in ordinary CI, without a compiler, fuzzer or CBMC."""
import importlib.util
from pathlib import Path
import shutil
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("fault_controls", ROOT / "tools/fault_controls.py")
CONTROLS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CONTROLS)


class ControlTests(unittest.TestCase):
    def test_every_fault_applies_once_and_changes_the_source(self):
        for name, (_, patches, expectations) in CONTROLS.CONTROLS.items():
            with self.subTest(control=name), tempfile.TemporaryDirectory() as work:
                copy = Path(work)
                for file in {f for f, _, _ in patches}:
                    (copy / file).parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(ROOT / file, copy / file)
                CONTROLS.apply(copy, patches)
                for file in {f for f, _, _ in patches}:
                    self.assertNotEqual((copy / file).read_text(), (ROOT / file).read_text())
                self.assertTrue(any(expected is not None for _, expected in expectations))

    def test_a_moved_fault_is_reported_stale(self):
        with tempfile.TemporaryDirectory() as work:
            copy = Path(work)
            (copy / "src").mkdir()
            (copy / "src/cu_arena.c").write_text("/* the faulted text is gone */\n")
            with self.assertRaises(SystemExit):
                CONTROLS.apply(copy, [("src/cu_arena.c", CONTROLS.REALLOC, "")])


if __name__ == "__main__":
    unittest.main()
