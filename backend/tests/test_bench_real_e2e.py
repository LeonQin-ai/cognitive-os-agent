"""Regression test that a natural-language success is not an MCP E2E pass."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


BACKEND = Path(__file__).resolve().parents[1]
EXE = BACKEND / "build" / ("cognitive-os-agent-bench-real.exe" if os.name == "nt"
                            else "cognitive-os-agent-bench-real")


@unittest.skipUnless(shutil.which("node"), "requires Node.js for the stdio MCP fixture")
class RealBenchmarkE2ETest(unittest.TestCase):
    def test_mcp_natural_language_fallback_is_not_counted_as_success(self):
        self.assertTrue(EXE.is_file(), f"build benchmark first: {EXE}")
        with tempfile.TemporaryDirectory(prefix="real-bench-e2e-") as cwd:
            result = subprocess.run(
                [str(EXE), "--mock"],
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
            self.assertIn("MCP fixture: ready", result.stdout)
            self.assertRegex(result.stdout,
                             r"\[toolbench/mcp\s+\] sel=X e2e=X side=N/A")


if __name__ == "__main__":
    unittest.main()
