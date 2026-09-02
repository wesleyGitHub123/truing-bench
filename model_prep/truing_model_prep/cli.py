"""Command line: generate the fixture artifact and report its structural findings.

    python -m truing_model_prep.cli generate-fixture --out golden/fixture_sym32_artifact.json
    python -m truing_model_prep.cli report golden/fixture_sym32_artifact.json
"""
from __future__ import annotations

import argparse
import sys

import numpy as np

from . import artifact as art_mod
from . import fixtures
from .generator import generate


def report(art) -> str:
    cm = art.common_mode_direction
    lines = [
        f"artifact {art.artifact_id}  fingerprint {art.generating_fingerprint[:16]}...  content {art.content_hash[:16]}...",
        f"n_spokes={art.n_spokes} n_rim_angles={art.n_rim_angles} N_lat={art.N_lat} N_rad={art.N_rad} "
        f"fit samples lat/rad={art.n_fit_samples_lat}/{art.n_fit_samples_rad}",
        f"bracing angles A/B = {art.bracing_angle_a_rad:.4f} / {art.bracing_angle_b_rad:.4f} rad",
        f"Fourier fit residual RMS: lateral max {art.fit_rms_lat.max():.3e} mm, radial max {art.fit_rms_rad.max():.3e} mm",
        f"Phi_u range [{art.phi_u.min():.4f}, {art.phi_u.max():.4f}] mm/rev; Phi_v range [{art.phi_v.min():.4f}, {art.phi_v.max():.4f}] mm/rev; "
        f"Phi_t diag mean {np.mean(np.diag(art.phi_t)):.1f} N/rev",
    ]
    for l in art.layouts:
        sv = l.singular_values
        lines.append(f"layout {l.layout_id:15s} rows={int(l.row_mask.sum()):3d} eff_rank={l.effective_rank} "
                     f"eff_cond={l.effective_condition_number:.3e} null_dim={l.expected_null_dim} "
                     f"sigma1={sv[0]:.3e} sigma_min={sv[-1]:.3e}")
    lines.append(f"n_mt identified={cm.identified} null_dim={cm.displacement_null_dim} cond_i={cm.cond_i_residual} "
                 f"cond_ii={cm.cond_ii_residual} :: {cm.note}")
    return "\n".join(lines)


def main(argv=None) -> int:
    p = argparse.ArgumentParser(prog="truing_model_prep")
    sub = p.add_subparsers(dest="cmd", required=True)
    g = sub.add_parser("generate-fixture", help="generate the SYNTHETIC sym32 fixture artifact")
    g.add_argument("--out", required=True)
    g.add_argument("--asym36", action="store_true", help="generate the asymmetric 36-spoke fixture instead")
    r = sub.add_parser("report", help="print an artifact's structural findings")
    r.add_argument("path")
    f = sub.add_parser("parity-fixtures", help="export golden solver fixtures for firmware parity (SPEC 14.3.5)")
    f.add_argument("--artifact", required=True)
    f.add_argument("--out", required=True)
    f.add_argument("--cases", type=int, default=4)
    e = sub.add_parser("export-c-fixtures", help="write the compact binary artifact and parity cases as C headers")
    e.add_argument("--artifact", required=True)
    e.add_argument("--parity", required=True)
    e.add_argument("--artifact-c", required=True, help="output .c path for the blob definition")
    e.add_argument("--parity-h", required=True, help="output .h path for the parity cases")
    e.add_argument("--name", default="fixture_sym32")
    e.add_argument("--id", type=int, default=1)
    a = p.parse_args(argv)
    if a.cmd == "export-c-fixtures":
        from .export import write_c_fixtures
        art = art_mod.load(a.artifact)
        paths = write_c_fixtures(art, a.id, a.parity, a.artifact_c, a.parity_h, a.name)
        print("wrote", *paths)
        return 0
    if a.cmd == "parity-fixtures":
        from .parity import export_parity_fixtures
        wheel = fixtures.wheel_sym32()
        solver = fixtures.solver_params(wheel.n_spokes)
        n = export_parity_fixtures(art_mod.load(a.artifact), wheel, solver, a.out, a.cases)
        print(f"wrote {n} parity cases to {a.out}")
        return 0
    if a.cmd == "generate-fixture":
        wheel = fixtures.wheel_asym36() if a.asym36 else fixtures.wheel_sym32()
        solver = fixtures.solver_params(wheel.n_spokes)
        gen = fixtures.generator_params()
        raw = generate(wheel, solver, gen)
        art = art_mod.assemble(raw, wheel, solver, gen, artifact_id="fixture_asym36" if a.asym36 else "fixture_sym32")
        art_mod.save(art, a.out)
        print(report(art))
        return 0
    if a.cmd == "report":
        print(report(art_mod.load(a.path)))
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())
