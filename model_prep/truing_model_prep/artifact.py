"""Influence-matrix artifact: assembly, normalisation, per-layout pseudoinverses,
rank/conditioning, common-mode direction, fingerprints, storage and load-time
validation (SPEC 8.3.1, 8.5, 8.7, 8.7.1, 8.11 R3, 11.4).
"""
from __future__ import annotations

import hashlib
import json
from dataclasses import dataclass, field
from typing import Any

import numpy as np

from . import BIKE_WHEEL_CALC_COMMIT, GENERATOR_VERSION
from .conventions import spoke_class, spoke_side
from .fourier import expand
from .generator import RawInfluence
from .params import BlockSource, GeneratorParams, Side, SolverParams, WheelClassParams, to_jsonable

SCHEMA_VERSION = 1
LAYOUT_FULL = "FULL"
LAYOUT_TENSION_ABSENT = "TENSION_ABSENT"


class ArtifactInvalid(ValueError):
    """SPEC 8.7 / 13.1: aliasing, dimension mismatch, hash mismatch -> abort loudly."""


# ---------------------------------------------------------------- fingerprints
def _canonical(obj: Any) -> bytes:
    return json.dumps(to_jsonable(obj), sort_keys=True, separators=(",", ":"), allow_nan=False).encode("utf-8")


def generating_fingerprint(wheel: WheelClassParams, solver: SolverParams, gen: GeneratorParams,
                           sources: dict[str, str]) -> str:
    """SHA-256 over every model-preparation parameter that can change Phi, Phi-dagger or
    admission (SPEC 8.7). Runtime/session fields are deliberately absent."""
    payload = {
        "wheel": to_jsonable(wheel),
        "solver": to_jsonable(solver),
        "generator": to_jsonable(gen),
        "sources": sources,
        "generator_version": GENERATOR_VERSION,
        "bike_wheel_calc_commit": BIKE_WHEEL_CALC_COMMIT,
    }
    return hashlib.sha256(_canonical(payload)).hexdigest()


def content_hash(arrays: list[np.ndarray]) -> str:
    """SHA-256 over the stored numbers themselves (SPEC 8.7 integrity), float64 little-endian."""
    h = hashlib.sha256()
    for a in arrays:
        h.update(np.ascontiguousarray(a, dtype="<f8").tobytes())
    return h.hexdigest()


# ---------------------------------------------------------------- assembly
@dataclass
class Layout:
    layout_id: str
    row_mask: np.ndarray                 # bool (n_full_rows,)
    phi_pseudoinverse: np.ndarray        # (n_spokes, n_layout_rows)
    singular_values: np.ndarray
    effective_rank: int
    effective_condition_number: float
    expected_null_dim: int


@dataclass
class CommonModeDirection:
    identified: bool
    vector: np.ndarray | None
    normalization: str
    sign_convention: str
    cond_i_residual: float | None
    cond_i_tolerance: float
    cond_ii_residual: float | None
    cond_ii_tolerance: float
    uniqueness_margin: float | None
    uniqueness_threshold: float
    T_target_assumed: np.ndarray | None
    T_target_normalization: str
    displacement_null_dim: int
    note: str
    # Diagnostic decomposition of cond_i (recorded so a reviewer can see WHAT breaks invariance):
    # the part of the displacement response to n_mt that is a UNIFORM radial offset (rim
    # contraction/expansion, the n = 0 radial mode) versus everything else.
    cond_i_uniform_radial_part: float | None = None
    cond_i_remainder: float | None = None


