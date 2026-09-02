"""SPEC 6.4.2 physical ground-truth sign assertions and SPEC 6.5 indexing-origin handling.
These assert MECHANICS, not the generator's own output."""
import numpy as np
import pytest

from truing_model_prep import conventions as conv
from truing_model_prep import fixtures
from truing_model_prep.params import LeadTrail, Side
from truing_model_prep.generator import generate


def nearest_grid_index(theta_rim, theta):
    d = np.abs(np.angle(np.exp(1j * (theta_rim - theta))))
    return int(np.argmin(d))


def test_bike_wheel_calc_boundary_mapping():
    assert conv.indexing_offset(Side.B, LeadTrail.LEADING) == 0
    assert conv.indexing_offset(Side.A, LeadTrail.LEADING) == 1
    assert conv.indexing_offset(Side.B, LeadTrail.TRAILING) == 2
    assert conv.indexing_offset(Side.A, LeadTrail.TRAILING) == 3
    assert conv.radial_bwc_to_system_mm(np.array([1e-3]))[0] == pytest.approx(-1.0)   # inward -> outward flip
    assert conv.lateral_bwc_to_system_mm(np.array([1e-3]))[0] == pytest.approx(1.0)   # NDS == Side B: no flip
    assert conv.spoke_side(0, Side.B) == Side.B and conv.spoke_side(1, Side.B) == Side.A


def test_tightening_pulls_rim_toward_hub_at_that_spoke(sym32):
    """Positive tightening -> NEGATIVE radial displacement (outward-positive) at the spoke's angle."""
    wheel, solver, gen, raw, art = sym32
    for i in range(art.n_spokes):
        k = nearest_grid_index(art.theta_rim, 2 * np.pi * i / art.n_spokes)
        assert art.phi_v[k, i] < 0.0, f"spoke {i}: radial influence at its own angle must be negative"


def test_tightening_side_a_pulls_rim_toward_side_a(sym32):
    """Positive tightening of a Side A spoke -> NEGATIVE lateral (Side-B-positive) at that spoke;
    a Side B spoke -> positive."""
    wheel, solver, gen, raw, art = sym32
    for i in range(art.n_spokes):
        side = conv.spoke_side(i, wheel.indexing_origin_side)
        k = nearest_grid_index(art.theta_rim, 2 * np.pi * i / art.n_spokes)
        if side == Side.A:
            assert art.phi_u[k, i] < 0.0, f"spoke {i} (Side A): lateral influence must be negative"
        else:
            assert art.phi_u[k, i] > 0.0, f"spoke {i} (Side B): lateral influence must be positive"


def test_tightening_raises_own_tension(sym32):
    wheel, solver, gen, raw, art = sym32
    d = np.diag(art.phi_t)
    assert np.all(d > 0.0)
    # Own response dominates any other spoke's response to it.
    for i in range(art.n_spokes):
        col = art.phi_t[:, i].copy()
        col[i] = 0.0
        assert d[i] > np.max(np.abs(col))


def test_indexing_origin_offsets_columns_and_angles():
    """SPEC 6.5: declaring a different spoke 0 shifts every column by whole spoke pitches.
    Generating with origin (A, leading) must equal the (B, leading) artifact shifted by one spoke."""
    from truing_model_prep.params import HubSide, WheelClassParams
    base = fixtures.wheel_sym32()
    shifted = WheelClassParams(**{**base.__dict__, "indexing_origin_side": Side.A, "indexing_origin_lead_trail": LeadTrail.LEADING})
    solver = fixtures.solver_params(32)
    gen = fixtures.generator_params()
    r0 = generate(base, solver, gen)
    r1 = generate(shifted, solver, gen)
    assert r1.indexing_offset == 1
    # Tension block: machine spoke m of the shifted wheel is generator spoke m+1.
    n = 32
    for m in range(n):
        for j in range(n):
            assert r1.tension_n[j, m] == pytest.approx(r0.tension_n[(j + 1) % n, (m + 1) % n], rel=1e-9, abs=1e-9)
    # Lateral columns: machine angle theta of the shifted wheel is generator angle theta + 2pi/n.
    step = r0.theta_fit_lat.size // n   # 256/32 = 8 fit samples per spoke pitch
    for m in range(n):
        assert np.allclose(r1.lateral_mm[:, m], np.roll(r0.lateral_mm[:, (m + 1) % n], -step), atol=1e-9)
