"""SPEC 8.10 / 17.1.2: the analytical path's dependency must actually run."""
import numpy as np

from truing_model_prep import BIKE_WHEEL_CALC_COMMIT


def test_bike_wheel_calc_imports_and_solves():
    from bikewheelcalc import BicycleWheel, Hub, ModeMatrix, Rim

    w = BicycleWheel()
    w.hub = Hub(diameter=0.05, width=0.05)
    w.rim = Rim(radius=0.3, area=100e-6, I_lat=200.0 / 69e9, I_rad=100.0 / 69e9, J_tor=25.0 / 26e9, I_warp=0.0,
                young_mod=69e9, shear_mod=26e9)
    w.lace_cross(n_spokes=32, n_cross=3, diameter=2.0e-3, young_mod=210e9)
    w.apply_tension(T_avg=1000.0)
    mm = ModeMatrix(w, N=12)
    K = mm.K_rim(tension=True) + mm.K_spk(smeared_spokes=False, tension=True)
    A = mm.A_adj()
    assert A.shape == (4 + 8 * 12, 32)
    a = np.zeros(32)
    a[0] = 0.45e-3
    dm = np.linalg.solve(K, A @ a)
    dT = mm.spoke_tension_change(dm, a)
    assert dT.shape == (32,) and dT[0] > 0.0   # the per-spoke tension response exists (SPEC 8.10.1)
    assert np.isfinite(mm.rim_def_lat(0.0, dm)[0]) and np.isfinite(mm.rim_def_rad(0.0, dm)[0])


def test_pinned_commit_is_installed():
    import importlib.metadata as md
    dist = md.distribution("bikewheelcalc")
    direct = dist.read_text("direct_url.json") or ""
    assert BIKE_WHEEL_CALC_COMMIT in direct, "installed bike-wheel-calc is not the pinned commit"