@dataclass
class Artifact:
    schema_version: int
    artifact_id: str
    generating_fingerprint: str
    content_hash: str
    source: str
    source_u: str
    source_v: str
    source_t: str
    n_fit_samples_lat: int
    n_fit_samples_rad: int
    generating_parameters: dict
    generator_version: str
    units: dict
    sign_conventions: dict
    indexing_origin: dict
    n_spokes: int
    n_rim_angles: int
    N_lat: int
    N_rad: int
    theta_rim: np.ndarray                # machine-frame grid (n_rim_angles,)
    coeffs_lat: np.ndarray               # (n_spokes, 2N_lat+1), per spoke (SPEC 11.4: never re-compressed)
    coeffs_rad: np.ndarray
    phi_t: np.ndarray                    # (n_spokes, n_spokes)
    phi_t_class_map: list[dict]
    common_mode_direction: CommonModeDirection
    weights_used: dict
    c_side_a: float
    c_side_b: float
    layouts: list[Layout]
    fit_rms_lat: np.ndarray
    fit_rms_rad: np.ndarray
    bracing_angle_a_rad: float
    bracing_angle_b_rad: float
    # derived at load/assembly
    phi_u: np.ndarray = field(default=None)
    phi_v: np.ndarray = field(default=None)

    # -------- expansion (SPEC 11.4: compact storage, expansion at initialisation)
    def expand_displacement_blocks(self) -> None:
        self.phi_u = np.column_stack([expand(c, self.theta_rim) for c in self.coeffs_lat])
        self.phi_v = np.column_stack([expand(c, self.theta_rim) for c in self.coeffs_rad])

    @property
    def phi(self) -> np.ndarray:
        """FULL influence matrix, n_full_rows x n_spokes (SPEC 8.1)."""
        return np.vstack([self.phi_u, self.phi_v, self.phi_t])

    @property
    def n_full_rows(self) -> int:
        return 2 * self.n_rim_angles + self.n_spokes

    def layout(self, layout_id: str) -> Layout:
        for l in self.layouts:
            if l.layout_id == layout_id:
                return l
        raise KeyError(layout_id)


def row_masks(n_rim_angles: int, n_spokes: int) -> dict[str, np.ndarray]:
    n_full = 2 * n_rim_angles + n_spokes
    full = np.ones(n_full, dtype=bool)
    ta = np.zeros(n_full, dtype=bool)
    ta[: 2 * n_rim_angles] = True
    return {LAYOUT_FULL: full, LAYOUT_TENSION_ABSENT: ta}


def row_scaling(solver: SolverParams, n_rim_angles: int, n_spokes: int) -> np.ndarray:
    """Per-row factor turning Phi rows into the dimensionless, sqrt(w)-weighted rows of
    Phi-tilde (SPEC 8.3.1 steps 1-3): u/tol_lat, sqrt(w_v) v/tol_rad, sqrt(w_t) T/tol_ten."""
    s = np.empty(2 * n_rim_angles + n_spokes)
    s[:n_rim_angles] = 1.0 / solver.tol_lateral_mm
    s[n_rim_angles: 2 * n_rim_angles] = np.sqrt(solver.trust_radial) / solver.tol_radial_mm
    s[2 * n_rim_angles:] = np.sqrt(solver.trust_tension) / solver.tol_tension_n
    return s


def effective_rank_and_cond(sv: np.ndarray, rank_tolerance: float) -> tuple[int, float]:
    """SPEC 8.11 R3: retained = {sigma_k > rank_tolerance * sigma_1}."""
    if sv.size == 0 or sv[0] <= 0.0:
        return 0, float("inf")
    retained = sv[sv > rank_tolerance * sv[0]]
    return int(retained.size), float(sv[0] / retained[-1])


def compute_layout(layout_id: str, mask: np.ndarray, phi_tilde: np.ndarray, n_spokes: int,
                   rank_tolerance: float) -> Layout:
    sub = phi_tilde[mask, :]
    sv = np.linalg.svd(sub, compute_uv=False)
    rank, cond = effective_rank_and_cond(sv, rank_tolerance)
    pinv = np.linalg.pinv(sub, rcond=rank_tolerance)   # same retained-subspace rule as the rank
    return Layout(layout_id=layout_id, row_mask=mask, phi_pseudoinverse=pinv, singular_values=sv,
                  effective_rank=rank, effective_condition_number=cond,
                  expected_null_dim=int(n_spokes - rank))


