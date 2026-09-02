"""Host reference implementation of the truing solve (SPEC 8.2-8.6, 8.11).

This is the oracle for the firmware port (Phase 1c parity, SPEC 14.3.5) and the
solver used by the Phase 1b host gate. It applies:

  * tolerance normalisation and sqrt(w) weighting (SPEC 8.3.1);
  * the tension residual T - s*T_norm with s the LS-estimated current scale;
  * the TWO-PART solve (SPEC 8.4): shape correction with the n_mt component
    removed, mean-tension shift as a per-spoke VECTOR from the measured c;
  * the row-count-normalised cost J (SPEC 8.6) and per-channel convergence.

d_adj = -d_ls (the reference one-part method) is deliberately NOT implemented.
"""
from __future__ import annotations

from dataclasses import dataclass

import numpy as np

from .artifact import LAYOUT_FULL, LAYOUT_TENSION_ABSENT, Artifact, row_scaling
from .conventions import spoke_side
from .params import Side, SolverParams


class SolverRefusal(RuntimeError):
    def __init__(self, reason: str, message: str):
        super().__init__(f"{reason}: {message}")
        self.reason = reason


@dataclass
class SolveResult:
    layout: str
    d_ls: np.ndarray            # recovered disturbance (SPEC 14.3: d_ls ~ d_applied)
    d_shape: np.ndarray         # Part 1: -(d_ls - proj_nmt d_ls)  (TENSION_ABSENT: -d_ls, no projection)
    d_cm: np.ndarray            # Part 2: per-spoke vector (zeros when refused)
    d_adj: np.ndarray           # d_shape + d_cm
    cost_j: float
    n_active_rows: int
    mean_tension_targeting_applied: bool
    policy_reason: str | None   # MEAN_TENSION_MODEL_UNAVAILABLE when Part 2 is refused
    s_scale: float | None       # LS estimate of the current overall tension scale


def normalized_residual(art: Artifact, solver: SolverParams, u: np.ndarray, v: np.ndarray, T: np.ndarray | None,
                        u0: np.ndarray | None = None, v0: np.ndarray | None = None) -> tuple[np.ndarray, float | None]:
    """Y-tilde for the FULL row order (lateral, radial, tension); tension rows NaN when T is None."""
    n, nra = art.n_spokes, art.n_rim_angles
    u0 = np.zeros(nra) if u0 is None else u0
    v0 = np.zeros(nra) if v0 is None else v0
    scale = row_scaling(solver, nra, n)
    y = np.empty(2 * nra + n)
    y[:nra] = (u - u0) * scale[:nra]
    y[nra: 2 * nra] = (v - v0) * scale[nra: 2 * nra]
    s = None
    if T is None:
        y[2 * nra:] = np.nan
    else:
        cm = art.common_mode_direction
        T_norm = cm.T_target_assumed if cm.T_target_assumed is not None else np.ones(n)
        s = float((T @ T_norm) / (T_norm @ T_norm))          # current overall scale (SPEC 8.3.1)
        y[2 * nra:] = (T - s * T_norm) * scale[2 * nra:]
    return y, s


def cost(y_tilde_active: np.ndarray) -> float:
    """J(L) = ||r_active||^2 / n_active_rows (SPEC 8.6)."""
    return float(y_tilde_active @ y_tilde_active) / y_tilde_active.size


def mean_tension_shift(art: Artifact, solver: SolverParams, T: np.ndarray, wheel_asymmetric: bool,
                       origin_side: Side) -> tuple[np.ndarray, bool, str | None]:
    """SPEC 8.4 Part 2 as a per-spoke VECTOR. Symmetric: (target - mean(T)) / c for every spoke.
    Asymmetric compensation is OPEN and is REFUSED, never approximated."""
    n = art.n_spokes
    cm = art.common_mode_direction
    if not cm.identified:
        return np.zeros(n), False, "MEAN_TENSION_MODEL_UNAVAILABLE"
    if wheel_asymmetric:
        return np.zeros(n), False, "MEAN_TENSION_MODEL_UNAVAILABLE"
    c = np.array([art.c_side_a if spoke_side(i, origin_side) == Side.A else art.c_side_b for i in range(n)])
    d_cm = (solver.target_tension_n - float(np.mean(T))) / c
    return d_cm, True, None


