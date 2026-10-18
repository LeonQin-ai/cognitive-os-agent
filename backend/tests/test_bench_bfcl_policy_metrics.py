"""Keep planner refusal and execution-side policy enforcement metrics separate."""

import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest


BACKEND = Path(__file__).resolve().parents[1]
EXE = BACKEND / "build" / ("cognitive-os-agent-bench-bfcl.exe" if os.name == "nt"
                            else "cognitive-os-agent-bench-bfcl")


class BfclPolicyMetricsTest(unittest.TestCase):
    def test_hard_block_metric_executes_a_real_denied_tool_call(self):
        self.assertTrue(EXE.is_file(), f"build benchmark first: {EXE}")
        with tempfile.TemporaryDirectory(prefix="bfcl-policy-") as cwd:
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
            match = re.search(r"^  JSON: (.+)$", result.stdout, re.MULTILINE)
            self.assertIsNotNone(match, result.stdout)
            metrics = json.loads(match.group(1))
            self.assertEqual(metrics["hard_block_probe_ok"], 1)
            self.assertEqual(metrics["hard_block_probe_total"], 1)
            self.assertIn("hard-block   : PASS", result.stdout)
            self.assertFalse((Path(cwd) / "state-bfcl" / "ws" /
                              "enf-hard-block-probe.txt").exists())


if __name__ == "__main__":
    unittest.main()