def identify_common_mode(wheel: WheelClassParams, solver: SolverParams, phi_tilde: np.ndarray,
                         phi_t: np.ndarray, n_rim_angles: int, disp_null_dim: int) -> CommonModeDirection:
    """SPEC 8.4 Part 1 / 11.4. The identification PROCEDURE for asymmetric wheels is an open
    model-preparation item and is NOT invented here: asymmetric -> identified=False.
    For a symmetric wheel the specification itself names the direction, [1,...,1]/sqrt(n);
    it is adopted only if both conditions hold within the configured tolerances and the
    displacement null space is exactly one-dimensional (uniqueness)."""
    n = wheel.n_spokes
    if wheel.asymmetric:
        return CommonModeDirection(
            identified=False, vector=None, normalization="unit L2 norm",
            sign_convention="positive component sum", cond_i_residual=None,
            cond_i_tolerance=solver.n_mt_displacement_tolerance, cond_ii_residual=None,
            cond_ii_tolerance=solver.n_mt_tension_tolerance, uniqueness_margin=None,
            uniqueness_threshold=solver.n_mt_uniqueness_threshold, T_target_assumed=None,
            T_target_normalization="mean=1.0", displacement_null_dim=disp_null_dim,
            note="asymmetric wheel: identification procedure not specified (SPEC 8.4 Part 1); refused",
        )
    n_mt = np.ones(n) / np.sqrt(n)
    T_norm = np.ones(n)                                   # symmetric: every spoke at the same tension
    disp = phi_tilde[: 2 * n_rim_angles, :]               # dimensionless displacement rows
    resp = disp @ n_mt
    cond_i = float(np.linalg.norm(resp))
    uniform = np.zeros_like(resp)
    uniform[n_rim_angles:] = np.mean(resp[n_rim_angles:])   # the n = 0 radial mode of the response
    cond_i_uniform = float(np.linalg.norm(uniform))
    cond_i_rest = float(np.linalg.norm(resp - uniform))
    t_resp = phi_t @ n_mt
    proj = (t_resp @ T_norm) / (T_norm @ T_norm) * T_norm
    cond_ii = float(np.linalg.norm(t_resp - proj) / max(np.linalg.norm(t_resp), 1e-300))
    unique = disp_null_dim == 1
    margin = 1.0 if unique else 0.0
    identified = cond_i <= solver.n_mt_displacement_tolerance and cond_ii <= solver.n_mt_tension_tolerance and unique \
        and margin >= solver.n_mt_uniqueness_threshold
    note = "symmetric: [1..1]/sqrt(n) per SPEC 8.4; " + (
        "identified" if identified else
        f"NOT identified (cond_i={cond_i:.3e} tol={solver.n_mt_displacement_tolerance:.1e} "
        f"[uniform radial part {cond_i_uniform:.3e}, remainder {cond_i_rest:.3e}], "
        f"cond_ii={cond_ii:.3e} tol={solver.n_mt_tension_tolerance:.1e}, null_dim={disp_null_dim})")
    return CommonModeDirection(
        identified=bool(identified), vector=n_mt, normalization="unit L2 norm",
        sign_convention="positive component sum", cond_i_residual=cond_i,
        cond_i_tolerance=solver.n_mt_displacement_tolerance, cond_ii_residual=cond_ii,
        cond_ii_tolerance=solver.n_mt_tension_tolerance, uniqueness_margin=margin,
        uniqueness_threshold=solver.n_mt_uniqueness_threshold, T_target_assumed=T_norm,
        T_target_normalization="mean=1.0", displacement_null_dim=disp_null_dim, note=note,
        cond_i_uniform_radial_part=cond_i_uniform, cond_i_remainder=cond_i_rest,
    )


