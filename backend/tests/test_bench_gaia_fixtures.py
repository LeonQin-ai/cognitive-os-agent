"""Regression test for resolving GAIA fixtures when launched outside backend/."""

import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


BACKEND = Path(__file__).resolve().parents[1]
EXE = BACKEND / "build" / ("cognitive-os-agent-bench-gaia.exe" if os.name == "nt"
                            else "cognitive-os-agent-bench-gaia")


@unittest.skipUnless(importlib.util.find_spec("docx") and importlib.util.find_spec("PIL"),
                     "requires python-docx and Pillow")
class GaiaFixtureLocationTest(unittest.TestCase):
    def test_fixtures_resolve_relative_to_executable_from_external_cwd(self):
        self.assertTrue(EXE.is_file(), f"build benchmark first: {EXE}")
        with tempfile.TemporaryDirectory(prefix="gaia-fixture-cwd-") as cwd:
            result = subprocess.run(
                [str(EXE), "--fixtures-only"],
                cwd=cwd,
                text=True,
                encoding="utf-8",
                errors="replace",
                capture_output=True,
                timeout=60,
                check=False,
            )
            self.assertEqual(result.returncode, 0,
                             f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}")
            fixture_dir = Path(cwd) / "state-bench-gaia" / "ws" / "gaia"
            for name in ("contact.docx", "report.docx", "diagram.png"):
                self.assertTrue((fixture_dir / name).is_file(), name)


if __name__ == "__main__":
    unittest.main()
