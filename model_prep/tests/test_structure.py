"""SPEC 14.3.2 structural property tests, SPEC 8.8 class structure, SPEC 6.5 localization,
SPEC 8.9 sampling floor, SPEC 8.5 / 8.11 rank per layout."""
import numpy as np
import pytest

from truing_model_prep import artifact as art_mod
from truing_model_prep import fixtures
from truing_model_prep.fourier import SamplingFloorError, fit
from truing_model_prep.params import GeneratorParams


def circular_distance(a, b):
    return float(np.abs(np.angle(np.exp(1j * (a - b)))))


def test_phi_t_four_class_cyclic_structure(sym32):
    """POSITIVE: circshift(Phi_t[:, i], 4) ~ Phi_t[:, i+4]; NEGATIVE: circshift by 1 fails (SPEC 8.8)."""
    _, _, _, _, art = sym32
    n = art.n_spokes
    tol = 1e-6 * np.max(np.abs(art.phi_t))
    for i in range(n):
        assert np.allclose(np.roll(art.phi_t[:, i], 4), art.phi_t[:, (i + 4) % n], atol=tol)
    violations = sum(not np.allclose(np.roll(art.phi_t[:, i], 1), art.phi_t[:, (i + 1) % n], atol=tol) for i in range(n))
    assert violations == n, "classes must be genuinely distinct: a shift of one spoke must NOT map columns"


def test_symmetric_mirror_relationship(sym32):
    """For a symmetric wheel the laced pattern is invariant under reflecting the rim plane AND the
    tangential direction followed by a rotation of three pitches: spoke p -> (3 - p) mod n with side
    and lead/trail both swapped. Tension responses map accordingly; lateral responses change sign."""
    _, _, _, raw, art = sym32
    n = art.n_spokes
    tol = 1e-6 * np.max(np.abs(art.phi_t))
    for i in range(n):
        for j in range(n):
            assert art.phi_t[j, i] == pytest.approx(art.phi_t[(3 - j) % n, (3 - i) % n], abs=tol)
    # Lateral: u_i(theta) = -u_{i'}(theta') with theta' = 2pi*3/n - theta.
    grid = raw.theta_fit_lat
    step = grid.size // n
    for i in range(n):
        ip = (3 - i) % n
        mirrored = -np.roll(raw.lateral_mm[::-1, ip], 3 * step + 1)   # theta -> 3*pitch - theta on the grid
        assert np.allclose(raw.lateral_mm[:, i], mirrored, atol=1e-6 * np.max(np.abs(raw.lateral_mm)))


def test_asymmetric_wheel_breaks_mirror_but_keeps_class_structure(asym36):
    _, _, _, _, art = asym36
    n = art.n_spokes
    tol = 1e-6 * np.max(np.abs(art.phi_t))
    for i in range(n):
        assert np.allclose(np.roll(art.phi_t[:, i], 4), art.phi_t[:, (i + 4) % n], atol=tol)
    mirror_ok = all(abs(art.phi_t[j, 0] - art.phi_t[(3 - j) % n, 3]) <= tol for j in range(n))
    assert not mirror_ok, "the dished wheel must not satisfy the symmetric mirror relation"
    assert art.common_mode_direction.identified is False


def test_column_localization(sym32):
    """SPEC 6.5: the extremum of the DETRENDED lateral column sits within tol_angular of spoke i."""
    _, solver, _, _, art = sym32
    n = art.n_spokes
    for i in range(n):
        col = art.phi_u[:, i] - np.mean(art.phi_u[:, i])
        k = int(np.argmax(np.abs(col)))
        assert circular_distance(art.theta_rim[k], 2 * np.pi * i / n) <= solver.tol_angular_rad + 1e-12


def test_sampling_floor_fires():
    theta = np.linspace(0, 2 * np.pi, 20, endpoint=False)
    with pytest.raises(SamplingFloorError):
        fit(theta, np.cos(theta), order=10)   # floor is 21
    fit(np.linspace(0, 2 * np.pi, 21, endpoint=False), np.cos(np.linspace(0, 2 * np.pi, 21, endpoint=False)), order=10)


def test_fit_residuals_are_small_at_fixture_orders(sym32):
    _, solver, _, _, art = sym32
    assert art.fit_rms_lat.max() < 5e-3 * np.max(np.abs(art.phi_u))
    assert art.fit_rms_rad.max() < 5e-2 * np.max(np.abs(art.phi_v))