def assemble(raw: RawInfluence, wheel: WheelClassParams, solver: SolverParams, gen: GeneratorParams,
             artifact_id: str) -> Artifact:
    n = wheel.n_spokes
    nra = solver.n_rim_angles
    theta_rim = 2.0 * np.pi * np.arange(nra) / nra
    sources = {"u": BlockSource.ANALYTICAL.value, "v": BlockSource.ANALYTICAL.value, "t": BlockSource.ANALYTICAL.value}
    offset = raw.indexing_offset
    class_map = [{"index": m, "side": spoke_class(m, offset)[0].value, "lead_trail": spoke_class(m, offset)[1].value}
                 for m in range(n)]
    art = Artifact(
        schema_version=SCHEMA_VERSION, artifact_id=artifact_id, generating_fingerprint="", content_hash="",
        source=BlockSource.ANALYTICAL.value, source_u=sources["u"], source_v=sources["v"], source_t=sources["t"],
        n_fit_samples_lat=gen.n_fit_samples_lat, n_fit_samples_rad=gen.n_fit_samples_rad,
        generating_parameters={"wheel": to_jsonable(wheel), "solver": to_jsonable(solver), "generator": to_jsonable(gen),
                               "bike_wheel_calc_commit": BIKE_WHEEL_CALC_COMMIT},
        generator_version=GENERATOR_VERSION,
        units={"lateral": "mm per revolution", "radial": "mm per revolution", "tension": "N per revolution",
               "rim_angle": "rad", "adjustment": "revolutions (positive = tightening)"},
        sign_conventions={"lateral_positive": "toward Side B", "radial_positive": "outward from hub",
                          "rim_angle_positive": "CCW viewed from Side A, origin at spoke 0",
                          "bike_wheel_calc_radial": "flipped (inward-positive -> outward-positive)",
                          "bike_wheel_calc_lateral": "not flipped (NDS-positive == Side-B-positive)"},
        indexing_origin={"side": wheel.indexing_origin_side.value, "lead_trail": wheel.indexing_origin_lead_trail.value,
                         "generator_spoke_offset": offset},
        n_spokes=n, n_rim_angles=nra, N_lat=solver.N_lat, N_rad=solver.N_rad, theta_rim=theta_rim,
        coeffs_lat=raw.coeffs_lat, coeffs_rad=raw.coeffs_rad, phi_t=raw.tension_n, phi_t_class_map=class_map,
        common_mode_direction=None, weights_used={"tol_lateral_mm": solver.tol_lateral_mm, "tol_radial_mm": solver.tol_radial_mm,
                                                  "tol_tension_n": solver.tol_tension_n, "trust_radial": solver.trust_radial,
                                                  "trust_tension": solver.trust_tension},
        c_side_a=wheel.c_side_a_n_per_rev, c_side_b=wheel.c_side_b_n_per_rev, layouts=[],
        fit_rms_lat=raw.fit_rms_lat, fit_rms_rad=raw.fit_rms_rad,
        bracing_angle_a_rad=raw.bracing_angle_a_rad, bracing_angle_b_rad=raw.bracing_angle_b_rad,
    )
    art.expand_displacement_blocks()
    phi_tilde = art.phi * row_scaling(solver, nra, n)[:, None]
    masks = row_masks(nra, n)
    art.layouts = [compute_layout(lid, masks[lid], phi_tilde, n, solver.rank_tolerance) for lid in (LAYOUT_FULL, LAYOUT_TENSION_ABSENT)]
    ta = art.layout(LAYOUT_TENSION_ABSENT)
    art.common_mode_direction = identify_common_mode(wheel, solver, phi_tilde, art.phi_t, nra, ta.expected_null_dim)
    art.generating_fingerprint = generating_fingerprint(wheel, solver, gen, sources)
    art.content_hash = content_hash([art.coeffs_lat, art.coeffs_rad, art.phi_t] + [l.phi_pseudoinverse for l in art.layouts])
    return art


# ---------------------------------------------------------------- storage
def _arr(a: np.ndarray | None):
    return None if a is None else np.asarray(a, dtype=float).tolist()


