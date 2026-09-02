"""SYNTHETIC fixture parameters for tests and the reference artifact.

These describe NO real wheel. The rim section is the example section shipped
with bike-wheel-calc's own examples (an aluminium rim of plausible stiffness),
labelled as such; the spoke, hub and tolerance values mirror the firmware
fixtures in lib/truing_fixtures so host and firmware tests talk about the same
synthetic wheel. Reference-work figures (c = 473 N/rev, N_lat = 6, N_rad = 13,
the reference tolerances) are fixture content only (SPEC P5).
"""
from __future__ import annotations

from .params import GeneratorParams, HubSide, LeadTrail, RimSection, Side, SolverParams, WheelClassParams

BWC_EXAMPLE_RIM = RimSection(
    area_m2=100e-6,
    I_rad_m4=100.0 / 69e9,
    I_lat_m4=200.0 / 69e9,
    J_tor_m4=25.0 / 26e9,
    I_warp_m6=0.0,
    young_mod_pa=69e9,
    shear_mod_pa=26e9,
    provenance="bike-wheel-calc examples/example_flange_height.py section (SYNTHETIC fixture, not measured)",
)


def wheel_sym32() -> WheelClassParams:
    return WheelClassParams(
        n_spokes=32, n_cross=3, rim_diameter_mm=622.0, spoke_diameter_mm=2.0, spoke_material_modulus_pa=200e9,
        asymmetric=False, hub_side_a=HubSide(45.0, 35.0), hub_side_b=HubSide(45.0, 35.0),
        c_side_a_n_per_rev=473.0, c_side_b_n_per_rev=473.0,
        indexing_origin_side=Side.B, indexing_origin_lead_trail=LeadTrail.LEADING,
        symmetry_angle_tolerance_rad=0.01, nipple_thread_pitch_mm=0.45, rim_section=BWC_EXAMPLE_RIM,
    )


def wheel_asym36() -> WheelClassParams:
    return WheelClassParams(
        n_spokes=36, n_cross=3, rim_diameter_mm=622.0, spoke_diameter_mm=2.0, spoke_material_modulus_pa=200e9,
        asymmetric=True, hub_side_a=HubSide(45.0, 20.0), hub_side_b=HubSide(45.0, 35.0),
        c_side_a_n_per_rev=450.0, c_side_b_n_per_rev=500.0,
        indexing_origin_side=Side.B, indexing_origin_lead_trail=LeadTrail.LEADING,
        symmetry_angle_tolerance_rad=0.01, nipple_thread_pitch_mm=0.45, rim_section=BWC_EXAMPLE_RIM,
    )


def solver_params(n_rim_angles: int) -> SolverParams:
    return SolverParams(
        tol_lateral_mm=0.1, tol_radial_mm=0.05, tol_tension_n=100.0, tol_tension_cv=0.10,
        tol_mean_tension_error_n=100.0, tol_angular_rad=2.0 * 3.141592653589793 / n_rim_angles,
        max_condition_number=1e4, rank_tolerance=1e-6, n_mt_displacement_tolerance=1e-6,
        n_mt_tension_tolerance=1e-3, n_mt_uniqueness_threshold=0.1, trust_radial=0.5, trust_tension=1e-5,
        N_lat=6, N_rad=13, n_rim_angles=n_rim_angles, target_tension_n=1000.0,
    )


def generator_params() -> GeneratorParams:
    return GeneratorParams(rr_modes=36, n_fit_samples_lat=256, n_fit_samples_rad=256)
