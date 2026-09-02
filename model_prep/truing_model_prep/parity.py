"""Golden solver fixtures for host/firmware numerical parity (SPEC 14.2, 14.3.5).

Each case records the physical inputs (u, v, T in system units), the normalised
residual, and the host reference outputs per layout: d_ls, d_shape, d_cm, d_adj,
J and the tension scale s. The Phase 1c firmware port must reproduce them on
identical inputs within a stated tolerance.
"""
from __future__ import annotations

import json

import numpy as np

from .artifact import LAYOUT_FULL, LAYOUT_TENSION_ABSENT, Artifact
from .params import SolverParams, WheelClassParams
from .solver import SolverRefusal, ls_invert, predict, solve


def export_parity_fixtures(art: Artifact, wheel: WheelClassParams, solver: SolverParams, out_path: str,
                           n_cases: int = 4, seed: int = 2026) -> int:
    rng = np.random.default_rng(seed)
    n, nra = art.n_spokes, art.n_rim_angles
    cases = []
    for k in range(n_cases):
        d_applied = rng.uniform(-0.1, 0.1, n)
        T_b = solver.target_tension_n * (1.0 + rng.uniform(-0.05, 0.05, n))
        y_b = np.concatenate([rng.normal(0.0, 0.02, nra), rng.normal(0.0, 0.01, nra), T_b])
        y = predict(art, y_b, d_applied)
        u, v, T = y[:nra], y[nra: 2 * nra], y[2 * nra:]
        case = {"case": k, "d_applied_rev": d_applied.tolist(), "u_mm": u.tolist(), "v_mm": v.tolist(), "T_n": T.tolist(),
                "layouts": {}}
        for layout in (LAYOUT_FULL, LAYOUT_TENSION_ABSENT):
            d_ls, y_tilde, s = ls_invert(art, solver, layout, u, v, T if layout == LAYOUT_FULL else None)
            entry = {"y_tilde_active": y_tilde.tolist(), "d_ls_rev": d_ls.tolist(),
                     "cost_j": float(y_tilde @ y_tilde) / y_tilde.size, "n_active_rows": int(y_tilde.size),
                     "s_scale_n": s}
            try:
                r = solve(art, solver, layout, u, v, T if layout == LAYOUT_FULL else None, wheel.asymmetric,
                          wheel.indexing_origin_side)
                entry.update({"d_shape_rev": r.d_shape.tolist(), "d_cm_rev": r.d_cm.tolist(), "d_adj_rev": r.d_adj.tolist(),
                              "mean_tension_targeting_applied": r.mean_tension_targeting_applied,
                              "policy_reason": r.policy_reason, "refused": None})
            except SolverRefusal as e:
                entry.update({"refused": e.reason})
            case["layouts"][layout] = entry
        cases.append(case)
    doc = {"artifact_id": art.artifact_id, "generating_fingerprint": art.generating_fingerprint,
           "content_hash": art.content_hash, "tolerance_rel": 1e-5, "tolerance_abs_rev": 1e-7,
           "note": "Host reference outputs (numpy float64). Firmware float32 parity is judged against the stated tolerances.",
           "cases": cases}
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1, allow_nan=False)
    return len(cases)
