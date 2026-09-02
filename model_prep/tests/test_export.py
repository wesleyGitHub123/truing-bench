"""Binary export: header layout, integrity hash, and a Python re-read that must reproduce the
JSON artifact bit-for-bit at float32 (the firmware loader parses the same bytes)."""
import hashlib
import json
import struct
from pathlib import Path

import numpy as np
import pytest

from truing_model_prep import artifact as art_mod
from truing_model_prep.export import HEADER_BYTES, MAGIC, SCHEMA_VERSION, c_array, export_binary, parity_c_header


@pytest.fixture(scope="module")
def sym32_artifact(sym32):
    return sym32[4]


@pytest.fixture(scope="module")
def parity_doc():
    with open(Path(__file__).resolve().parents[1] / "golden" / "parity_sym32.json", "r", encoding="utf-8") as f:
        return json.load(f)


def _read(blob: bytes):
    magic, ver, _res, aid = struct.unpack_from("<IHHI", blob, 0)
    name = blob[12:44].split(b"\0")[0].decode()
    fp = blob[44:76]
    chash = blob[76:108]
    n, nra, N_lat, N_rad, asym, ident, n_layouts, t_present = struct.unpack_from("<8B", blob, 108)
    (plen,) = struct.unpack_from("<I", blob, 116)
    return dict(magic=magic, ver=ver, aid=aid, name=name, fp=fp, chash=chash, n=n, nra=nra, N_lat=N_lat, N_rad=N_rad,
                asym=asym, ident=ident, n_layouts=n_layouts, t_present=t_present, plen=plen)


def test_header_and_integrity(sym32_artifact):
    blob = export_binary(sym32_artifact, 7)
    h = _read(blob)
    assert h["magic"] == MAGIC and h["ver"] == SCHEMA_VERSION and h["aid"] == 7
    assert h["name"] == sym32_artifact.artifact_id[:31]
    assert h["fp"].hex() == sym32_artifact.generating_fingerprint
    assert h["plen"] == len(blob) - HEADER_BYTES
    assert hashlib.sha256(blob[HEADER_BYTES:]).digest() == h["chash"]
    assert (h["n"], h["nra"], h["N_lat"], h["N_rad"]) == (32, 32, sym32_artifact.N_lat, sym32_artifact.N_rad)
    assert h["asym"] == 0 and h["n_layouts"] == 2 and h["t_present"] == 1
    assert h["ident"] == int(sym32_artifact.common_mode_direction.identified)


def test_payload_reproduces_matrices_at_float32(sym32_artifact):
    art = sym32_artifact
    blob = export_binary(art, 1)
    h = _read(blob)
    p = blob[HEADER_BYTES:]
    off = 0
    w = struct.unpack_from("<7f", p, off); off += 28
    assert w[:5] == tuple(np.float32(art.weights_used[k]) for k in
                          ("tol_lateral_mm", "tol_radial_mm", "tol_tension_n", "trust_radial", "trust_tension"))
    off += 24 + 1                                    # diagnostics + displacement_null_dim
    n = h["n"]
    if h["t_present"]:
        t = np.frombuffer(p, dtype="<f4", count=n, offset=off); off += 4 * n
        assert np.allclose(t, art.common_mode_direction.T_target_assumed, rtol=1e-6, atol=0)
    if h["ident"]:
        v = np.frombuffer(p, dtype="<f4", count=n, offset=off); off += 4 * n
        assert np.allclose(v, art.common_mode_direction.vector, rtol=1e-6, atol=0)
    ncl, ncr = 2 * h["N_lat"] + 1, 2 * h["N_rad"] + 1
    cl = np.frombuffer(p, dtype="<f4", count=n * ncl, offset=off).reshape(n, ncl); off += 4 * n * ncl
    cr = np.frombuffer(p, dtype="<f4", count=n * ncr, offset=off).reshape(n, ncr); off += 4 * n * ncr
    pt = np.frombuffer(p, dtype="<f4", count=n * n, offset=off).reshape(n, n); off += 4 * n * n
    assert np.array_equal(cl, art.coeffs_lat.astype("<f4")) and np.array_equal(cr, art.coeffs_rad.astype("<f4"))
    assert np.array_equal(pt, art.phi_t.astype("<f4"))
    off += n                                         # class map
    for lid in (art_mod.LAYOUT_FULL, art_mod.LAYOUT_TENSION_ABSENT):
        L = art.layout(lid)
        code, rank, null, _r, nrows, _r2, cond = struct.unpack_from("<BBBBHHf", p, off); off += 12
        assert (rank, null, nrows) == (L.effective_rank, L.expected_null_dim, int(L.row_mask.sum()))
        assert cond == np.float32(L.effective_condition_number)
        words = struct.unpack_from("<4I", p, off); off += 16
        mask = np.array([(words[r // 32] >> (r % 32)) & 1 for r in range(len(L.row_mask))], dtype=bool)
        assert np.array_equal(mask, L.row_mask)
        pinv = np.frombuffer(p, dtype="<f4", count=n * nrows, offset=off).reshape(n, nrows); off += 4 * n * nrows
        assert np.array_equal(pinv, L.phi_pseudoinverse.astype("<f4"))
    assert off == len(p)


def test_c_emitters_are_well_formed(sym32_artifact, parity_doc):
    blob = export_binary(sym32_artifact, 1)
    src = c_array(blob, "blob", "test")
    assert f"const uint8_t blob[{len(blob)}]" in src and src.count("0x") == len(blob)
    hdr = parity_c_header(parity_doc, "par")
    assert "par_cases[4]" in hdr and hdr.count("{1, 0,") + hdr.count("{1, 1,") == 4   # one FULL entry per case
    assert parity_doc["generating_fingerprint"] in hdr
