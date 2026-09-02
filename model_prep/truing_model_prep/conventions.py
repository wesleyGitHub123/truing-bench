"""Unit, sign and indexing conventions reconciled at the bike-wheel-calc boundary
(SPEC 6.4, 6.4.1, 6.5). This is the ONLY module that knows both conventions.

bike-wheel-calc (verified in its source at commit 6fc380c):
  * SI units (metres, newtons).
  * lateral u: positive toward the non-drive side (NDS)      -> same as ours (toward Side B): no flip
  * radial  v: positive INWARD toward the hub                -> ours is outward: FLIP
  * lace_cross(): spoke k sits at theta = 2*pi*k/n_spokes; spokes alternate NDS, DS, NDS, ...
    and leading, leading, trailing, trailing, ...; spoke 0 is NDS leading at theta = 0.
  * A positive adjustment `a` shortens the spoke (tension increases) -> positive = tightening: same.
"""
from __future__ import annotations

import numpy as np

from .params import LeadTrail, Side

MM_PER_M = 1000.0

# bike-wheel-calc spoke class by index modulo 4 (from lace_cross_nds / lace_cross_ds).
BWC_CLASS_BY_INDEX_MOD_4 = {
    0: (Side.B, LeadTrail.LEADING),
    1: (Side.A, LeadTrail.LEADING),
    2: (Side.B, LeadTrail.TRAILING),
    3: (Side.A, LeadTrail.TRAILING),
}


def indexing_offset(origin_side: Side, origin_lead_trail: LeadTrail) -> int:
    """bike-wheel-calc index of the spoke whose class matches the machine's declared spoke 0
    (SPEC 6.5). Machine spoke m corresponds to generator spoke (m + offset) mod n_spokes."""
    for k, cls in BWC_CLASS_BY_INDEX_MOD_4.items():
        if cls == (origin_side, origin_lead_trail):
            return k
    raise ValueError("unknown indexing origin class")


def machine_to_bwc_spoke(m: int, offset: int, n_spokes: int) -> int:
    return (m + offset) % n_spokes


def machine_angle_to_bwc(theta_machine: np.ndarray, offset: int, n_spokes: int) -> np.ndarray:
    """Machine rim angle (spoke 0 at 0) -> generator angle (generator spoke `offset` at machine 0)."""
    return np.asarray(theta_machine, dtype=float) + 2.0 * np.pi * offset / n_spokes


def lateral_bwc_to_system_mm(u_m: np.ndarray) -> np.ndarray:
    """NDS-positive metres -> Side-B-positive millimetres: no sign flip (SPEC 6.4)."""
    return np.asarray(u_m, dtype=float) * MM_PER_M


def radial_bwc_to_system_mm(v_m: np.ndarray) -> np.ndarray:
    """Inward-positive metres -> outward-positive millimetres: SIGN FLIP (SPEC 6.4)."""
    return -np.asarray(v_m, dtype=float) * MM_PER_M


def spoke_side(m: int, origin_side: Side) -> Side:
    """Spokes alternate sides starting from the declared origin (SPEC 6.5)."""
    if m % 2 == 0:
        return origin_side
    return Side.B if origin_side == Side.A else Side.A


def spoke_class(m: int, offset: int) -> tuple[Side, LeadTrail]:
    return BWC_CLASS_BY_INDEX_MOD_4[(m + offset) % 4]