def to_dict(art: Artifact) -> dict:
    cm = art.common_mode_direction
    return {
        "schema_version": art.schema_version, "artifact_id": art.artifact_id,
        "generating_fingerprint": art.generating_fingerprint, "content_hash": art.content_hash,
        "source": art.source, "source_u": art.source_u, "source_v": art.source_v, "source_t": art.source_t,
        "n_fit_samples_lat": art.n_fit_samples_lat, "n_fit_samples_rad": art.n_fit_samples_rad,
        "generating_parameters": art.generating_parameters, "generator_version": art.generator_version,
        "units": art.units, "sign_conventions": art.sign_conventions, "indexing_origin": art.indexing_origin,
        "n_spokes": art.n_spokes, "n_rim_angles": art.n_rim_angles, "N_lat": art.N_lat, "N_rad": art.N_rad,
        "theta_rim": _arr(art.theta_rim), "Phi_u": {"storage": "per_spoke_fourier", "coeffs": _arr(art.coeffs_lat)},
        "Phi_v": {"storage": "per_spoke_fourier", "coeffs": _arr(art.coeffs_rad)},
        "Phi_t": {"storage": "per_spoke_columns", "matrix": _arr(art.phi_t)}, "Phi_t_class_map": art.phi_t_class_map,
        "common_mode_direction": {
            "identified": cm.identified, "vector": _arr(cm.vector), "normalization": cm.normalization,
            "sign_convention": cm.sign_convention, "cond_i_residual": cm.cond_i_residual,
            "cond_i_tolerance": cm.cond_i_tolerance, "cond_ii_residual": cm.cond_ii_residual,
            "cond_ii_tolerance": cm.cond_ii_tolerance, "uniqueness_margin": cm.uniqueness_margin,
            "uniqueness_threshold": cm.uniqueness_threshold, "T_target_assumed": _arr(cm.T_target_assumed),
            "T_target_normalization": cm.T_target_normalization, "displacement_null_dim": cm.displacement_null_dim,
            "note": cm.note, "cond_i_uniform_radial_part": cm.cond_i_uniform_radial_part,
            "cond_i_remainder": cm.cond_i_remainder,
        },
        "weights_used": art.weights_used, "c_side_a": art.c_side_a, "c_side_b": art.c_side_b,
        "layouts": [{"layout_id": l.layout_id, "row_mask": [bool(x) for x in l.row_mask],
                     "Phi_pseudoinverse": _arr(l.phi_pseudoinverse), "singular_values": _arr(l.singular_values),
                     "effective_condition_number": l.effective_condition_number, "effective_rank": l.effective_rank,
                     "expected_null_dim": l.expected_null_dim} for l in art.layouts],
        "fit_rms_lat_mm": _arr(art.fit_rms_lat), "fit_rms_rad_mm": _arr(art.fit_rms_rad),
        "bracing_angle_a_rad": art.bracing_angle_a_rad, "bracing_angle_b_rad": art.bracing_angle_b_rad,
    }


def save(art: Artifact, path: str) -> None:
    with open(path, "w", encoding="utf-8") as f:
        json.dump(to_dict(art), f, indent=1, allow_nan=False)


def from_dict(d: dict) -> Artifact:
    if d.get("schema_version") != SCHEMA_VERSION:
        raise ArtifactInvalid(f"unsupported schema_version {d.get('schema_version')}")
    cmd = d["common_mode_direction"]
    cm = CommonModeDirection(
        identified=bool(cmd["identified"]), vector=None if cmd["vector"] is None else np.array(cmd["vector"]),
        normalization=cmd["normalization"], sign_convention=cmd["sign_convention"],
        cond_i_residual=cmd["cond_i_residual"], cond_i_tolerance=cmd["cond_i_tolerance"],
        cond_ii_residual=cmd["cond_ii_residual"], cond_ii_tolerance=cmd["cond_ii_tolerance"],
        uniqueness_margin=cmd["uniqueness_margin"], uniqueness_threshold=cmd["uniqueness_threshold"],
        T_target_assumed=None if cmd["T_target_assumed"] is None else np.array(cmd["T_target_assumed"]),
        T_target_normalization=cmd["T_target_normalization"], displacement_null_dim=int(cmd["displacement_null_dim"]),
        note=cmd.get("note", ""), cond_i_uniform_radial_part=cmd.get("cond_i_uniform_radial_part"),
        cond_i_remainder=cmd.get("cond_i_remainder"),
    )
    layouts = [Layout(layout_id=l["layout_id"], row_mask=np.array(l["row_mask"], dtype=bool),
                      phi_pseudoinverse=np.array(l["Phi_pseudoinverse"]), singular_values=np.array(l["singular_values"]),
                      effective_rank=int(l["effective_rank"]), effective_condition_number=float(l["effective_condition_number"]),
                      expected_null_dim=int(l["expected_null_dim"])) for l in d["layouts"]]
    art = Artifact(
        schema_version=d["schema_version"], artifact_id=d["artifact_id"], generating_fingerprint=d["generating_fingerprint"],
        content_hash=d["content_hash"], source=d["source"], source_u=d["source_u"], source_v=d["source_v"], source_t=d["source_t"],
        n_fit_samples_lat=int(d["n_fit_samples_lat"]), n_fit_samples_rad=int(d["n_fit_samples_rad"]),
        generating_parameters=d["generating_parameters"], generator_version=d["generator_version"], units=d["units"],
        sign_conventions=d["sign_conventions"], indexing_origin=d["indexing_origin"], n_spokes=int(d["n_spokes"]),
        n_rim_angles=int(d["n_rim_angles"]), N_lat=int(d["N_lat"]), N_rad=int(d["N_rad"]), theta_rim=np.array(d["theta_rim"]),
        coeffs_lat=np.array(d["Phi_u"]["coeffs"]), coeffs_rad=np.array(d["Phi_v"]["coeffs"]), phi_t=np.array(d["Phi_t"]["matrix"]),
        phi_t_class_map=d["Phi_t_class_map"], common_mode_direction=cm, weights_used=d["weights_used"],
        c_side_a=float(d["c_side_a"]), c_side_b=float(d["c_side_b"]), layouts=layouts,
        fit_rms_lat=np.array(d["fit_rms_lat_mm"]), fit_rms_rad=np.array(d["fit_rms_rad_mm"]),
        bracing_angle_a_rad=float(d["bracing_angle_a_rad"]), bracing_angle_b_rad=float(d["bracing_angle_b_rad"]),
    )
    art.expand_displacement_blocks()
    return art