def test_rank_and_conditioning_recorded_per_layout(sym32):
    """SPEC 8.5: TENSION_ABSENT rank is bounded by the Fourier basis and measured; FULL is measured,
    never assumed deficient. The recorded values are self-consistent and within limits."""
    _, solver, _, _, art = sym32
    n = art.n_spokes
    full = art.layout(art_mod.LAYOUT_FULL)
    ta = art.layout(art_mod.LAYOUT_TENSION_ABSENT)
    assert full.phi_pseudoinverse.shape == (n, 2 * art.n_rim_angles + n)
    assert ta.phi_pseudoinverse.shape == (n, 2 * art.n_rim_angles)
    for l in (full, ta):
        rank, cond = art_mod.effective_rank_and_cond(l.singular_values, solver.rank_tolerance)
        assert rank == l.effective_rank and cond == pytest.approx(l.effective_condition_number)
        assert l.expected_null_dim == n - l.effective_rank
        assert l.effective_condition_number <= solver.max_condition_number
    assert ta.effective_rank <= min(2 * art.N_lat + 1 + 2 * art.N_rad + 1, n)   # Fourier truncation bound (SPEC 8.5)
    assert full.effective_rank >= ta.effective_rank


def test_common_mode_conditions_evaluated_and_recorded(sym32):
    _, solver, _, _, art = sym32
    cm = art.common_mode_direction
    assert cm.vector is not None and np.allclose(cm.vector, np.ones(32) / np.sqrt(32))
    assert cm.T_target_assumed is not None and np.mean(cm.T_target_assumed) == pytest.approx(1.0)
    assert cm.cond_i_residual is not None and cm.cond_ii_residual is not None
    assert cm.cond_i_tolerance == solver.n_mt_displacement_tolerance
    # Condition (i) decomposition is recorded: on this analytical wheel, equal tightening produces
    # NO lateral response and a UNIFORM radial contraction (rim compression), which is the entire
    # residual. Whether that uniform mode counts as "displacement change" is a specification
    # question (SPEC 8.4 Part 1 / 14.3.2) recorded in docs/IMPLEMENTATION_NOTES.md, not decided here.
    assert cm.cond_i_uniform_radial_part is not None and cm.cond_i_remainder is not None
    assert cm.cond_i_uniform_radial_part > 0.99 * cm.cond_i_residual
    assert cm.cond_i_remainder < 0.05 * cm.cond_i_residual
    lateral_resp = art.phi_u @ cm.vector
    assert np.max(np.abs(lateral_resp)) < 1e-9, "equal tightening of a symmetric wheel has no lateral response"
    # Condition (ii): the tension response to equal tightening is uniform to within 1 %, with the
    # two symmetric class pairs (B-lead/A-trail, A-lead/B-trail) each internally equal.
    t = art.phi_t @ cm.vector
    assert t[0::4] == pytest.approx(t[3::4], rel=1e-9) and t[1::4] == pytest.approx(t[2::4], rel=1e-9)
    assert cm.cond_ii_residual < 1e-2
    # identified is exactly the recorded rule, never a bare assertion (SPEC 11.4 "reproducible").
    expected = (cm.cond_i_residual <= cm.cond_i_tolerance and cm.cond_ii_residual <= cm.cond_ii_tolerance
                and cm.displacement_null_dim == 1)
    assert cm.identified == expected


def test_reducing_fourier_order_changes_rank_bound(sym32):
    """SPEC 8.5 / 8.9 coupling: a lower N_rad lowers the displacement basis and therefore the
    TENSION_ABSENT rank bound; model prep must re-verify after any change."""
    wheel, solver, gen, raw, art = sym32
    from dataclasses import replace
    from truing_model_prep.generator import generate
    low = replace(solver, N_lat=2, N_rad=3)
    raw_low = generate(wheel, low, gen)
    art_low = art_mod.assemble(raw_low, wheel, low, gen, artifact_id="low_order")
    ta_low = art_low.layout(art_mod.LAYOUT_TENSION_ABSENT)
    assert ta_low.effective_rank <= 2 * 2 + 1 + 2 * 3 + 1 == 12
    assert ta_low.expected_null_dim >= 32 - 12
    assert art_low.generating_fingerprint != art.generating_fingerprint
