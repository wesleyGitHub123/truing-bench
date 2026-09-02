"""Analytical influence-function generation with bike-wheel-calc (SPEC 8.8, 8.10, 8.10.1).

For each spoke i a unit nipple adjustment (one revolution = `nipple_thread_pitch`
of shortening) is applied through the adjustment matrix A_adj:

    (K_rim + K_spk) dm = A_adj a        with a = pitch * e_i

and the lateral / radial rim deflection and every spoke's tension change are
read back. Columns are generated PER SPOKE (never one rotated curve, SPEC 8.8);
Phi_t comes from the same solution through spoke_tension_change(), which is the
per-spoke tension response the API exposes (confirmed in source, SPEC 8.10.1).
"""
from __future__ import annotations

from dataclasses import dataclass

import numpy as np
from bikewheelcalc import BicycleWheel, Hub, ModeMatrix, Rim

from . import conventions as conv
from .fourier import fit
from .params import GeneratorParams, SolverParams, WheelClassParams


@dataclass
class RawInfluence:
    """Influence functions in SYSTEM conventions (mm per revolution, N per revolution),
    indexed by MACHINE spoke, evaluated on the machine-frame fit grid."""

    theta_fit_lat: np.ndarray            # machine-frame angles used for the lateral fit
    theta_fit_rad: np.ndarray
    lateral_mm: np.ndarray               # (n_fit_lat, n_spokes)
    radial_mm: np.ndarray                # (n_fit_rad, n_spokes)
    tension_n: np.ndarray                # (n_spokes, n_spokes): [j, i] = dT_j per revolution of spoke i
    coeffs_lat: np.ndarray               # (n_spokes, 2*N_lat+1)
    coeffs_rad: np.ndarray               # (n_spokes, 2*N_rad+1)
    fit_rms_lat: np.ndarray              # per-spoke fit residual (mm)
    fit_rms_rad: np.ndarray
    indexing_offset: int
    bracing_angle_a_rad: float
    bracing_angle_b_rad: float
    rr_modes: int


def build_wheel(wheel: WheelClassParams, T_avg_n: float) -> BicycleWheel:
    w = BicycleWheel()
    # Side A -> DS, Side B -> NDS (SPEC 6.4.1). Hub widths are rim-plane-to-flange distances.
    w.hub = Hub(
        diameter_nds=float(wheel.hub_side_b.flange_diameter_mm) / conv.MM_PER_M,
        diameter_ds=float(wheel.hub_side_a.flange_diameter_mm) / conv.MM_PER_M,
        width_nds=float(wheel.hub_side_b.flange_offset_mm) / conv.MM_PER_M,
        width_ds=float(wheel.hub_side_a.flange_offset_mm) / conv.MM_PER_M,
    )
    s = wheel.rim_section
    w.rim = Rim(
        radius=wheel.rim_diameter_mm / 2.0 / conv.MM_PER_M,
        area=s.area_m2,
        I_rad=s.I_rad_m4,
        I_lat=s.I_lat_m4,
        J_tor=s.J_tor_m4,
        I_warp=s.I_warp_m6,
        young_mod=s.young_mod_pa,
        shear_mod=s.shear_mod_pa,
    )
    w.lace_cross(
        n_spokes=wheel.n_spokes,
        n_cross=wheel.n_cross,
        diameter=wheel.spoke_diameter_mm / conv.MM_PER_M,
        young_mod=wheel.spoke_material_modulus_pa,
    )
    w.apply_tension(T_avg=float(T_avg_n))
    return w


def bracing_angles(w: BicycleWheel) -> tuple[float, float]:
    """Bracing angle per side from the laced geometry: angle between the spoke and the rim plane."""
    a, b = [], []
    for s in w.spokes:
        ang = float(np.arcsin(abs(s.n[0])))
        (b if s.hub_pt[2] > 0 else a).append(ang)   # NDS hub z > 0 -> Side B
    return float(np.mean(a)), float(np.mean(b))


def generate(wheel: WheelClassParams, solver: SolverParams, gen: GeneratorParams) -> RawInfluence:
    n = wheel.n_spokes
    if n % 4 != 0:
        raise ValueError("the four-class structure needs n_spokes divisible by 4 (SPEC 8.8)")
    w = build_wheel(wheel, solver.target_tension_n)
    mm = ModeMatrix(w, N=gen.rr_modes)
    K = mm.K_rim(tension=gen.include_tension_stiffness) + mm.K_spk(
        smeared_spokes=gen.smeared_spokes, tension=gen.include_tension_stiffness
    )
    A = mm.A_adj()
    pitch_m = wheel.nipple_thread_pitch_mm / conv.MM_PER_M

    offset = conv.indexing_offset(wheel.indexing_origin_side, wheel.indexing_origin_lead_trail)
    theta_lat = np.linspace(0.0, 2.0 * np.pi, gen.n_fit_samples_lat, endpoint=False)
    theta_rad = np.linspace(0.0, 2.0 * np.pi, gen.n_fit_samples_rad, endpoint=False)
    th_lat_bwc = conv.machine_angle_to_bwc(theta_lat, offset, n)
    th_rad_bwc = conv.machine_angle_to_bwc(theta_rad, offset, n)

    lateral = np.zeros((theta_lat.size, n))
    radial = np.zeros((theta_rad.size, n))
    tension = np.zeros((n, n))
    coeffs_lat = np.zeros((n, 2 * solver.N_lat + 1))
    coeffs_rad = np.zeros((n, 2 * solver.N_rad + 1))
    rms_lat = np.zeros(n)
    rms_rad = np.zeros(n)

    for m in range(n):
        i_bwc = conv.machine_to_bwc_spoke(m, offset, n)
        a = np.zeros(n)
        a[i_bwc] = pitch_m                                   # one revolution of tightening
        dm = np.linalg.solve(K, A @ a)
        u = conv.lateral_bwc_to_system_mm(mm.rim_def_lat(th_lat_bwc, dm))
        v = conv.radial_bwc_to_system_mm(mm.rim_def_rad(th_rad_bwc, dm))
        dT = mm.spoke_tension_change(dm, a)                  # generator spoke order
        lateral[:, m] = u
        radial[:, m] = v
        for j in range(n):
            tension[j, m] = dT[conv.machine_to_bwc_spoke(j, offset, n)]
        coeffs_lat[m] = fit(theta_lat, u, solver.N_lat)
        coeffs_rad[m] = fit(theta_rad, v, solver.N_rad)
        from .fourier import fit_residual_rms
        rms_lat[m] = fit_residual_rms(theta_lat, u, coeffs_lat[m])
        rms_rad[m] = fit_residual_rms(theta_rad, v, coeffs_rad[m])

    alpha_a, alpha_b = bracing_angles(w)
    return RawInfluence(
        theta_fit_lat=theta_lat, theta_fit_rad=theta_rad, lateral_mm=lateral, radial_mm=radial,
        tension_n=tension, coeffs_lat=coeffs_lat, coeffs_rad=coeffs_rad, fit_rms_lat=rms_lat,
        fit_rms_rad=rms_rad, indexing_offset=offset, bracing_angle_a_rad=alpha_a,
        bracing_angle_b_rad=alpha_b, rr_modes=gen.rr_modes,
    )