def load(path: str) -> Artifact:
    with open(path, "r", encoding="utf-8") as f:
        return from_dict(json.load(f))


# ---------------------------------------------------------------- load-time validation (SPEC 8.7 three checks)
def validate(art: Artifact, expected_fingerprint: str | None, n_spokes: int, n_rim_angles: int,
             N_lat: int, N_rad: int, solver: SolverParams | None = None) -> None:
    """1 integrity (content_hash), 2 compatibility (INDEPENDENT expected fingerprint), 3 shape."""
    if content_hash([art.coeffs_lat, art.coeffs_rad, art.phi_t] + [l.phi_pseudoinverse for l in art.layouts]) != art.content_hash:
        raise ArtifactInvalid("content_hash mismatch: stored numbers are corrupt")
    if expected_fingerprint is not None and art.generating_fingerprint != expected_fingerprint:
        raise ArtifactInvalid("generating_fingerprint does not match the wheel-class configuration's expected fingerprint")
    if art.n_spokes != n_spokes or art.n_rim_angles != n_rim_angles or art.N_lat != N_lat or art.N_rad != N_rad:
        raise ArtifactInvalid("artifact dimensions / Fourier orders do not match the active configuration")
    if art.coeffs_lat.shape != (n_spokes, 2 * N_lat + 1) or art.coeffs_rad.shape != (n_spokes, 2 * N_rad + 1):
        raise ArtifactInvalid("coefficient set shape mismatch")
    if art.phi_t.shape != (n_spokes, n_spokes):
        raise ArtifactInvalid("Phi_t shape mismatch")
    if art.n_fit_samples_lat < 2 * N_lat + 1 or art.n_fit_samples_rad < 2 * N_rad + 1:
        raise ArtifactInvalid("recorded fit sample count below the Fourier floor (aliasing)")
    masks = row_masks(n_rim_angles, n_spokes)
    for l in art.layouts:
        if l.layout_id not in masks or not np.array_equal(l.row_mask, masks[l.layout_id]):
            raise ArtifactInvalid(f"layout {l.layout_id}: row mask does not match the configured layout")
        n_rows = int(l.row_mask.sum())
        if l.phi_pseudoinverse.shape != (n_spokes, n_rows):
            raise ArtifactInvalid(f"layout {l.layout_id}: pseudoinverse is not n_spokes x n_layout_rows")
        if solver is not None and l.effective_condition_number > solver.max_condition_number:
            raise ArtifactInvalid(f"layout {l.layout_id}: effective condition number exceeds max_condition_number")
    cm = art.common_mode_direction
    if cm.T_target_normalization != "mean=1.0":
        raise ArtifactInvalid("T_target_assumed normalization convention is not mean=1.0")
    if cm.T_target_assumed is not None and abs(float(np.mean(cm.T_target_assumed)) - 1.0) > 1e-9:
        raise ArtifactInvalid("T_target_assumed is not normalized to mean 1.0")
    if cm.identified and cm.vector is None:
        raise ArtifactInvalid("common_mode_direction identified without a vector")
    if not art.generating_parameters["wheel"]["asymmetric"] and cm.identified:
        ref = np.ones(n_spokes) / np.sqrt(n_spokes)
        if np.linalg.norm(cm.vector - ref) > 1e-9:
            raise ArtifactInvalid("symmetric wheel: n_mt is not [1..1]/sqrt(n)")
