"""SPEC 11.4 artifact schema, SPEC 8.7 three load-time checks, fingerprint sensitivity."""
import json
from dataclasses import replace

import numpy as np
import pytest

from truing_model_prep import artifact as art_mod
from truing_model_prep import fixtures
from truing_model_prep.artifact import ArtifactInvalid
from truing_model_prep.generator import generate


def test_schema_fields_present(sym32):
    _, _, _, _, art = sym32
    d = art_mod.to_dict(art)
    for key in ("schema_version", "artifact_id", "generating_fingerprint", "content_hash", "source", "source_u", "source_v",
                "source_t", "n_fit_samples_lat", "n_fit_samples_rad", "generating_parameters", "generator_version", "units",
                "sign_conventions", "indexing_origin", "n_rim_angles", "N_lat", "N_rad", "Phi_u", "Phi_v", "Phi_t",
                "Phi_t_class_map", "common_mode_direction", "weights_used", "c_side_a", "c_side_b", "layouts"):
        assert key in d, key
    assert d["Phi_t_class_map"][0] == {"index": 0, "side": "B", "lead_trail": "leading"}
    assert d["Phi_t_class_map"][1]["side"] == "A"
    assert [l["layout_id"] for l in d["layouts"]] == ["FULL", "TENSION_ABSENT"]
    assert d["generating_parameters"]["bike_wheel_calc_commit"].startswith("6fc380c")
    assert d["units"]["adjustment"].startswith("revolutions")
    json.dumps(d, allow_nan=False)   # serialisable, no NaN placeholders


def test_save_load_roundtrip_and_validation(sym32, tmp_path):
    wheel, solver, _, _, art = sym32
    p = tmp_path / "a.json"
    art_mod.save(art, str(p))
    back = art_mod.load(str(p))
    assert back.generating_fingerprint == art.generating_fingerprint
    assert back.content_hash == art.content_hash
    assert np.allclose(back.phi, art.phi)
    assert np.allclose(back.layout("FULL").phi_pseudoinverse, art.layout("FULL").phi_pseudoinverse)
    art_mod.validate(back, art.generating_fingerprint, 32, 32, solver.N_lat, solver.N_rad, solver)


def test_integrity_check_detects_tampering(sym32, tmp_path):
    wheel, solver, _, _, art = sym32
    p = tmp_path / "a.json"
    art_mod.save(art, str(p))
    d = json.load(open(p))
    d["Phi_t"]["matrix"][0][0] += 1e-3
    back = art_mod.from_dict(d)
    with pytest.raises(ArtifactInvalid, match="content_hash"):
        art_mod.validate(back, art.generating_fingerprint, 32, 32, solver.N_lat, solver.N_rad, solver)


def test_compatibility_needs_an_independent_expectation(sym32):
    """A self-consistent artifact for the WRONG wheel must be refused by the fingerprint the
    wheel-class configuration carries, not by anything inside the artifact (SPEC 8.7, 11.4)."""
    wheel, solver, gen, _, art = sym32
    other = replace(wheel, hub_side_a=fixtures.HubSide(45.0, 30.0), hub_side_b=fixtures.HubSide(45.0, 30.0))
    raw2 = generate(other, solver, gen)
    art2 = art_mod.assemble(raw2, other, solver, gen, artifact_id="other")
    assert art2.n_spokes == art.n_spokes and art2.phi.shape == art.phi.shape   # identical DIMENSIONS
    assert art2.generating_fingerprint != art.generating_fingerprint
    art_mod.validate(art2, art2.generating_fingerprint, 32, 32, solver.N_lat, solver.N_rad, solver)   # self-consistent
    with pytest.raises(ArtifactInvalid, match="fingerprint"):
        art_mod.validate(art2, art.generating_fingerprint, 32, 32, solver.N_lat, solver.N_rad, solver)


def test_shape_mismatch_aborts_never_reshapes(sym32):
    wheel, solver, _, _, art = sym32
    with pytest.raises(ArtifactInvalid):
        art_mod.validate(art, art.generating_fingerprint, 36, 36, solver.N_lat, solver.N_rad, solver)
    with pytest.raises(ArtifactInvalid):
        art_mod.validate(art, art.generating_fingerprint, 32, 32, solver.N_lat + 1, solver.N_rad, solver)


def test_fingerprint_covers_every_generating_parameter(sym32):
    wheel, solver, gen, _, art = sym32
    base = art.generating_fingerprint
    src = {"u": "analytical", "v": "analytical", "t": "analytical"}
    assert art_mod.generating_fingerprint(replace(wheel, nipple_thread_pitch_mm=0.5), solver, gen, src) != base
    assert art_mod.generating_fingerprint(wheel, replace(solver, trust_tension=2e-5), gen, src) != base
    assert art_mod.generating_fingerprint(wheel, replace(solver, tol_lateral_mm=0.2), gen, src) != base
    assert art_mod.generating_fingerprint(wheel, replace(solver, rank_tolerance=1e-5), gen, src) != base
    assert art_mod.generating_fingerprint(wheel, replace(solver, n_rim_angles=36), gen, src) != base
    assert art_mod.generating_fingerprint(wheel, solver, replace(gen, n_fit_samples_rad=128), src) != base
    assert art_mod.generating_fingerprint(wheel, solver, gen, {**src, "t": "literature_seeded"}) != base
    assert art_mod.generating_fingerprint(wheel, solver, gen, src) == base   # deterministic


def test_t_target_normalization_enforced(sym32):
    wheel, solver, _, _, art = sym32
    cm = art.common_mode_direction
    if cm.T_target_assumed is not None:
        bad = replace(cm, T_target_assumed=cm.T_target_assumed * 1.2)
        art_bad = replace(art, common_mode_direction=bad)
        with pytest.raises(ArtifactInvalid, match="mean 1.0"):
            art_mod.validate(art_bad, art.generating_fingerprint, 32, 32, solver.N_lat, solver.N_rad, solver)
