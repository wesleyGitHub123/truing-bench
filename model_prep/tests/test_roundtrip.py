"""SPEC 14.3.1 synthetic round trip, DEFINED PER LAYOUT, as four separate tests."""
import numpy as np
import pytest

from truing_model_prep import artifact as art_mod
from truing_model_prep.artifact import LAYOUT_FULL, LAYOUT_TENSION_ABSENT
from truing_model_prep.params import Side
from truing_model_prep.solver import SolverRefusal, predict, solve


def disturbance(art, seed, scale_rev=0.05):
    rng = np.random.default_rng(seed)
    return rng.uniform(-scale_rev, scale_rev, art.n_spokes)


def split(art, y):
    nra = art.n_rim_angles
    return y[:nra], y[nra: 2 * nra], y[2 * nra:]


def row_space_projector(layout):
    """P = Phi-dagger Phi projects onto the row space (the identifiable subspace)."""
    return layout.phi_pseudoinverse @ np.linalg.pinv(layout.phi_pseudoinverse)


def test_1a_ls_inversion_recovers_the_disturbance(sym32):
    """FULL: the tension residual is T - s*T_norm with s the CURRENT scale (SPEC 8.3.1), so the
    mean of the tension disturbance is removed by construction before the inversion. d_ls then
    equals d_applied plus exactly the pseudoinverse image of that removed mean term; on this
    artifact (trust_tension = 1e-5) the effect is ~1e-5 relative. Both the exact identity and
    the practical recovery are asserted. TENSION_ABSENT: the row-space projection."""
    wheel, solver, _, _, art = sym32
    from truing_model_prep.artifact import row_scaling
    from truing_model_prep.solver import ls_invert
    n, nra = art.n_spokes, art.n_rim_angles
    scale = row_scaling(solver, nra, n)
    T0 = solver.target_tension_n * np.ones(n)
    for seed in range(5):
        d_applied = disturbance(art, seed)
        y = predict(art, np.concatenate([np.zeros(2 * nra), T0]), d_applied)   # Eq. 2, the DISTURBANCE
        u, v, T = split(art, y)
        full = art.layout(LAYOUT_FULL)
        d_ls, _, s = ls_invert(art, solver, LAYOUT_FULL, u, v, T)
        assert s == pytest.approx(float(np.mean(T)))
        removed_mean = np.concatenate([np.zeros(2 * nra), np.full(n, -float(np.mean(art.phi_t @ d_applied)) * scale[2 * nra])])
        P = row_space_projector(full)
        assert np.allclose(d_ls, P @ d_applied + full.phi_pseudoinverse @ removed_mean, atol=1e-10)
        if full.effective_rank == n:
            assert np.linalg.norm(d_ls - d_applied) / np.linalg.norm(d_applied) < 1e-3, "FULL recovers d_applied"
        # TENSION_ABSENT: recover the ROW-SPACE projection, never the unobservable component.
        ta = art.layout(LAYOUT_TENSION_ABSENT)
        d_ta, _, _ = ls_invert(art, solver, LAYOUT_TENSION_ABSENT, u, v, None)
        P_ta = row_space_projector(ta)
        assert np.allclose(d_ta, P_ta @ d_applied, atol=1e-8)
        if ta.expected_null_dim == 0:
            assert np.allclose(d_ta, d_applied, atol=1e-8)   # full rank here: the projection is the identity


def test_1a_with_representative_noise(sym32):
    wheel, solver, _, _, art = sym32
    rng = np.random.default_rng(7)
    T0 = solver.target_tension_n * np.ones(art.n_spokes)
    d_applied = disturbance(art, 11, scale_rev=0.2)
    y = predict(art, np.concatenate([np.zeros(2 * art.n_rim_angles), T0]), d_applied)
    u, v, T = split(art, y)
    u = u + rng.normal(0, 0.01, u.size)      # dial-reading noise, 0.01 mm
    v = v + rng.normal(0, 0.01, v.size)
    T = T + rng.normal(0, 20.0, T.size)      # provisional tension estimates, 20 N
    from truing_model_prep.artifact import row_scaling
    r = solve(art, solver, LAYOUT_TENSION_ABSENT, u, v, None, wheel.asymmetric, wheel.indexing_origin_side)
    ta = art.layout(LAYOUT_TENSION_ABSENT)
    P = row_space_projector(ta)
    scale = row_scaling(solver, art.n_rim_angles, art.n_spokes)[: 2 * art.n_rim_angles]
    y_clean = np.concatenate([split(art, predict(art, np.concatenate([np.zeros(2 * art.n_rim_angles), T0]), d_applied))[k]
                              for k in (0, 1)])
    noise_tilde = (np.concatenate([u, v]) - y_clean) * scale
    bound = np.linalg.norm(ta.phi_pseudoinverse, 2) * np.linalg.norm(noise_tilde)   # ||pinv||_2 * ||noise||
    err = np.linalg.norm(r.d_ls - P @ d_applied)
    assert err <= bound + 1e-12, "the noise-induced error can never exceed the pseudoinverse norm bound"
    # Recorded finding (docs/IMPLEMENTATION_NOTES.md): with 0.01 mm gauge noise a SINGLE solve on
    # this analytical wheel (effective condition ~48) carries tens of percent of error; the outer
    # cycle loop and the non-decrease abort are what make truing robust to it (SPEC 8.1).
    rel = err / np.linalg.norm(P @ d_applied)
    assert rel < 0.6


