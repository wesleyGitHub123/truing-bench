"""Model-preparation parameters (SPEC 11.1, 11.2, 8.9, 8.10).

Every value that can change the artifact is a field here so that the
generating fingerprint (SPEC 8.7) can cover it. No field has a default that is
a physical claim: the caller supplies everything, and the fixture module
supplies explicitly synthetic values for tests.
"""
from __future__ import annotations

from dataclasses import asdict, dataclass, field
from enum import Enum
from typing import Any


class Side(str, Enum):
    A = "A"   # cassette / rotor side -> bike-wheel-calc drive side (DS)
    B = "B"   # opposite side -> non-drive side (NDS); positive lateral is toward B


class LeadTrail(str, Enum):
    LEADING = "leading"
    TRAILING = "trailing"


class BlockSource(str, Enum):
    LITERATURE_SEEDED = "literature_seeded"
    ANALYTICAL = "analytical"
    COARSE_MEASURED = "coarse_measured"
    FULL_MEASURED = "full_measured"


@dataclass(frozen=True)
class RimSection:
    """Rim cross-section properties in SI. For Stage 1 these are ESTIMATES
    (SPEC 8.10.1); their provenance must be stated by the caller."""

    area_m2: float
    I_rad_m4: float
    I_lat_m4: float
    J_tor_m4: float
    I_warp_m6: float
    young_mod_pa: float
    shear_mod_pa: float
    provenance: str


@dataclass(frozen=True)
class HubSide:
    flange_diameter_mm: float
    flange_offset_mm: float      # hub centre-plane to flange centre-plane


@dataclass(frozen=True)
class WheelClassParams:
    """Mirror of the firmware wheel-class configuration (SPEC 11.1) plus the two
    generation facts the analytical path needs: the rim section and the nipple
    thread pitch (metres of spoke shortening per nipple revolution)."""

    n_spokes: int
    n_cross: int
    rim_diameter_mm: float
    spoke_diameter_mm: float
    spoke_material_modulus_pa: float
    asymmetric: bool
    hub_side_a: HubSide
    hub_side_b: HubSide
    c_side_a_n_per_rev: float
    c_side_b_n_per_rev: float
    indexing_origin_side: Side
    indexing_origin_lead_trail: LeadTrail
    symmetry_angle_tolerance_rad: float
    nipple_thread_pitch_mm: float
    rim_section: RimSection


@dataclass(frozen=True)
class SolverParams:
    """The artifact-bound subset of SPEC 11.2 that shapes Phi-tilde and the
    pseudoinverses, plus the acceptance / identification thresholds."""

    tol_lateral_mm: float
    tol_radial_mm: float
    tol_tension_n: float
    tol_tension_cv: float
    tol_mean_tension_error_n: float
    tol_angular_rad: float
    max_condition_number: float
    rank_tolerance: float
    n_mt_displacement_tolerance: float
    n_mt_tension_tolerance: float
    n_mt_uniqueness_threshold: float
    trust_radial: float
    trust_tension: float
    N_lat: int
    N_rad: int
    n_rim_angles: int
    target_tension_n: float      # operating mean tension the wheel is linearised about


@dataclass(frozen=True)
class GeneratorParams:
    """Analytical-generation settings (recorded, fingerprinted)."""

    rr_modes: int                 # Rayleigh-Ritz mode count of the bike-wheel-calc solution
    n_fit_samples_lat: int        # SPEC 8.9: >= 2*N_lat + 1
    n_fit_samples_rad: int        # SPEC 8.9: >= 2*N_rad + 1
    smeared_spokes: bool = False  # discrete spokes; True is the Smith-Pippard approximation
    include_tension_stiffness: bool = True


def to_jsonable(obj: Any) -> Any:
    """Dataclasses / enums / numpy scalars -> plain JSON types (for fingerprinting and storage)."""
    if hasattr(obj, "__dataclass_fields__"):
        return {k: to_jsonable(v) for k, v in asdict(obj).items()}
    if isinstance(obj, Enum):
        return obj.value
    if isinstance(obj, dict):
        return {str(k): to_jsonable(v) for k, v in obj.items()}
    if isinstance(obj, (list, tuple)):
        return [to_jsonable(v) for v in obj]
    if hasattr(obj, "item"):   # numpy scalar
        return obj.item()
    return obj
