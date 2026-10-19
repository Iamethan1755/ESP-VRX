"""Independent physical/model checks, C5VRX by Twotoz and contributors."""
import sys
from pathlib import Path
import unittest
import numpy as np
from scipy.special import ndtr
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'research'))
import iq_lane_fusion as F


class FusionTest(unittest.TestCase):
    def test_physical_bit_extraction_exhaustive(self):
        values = np.arange(-512, 512)
        for plan in F.all_plans(signs=False, symmetric=True):
            expected = np.array([sum(((int(x) & (1 << bit)) != 0) << j
                                    for j, bit in enumerate(plan.i)) for x in values], np.uint8)
            np.testing.assert_array_equal(F.component_code(values, plan.i), expected)
            spans = F.intervals(plan.i)
            for x, token in zip(values, expected):
                self.assertTrue(any(lo <= x + .5 < hi for lo, hi in spans[int(token)]))
        self.assertEqual(F.adc10(-513.), -512)
        self.assertEqual(F.adc10(512.), 511)
        self.assertEqual(F.adc10(-.01), -1)

    def test_same_radius_folding_ambiguity(self):
        z = np.array([144 + 16j, 16 + 144j])
        np.testing.assert_array_equal(F.capture(z), [17, 17])
        self.assertNotEqual(*F.capture(z, F.FINE))
        self.assertAlmostEqual(abs(z[0]), abs(z[1]))

    def test_likelihood_normalization_and_clipped_tails(self):
        for plan in (F.ULTRA, F.COARSE, F.SHADOW_PLANS[-1], F.Plan((0, 2, 5, 8), (1, 3, 6, 7))):
            L = F.likelihood(plan, F.Context((32., 180., 800.), sigma=13., dc_i=12., dc_q=-25., iq_ratio=1.3, skew_rad=.2), 64)
            np.testing.assert_allclose(L.sum(axis=0), 1., atol=2e-14)
        bits = (6, 7, 8, 9)
        axis = F.axis_likelihood(bits, np.array([0.]), 20.)
        self.assertAlmostEqual(axis[0, 0], ndtr(64 / 20) - .5)

    def test_frequency_joint_matches_direct_enumeration(self):
        rng = np.random.default_rng(12)
        previous = rng.random(32); previous /= previous.sum()
        L = rng.random((7, 32))
        kernel = F.delta_kernel(F.Context(), 32)[1]
        exact = np.array([[sum(previous[s] * row[(s + d) % 32] for s in range(32)) * kernel[d]
                           for d in range(32)] for row in L])
        np.testing.assert_allclose(F.delta_joint(previous, L, kernel), exact, atol=1e-15)

    def test_frequency_selection_requires_two_observations(self):
        # One phase observation with uniform previous phase says nothing
        # about frequency. Two perfect observations identify its increment.
        ctx = F.Context()
        L = np.eye(32)
        self.assertGreater(F.frequency_risk(np.full(32, 1 / 32), L, ctx), 1e12)
        self.assertLess(F.expected_pair_risk(L, L, ctx), 1.)

    def test_tracker_recovers_across_fold_and_context_gap(self):
        n = 768
        phase = 2 * np.pi * 1.25e6 / 40e6 * np.arange(n)
        z = 180 * np.exp(1j * phase)
        result = F.decode(F.capture(z, F.COARSE), F.COARSE, F.Context((180.,), sigma=2.), 128, reset_at=(384,))
        # Coarse cells have substantial instantaneous jitter. This checks
        # unbiased CW recovery, not a claim of 200 kHz per-sample precision.
        self.assertLess(abs(np.mean(result.hz[30:350]) - 1.25e6), 200000)
        self.assertLess(abs(np.mean(result.hz[420:]) - 1.25e6), 200000)
        self.assertEqual(result.ambiguity[384], 1)
        self.assertAlmostEqual(result.posterior.sum(), 1)
        self.assertTrue(np.isfinite(result.hz).all())

    def test_search_supports_asymmetry_signless_and_gain(self):
        self.assertEqual(sum(1 for _ in F.all_plans()), 7056)
        self.assertEqual(sum(1 for _ in F.all_plans(signs=False)), 44100)
        result = F.select(F.SHADOW_PLANS, (F.Context((100.,), sigma=5.),), gains=(.5, 1.), n=32)
        self.assertEqual(result['candidates'], 12)
        self.assertEqual(len(result['ranked']), 12)
        self.assertGreaterEqual(result['best']['risk_hz2'], 0)

    def test_transition_is_not_atomic(self):
        z = np.full(64, 145 + 67j)
        raw, epoch = F.route_transition(z, F.ULTRA, F.COARSE, 10, 2)
        np.testing.assert_array_equal(raw[:10], F.capture(z[:10], F.ULTRA))
        np.testing.assert_array_equal(raw[22:], F.capture(z[22:], F.COARSE))
        self.assertTrue(np.any((epoch > 0) & (epoch < epoch[-1])))
        # A swap of two lines can temporarily duplicate a physical source bit.
        swapped = F.Plan((5, 4, 6, 9), F.ULTRA.q)
        F.route_transition(z, F.ULTRA, swapped, 3)

    def test_gate_requires_all_contexts_and_invalidates_stale_evidence(self):
        g = F.DecisionGate()
        self.assertFalse(g.propose('fine', [10, 10], [8, 11], 0, 1))
        self.assertFalse(g.propose('fine', [10, 10], [8, 8], 1000, 1))
        self.assertFalse(g.propose('fine', [10, 10], [8, 8], 3000, 1))
        self.assertTrue(g.propose('fine', [10, 10], [8, 8], 7000, 1))
        self.assertFalse(g.propose('fine', [10, 10], [8, 8], 8000, 2))
        self.assertFalse(g.propose('fine', [10, 10], [8, 8], 9000, 2, valid=False))
        self.assertFalse(g.propose('fine', [10, 10], [np.nan, 8], 10000, 2))

    def test_simulator_gain_is_shared_and_folding_is_real(self):
        import sim
        c = sim.make_case(24, 6., fields=1, physical_adc=True, adc_gain=10.)
        np.testing.assert_array_equal(c['raw'], F.capture(c['adc']))
        np.testing.assert_array_equal(c['clean'], F.capture(c['clean_adc']))
        self.assertEqual(c['adc_gain'], 10.)
        self.assertGreater(np.max(abs(c['adc'].real)), 128.)

    def test_compressed_kernel_keeps_hardware_transition_plane(self):
        # Decoder replacement cannot silently overwrite state/DAC fields in
        # the shared LUT. It is still not equivalent to the full posterior.
        import sim
        from lane_fusion_study import compressed_lut
        L = compressed_lut(F.SHADOW_PLANS[-1], F.Context(), 64)
        np.testing.assert_array_equal(L & 2047, sim.u85q_lut() & 2047)
        self.assertEqual(L.nbytes, 2048)
        with self.assertRaises(ValueError):
            compressed_lut(F.Plan((0, 1, 2, 3), (0, 1, 2, 3)), F.Context(), 64)


if __name__ == '__main__':
    unittest.main()
