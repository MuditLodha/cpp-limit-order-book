"""End-to-end checks, without third-party Python dependencies."""
import csv
import importlib.util
import math
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "bin" / ("market_making.exe" if os.name == "nt" else "market_making")
ENV = os.environ.copy()
if os.name == "nt" and Path("C:/msys64/ucrt64/bin").exists():
    ENV["PATH"] = "C:/msys64/ucrt64/bin" + os.pathsep + ENV.get("PATH", "")

class SimulationCliTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        (ROOT / "results").mkdir(exist_ok=True)

    def invoke(self, *args):
        return subprocess.run([str(BINARY), *map(str, args)], capture_output=True, text=True, env=ENV)

    def test_invalid_arguments_fail(self):
        for args in (("--strategy", "nope"), ("--dt", "0"), ("--quote-size", "1.5"),
                     ("--sigma", "nan"), ("--informed", "1.1"), ("--unknown", "1"),
                     ("--seconds",), ("--seed", "-1"), ("--sigma", "1junk")):
            with self.subTest(args=args):
                result = self.invoke(*args)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("ERROR", result.stderr)

    def test_reproducible_files_and_ledger(self):
        with tempfile.TemporaryDirectory(dir=ROOT / "results") as temp:
            # TemporaryDirectory recursively removes only this verified project child.
            self.assertTrue(Path(temp).resolve().is_relative_to((ROOT / "results").resolve()))
            first, second = Path(temp) / "first", Path(temp) / "second"
            for directory in (first, second):
                result = self.invoke("--seconds", 5, "--seed", 9, "--output", directory)
                self.assertEqual(result.returncode, 0, result.stderr)
            for filename in ("runs.csv", "fills.csv", "trajectory.csv"):
                self.assertEqual((first/filename).read_bytes(), (second/filename).read_bytes())
            with (first/"fills.csv").open() as f:
                fills = list(csv.DictReader(f))
            cash = sum(-int(f["direction"])*int(f["quantity"])*float(f["price"])-float(f["fee"]) for f in fills)
            inventory = sum(int(f["direction"])*int(f["quantity"]) for f in fills)
            with (first/"runs.csv").open() as f:
                summary = next(csv.DictReader(f))
            self.assertEqual(inventory, 0)
            self.assertTrue(math.isclose(cash, float(summary["net_pnl"]), abs_tol=1e-6))

    def test_paired_study_and_audit_detect_tampering(self):
        spec = importlib.util.spec_from_file_location("study", ROOT/"simulation/run_study.py")
        study = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(study)
        with tempfile.TemporaryDirectory(dir=ROOT / "results") as temp:
            # TemporaryDirectory recursively removes only this verified project child.
            self.assertTrue(Path(temp).resolve().is_relative_to((ROOT / "results").resolve()))
            output = Path(temp)
            result = self.invoke("--study", "--seeds", 2, "--seconds", 1, "--output", output)
            self.assertEqual(result.returncode, 0, result.stderr)
            rows = study.read_csv(output/"runs.csv")
            self.assertEqual(len(rows), 96)
            self.assertEqual(study.audit(output, rows), 12)
            # A same-seed comparison must receive the same total incoming volume.
            for regime in study.REGIMES:
                for seed in (1, 2):
                    volumes = {r["incoming_volume"] for r in rows
                               if r["group"]=="comparison" and r["regime"]==regime and r["seed"]==seed}
                    self.assertEqual(len(volumes), 1)
            rows[0]["net_pnl"] += 100
            with self.assertRaises(ValueError):
                study.audit(output, rows)

if __name__ == "__main__":
    unittest.main()