def ls_invert(art: Artifact, solver: SolverParams, layout: str, u: np.ndarray, v: np.ndarray, T: np.ndarray | None,
              u0=None, v0=None) -> tuple[np.ndarray, np.ndarray, float | None]:
    """Eq. 4 alone: d_ls = Phi-dagger(L) Y-tilde on the active row set, with NO policy applied.
    Returns (d_ls, y_tilde_active, s). Used by the round-trip gate and the parity fixtures."""
    y_full, s = normalized_residual(art, solver, u, v, T, u0, v0)
    L = art.layout(layout)
    y = y_full[L.row_mask]
    if np.any(~np.isfinite(y)):
        raise SolverRefusal("PARTIAL_WHEEL_STATE", "active row set contains missing observations")
    return L.phi_pseudoinverse @ y, y, s


def solve(art: Artifact, solver: SolverParams, layout: str, u: np.ndarray, v: np.ndarray, T: np.ndarray | None,
          wheel_asymmetric: bool, origin_side: Side, u0=None, v0=None, apply_mean_tension: bool = True) -> SolveResult:
    y_full, s = normalized_residual(art, solver, u, v, T, u0, v0)
    L = art.layout(layout)
    y = y_full[L.row_mask]
    if np.any(~np.isfinite(y)):
        raise SolverRefusal("PARTIAL_WHEEL_STATE", "active row set contains missing observations")
    d_ls = L.phi_pseudoinverse @ y                                          # Eq. 4 on the ACTIVE row set
    cm = art.common_mode_direction
    if layout == LAYOUT_FULL:
        if not cm.identified:
            raise SolverRefusal("MEAN_TENSION_MODEL_UNAVAILABLE",
                                "n_mt unidentified: solve under TENSION_ABSENT, no projection (SPEC 8.4 Part 1)")
        n_mt = cm.vector
        d_shape = -(d_ls - (n_mt @ d_ls) * n_mt)                            # Part 1
    elif layout == LAYOUT_TENSION_ABSENT:
        d_shape = -d_ls                                                     # minimum-norm: no n_mt component by construction
    else:
        raise SolverRefusal("ARTIFACT_INVALID", f"unknown layout {layout}")
    policy = None
    applied = False
    d_cm = np.zeros(art.n_spokes)
    if layout == LAYOUT_FULL and apply_mean_tension and T is not None:
        d_cm, applied, policy = mean_tension_shift(art, solver, T, wheel_asymmetric, origin_side)
    elif layout == LAYOUT_TENSION_ABSENT:
        policy = "MEAN_TENSION_MODEL_UNAVAILABLE" if not cm.identified else None
    return SolveResult(layout=layout, d_ls=d_ls, d_shape=d_shape, d_cm=d_cm, d_adj=d_shape + d_cm,
                       cost_j=cost(y), n_active_rows=int(y.size), mean_tension_targeting_applied=applied,
                       policy_reason=policy, s_scale=s)


def predict(art: Artifact, y_b: np.ndarray, d: np.ndarray) -> np.ndarray:
    """Eq. 2: Y-hat = Y_b + Phi d (FULL row order, physical units)."""
    return y_b + art.phi @ d


@dataclass
class Convergence:
    geometric_converged: bool
    max_lateral_mm: float
    max_radial_mm: float
    non_uniformity: dict
    mean_error_n: dict
    tension_compliant_numeric: bool | None


def evaluate_convergence(solver: SolverParams, u: np.ndarray, v: np.ndarray, T: np.ndarray | None,
                         origin_side: Side, T_norm: np.ndarray | None, u0=None, v0=None) -> Convergence:
    """SPEC 8.6 per-channel test. Verification-grade gating is the caller's (needs statuses)."""
    u0 = np.zeros_like(u) if u0 is None else u0
    v0 = np.zeros_like(v) if v0 is None else v0
    ml, mr = float(np.max(np.abs(u - u0))), float(np.max(np.abs(v - v0)))
    geo = ml <= solver.tol_lateral_mm and mr <= solver.tol_radial_mm
    if T is None:
        return Convergence(geo, ml, mr, {}, {}, None)
    n = T.size
    T_norm = np.ones(n) if T_norm is None else T_norm
    target = solver.target_tension_n * T_norm
    nu, me = {}, {}
    ok = True
    for side in (Side.A, Side.B):
        idx = [i for i in range(n) if spoke_side(i, origin_side) == side]
        Ts = T[idx]
        mean = float(np.mean(Ts))
        nu[side.value] = float(np.std(Ts) / mean)                 # POPULATION std (SPEC 11.2)
        me[side.value] = float(abs(mean - np.mean(target[idx])))
        ok = ok and nu[side.value] <= solver.tol_tension_cv and me[side.value] <= solver.tol_mean_tension_error_n
    return Convergence(geo, ml, mr, nu, me, ok)