def test_1b_shape_solve_removes_the_common_mode_component(sym32):
    wheel, solver, _, _, art = sym32
    cm = art.common_mode_direction
    T0 = solver.target_tension_n * np.ones(art.n_spokes)
    d_applied = disturbance(art, 3) + 0.03   # add a deliberate common-mode (equal) component
    y = predict(art, np.concatenate([np.zeros(2 * art.n_rim_angles), T0]), d_applied)
    u, v, T = split(art, y)
    if cm.identified:
        r = solve(art, solver, LAYOUT_FULL, u, v, T, wheel.asymmetric, wheel.indexing_origin_side, apply_mean_tension=False)
        n_mt = cm.vector
        expected = -(r.d_ls - (n_mt @ r.d_ls) * n_mt)
        assert np.allclose(r.d_shape, expected)
        assert abs(n_mt @ r.d_shape) < 1e-9, "no component along n_mt remains"
    else:
        with pytest.raises(SolverRefusal) as e:
            solve(art, solver, LAYOUT_FULL, u, v, T, wheel.asymmetric, wheel.indexing_origin_side)
        assert e.value.reason == "MEAN_TENSION_MODEL_UNAVAILABLE"
    # TENSION_ABSENT: minimum-norm solution, NO projection applied, and it lies in the row space.
    r_ta = solve(art, solver, LAYOUT_TENSION_ABSENT, u, v, None, wheel.asymmetric, wheel.indexing_origin_side)
    P = row_space_projector(art.layout(LAYOUT_TENSION_ABSENT))
    assert np.allclose(r_ta.d_shape, -(P @ d_applied), atol=1e-6)
    assert np.allclose(P @ r_ta.d_shape, r_ta.d_shape, atol=1e-9)


def test_1c_mean_tension_shift_symmetric_only(sym32, asym36):
    wheel, solver, _, _, art = sym32
    from truing_model_prep.solver import mean_tension_shift
    T = np.full(art.n_spokes, 900.0)
    d_cm, applied, policy = mean_tension_shift(art, solver, T, wheel.asymmetric, wheel.indexing_origin_side)
    if art.common_mode_direction.identified:
        assert applied and policy is None
        assert np.allclose(d_cm, (1000.0 - 900.0) / 473.0)          # per-spoke VECTOR, equal entries
        assert d_cm.shape == (art.n_spokes,)
    else:
        assert not applied and policy == "MEAN_TENSION_MODEL_UNAVAILABLE"
    wheel_a, solver_a, _, _, art_a = asym36
    d_cm_a, applied_a, policy_a = mean_tension_shift(art_a, solver_a, np.full(36, 900.0), wheel_a.asymmetric, wheel_a.indexing_origin_side)
    assert not applied_a and policy_a == "MEAN_TENSION_MODEL_UNAVAILABLE"   # OPEN: refused, never approximated
    assert np.all(d_cm_a == 0.0)


def test_1d_combined_end_to_end_only_for_constructed_cases(sym32):
    """d_adj ~ -d_applied ONLY when Part 2 restores exactly what Part 1 removed: the disturbance
    has no n_mt component and the mean tension sits at target."""
    wheel, solver, _, _, art = sym32
    cm = art.common_mode_direction
    if not cm.identified:
        pytest.skip("n_mt not identified for this artifact; Part 2 refused by design")
    n_mt = cm.vector
    d_applied = disturbance(art, 21)
    d_applied = d_applied - (n_mt @ d_applied) * n_mt                 # constructed: orthogonal to n_mt
    T0 = solver.target_tension_n * np.ones(art.n_spokes)
    y = predict(art, np.concatenate([np.zeros(2 * art.n_rim_angles), T0]), d_applied)
    u, v, T = split(art, y)
    # Mean tension after the disturbance is not exactly target; pin it so Part 2 contributes zero.
    T = T - (np.mean(T) - solver.target_tension_n)
    r = solve(art, solver, LAYOUT_FULL, u, v, T, wheel.asymmetric, wheel.indexing_origin_side)
    assert np.allclose(r.d_adj, -d_applied, atol=1e-5)
    assert r.mean_tension_targeting_applied


def test_cost_is_row_count_normalised_and_layout_specific(sym32):
    wheel, solver, _, _, art = sym32
    u = np.full(art.n_rim_angles, solver.tol_lateral_mm)   # every lateral residual exactly at tolerance
    v = np.zeros(art.n_rim_angles)
    r_ta = solve(art, solver, LAYOUT_TENSION_ABSENT, u, v, None, wheel.asymmetric, wheel.indexing_origin_side)
    # 32 rows of 1.0^2 over 64 active rows -> 0.5 (J ~ 1 does NOT mean "at tolerance", SPEC 8.6).
    assert r_ta.cost_j == pytest.approx(0.5)
    assert r_ta.n_active_rows == 64
    T = np.full(art.n_spokes, 1000.0)
    if art.common_mode_direction.identified:
        r_full = solve(art, solver, LAYOUT_FULL, u, v, T, wheel.asymmetric, wheel.indexing_origin_side)
        assert r_full.cost_j == pytest.approx(32.0 / 96.0)
        assert r_full.n_active_rows == 96
        assert r_full.s_scale == pytest.approx(1000.0)


def test_sqrt_weighting_realises_the_intended_cost(sym32):
    """||sqrt(w) r||^2 = w ||r||^2: the radial energy enters J with weight w_v, not w_v^2 (SPEC 8.3.1)."""
    wheel, solver, _, _, art = sym32
    u = np.zeros(art.n_rim_angles)
    v = np.full(art.n_rim_angles, solver.tol_radial_mm)   # radial residual 1.0 (normalised) everywhere
    r = solve(art, solver, LAYOUT_TENSION_ABSENT, u, v, None, wheel.asymmetric, wheel.indexing_origin_side)
    assert r.cost_j == pytest.approx(solver.trust_radial * 32 / 64)
