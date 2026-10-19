"""C5VRX-5 by Twotoz and contributors: diagnostic log semantics regression."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("compare", Path(__file__).parents[1] / "tools/field_diag_compare.py")
compare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(compare)

class CompareTest(unittest.TestCase):
    def summary(self, text):
        with tempfile.NamedTemporaryFile(mode="w") as f:
            f.write(text)
            f.flush()
            return compare.summary(f.name)

    def test_partial_coverage_and_trace_not_seconds(self):
        r = self.summary("FIELD_DIAG t=1 sync_valid_pct=50 measurement_coverage_pct=50 sync_given_measured_pct=100 gain=70 max=83\n"
                         "FIELD_DIAG t=2 sync_valid_pct=0 measurement_coverage_pct=0 gain=70 max=83\n"
                         "FIELD_TRACE t=1 t_ms=1050 sync_valid_pct=0 measurement_coverage_pct=100\n")
        self.assertEqual(r["seconds"], 2)
        self.assertEqual(r["sync_evidence_ok_pct"], 50)
        self.assertEqual(r["coverage_pct_median"], 25)

    def test_legacy_unknown_coverage(self):
        r = self.summary("FIELD_DIAG t=1 video_pct=100 gain=80 max=83\n")
        self.assertEqual(r["legacy_unknown_coverage_s"], 1)
        self.assertNotEqual(r["sync_evidence_ok_pct"], 100)

    def test_ladder_at_the_edge(self):
        r = self.summary("FIELD_DIAG t=1 cn_est_x10=30 kp_x100=70 lp=1\n"
                         "FIELD_DIAG t=2 cn_est_x10=40 kp_x100=80 lp=0\n"
                         "FIELD_DIAG t=3 cn_est_x10=-990 kp_x100=85 lp=0\n"
                         "FIELD_DIAG t=4 cn_est_x10=150 kp_x100=75 lp=0\n")
        self.assertEqual(r["edge_s"], 2)
        self.assertEqual(r["edge_lp_pct"], 50)
        self.assertAlmostEqual(r["edge_kp_median"], 0.75)
        self.assertAlmostEqual(r["strong_kp_median"], 0.75)

    def test_old_log_without_ladder(self):
        r = self.summary("FIELD_DIAG t=1 cn_est_x10=30 gain=80 max=83\n")
        self.assertEqual(r["edge_s"], 0)

    def test_empty(self):
        self.assertEqual(self.summary("")["seconds"], 0)

if __name__ == "__main__":
    unittest.main()
