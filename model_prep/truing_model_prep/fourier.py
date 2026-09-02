"""Fourier representation of influence functions (SPEC 8.9).

    y(theta) = a0 + sum_{n=1..N} (a_n cos(n theta) + b_n sin(n theta))

Coefficient layout: [a0, a1, b1, a2, b2, ..., aN, bN]  (2N+1 values).
"""
from __future__ import annotations

import numpy as np


class SamplingFloorError(ValueError):
    """SPEC 8.9: fitting below 2N+1 samples aliases silently; refuse instead."""


def design_matrix(theta: np.ndarray, order: int) -> np.ndarray:
    theta = np.asarray(theta, dtype=float)
    cols = [np.ones_like(theta)]
    for n in range(1, order + 1):
        cols.append(np.cos(n * theta))
        cols.append(np.sin(n * theta))
    return np.column_stack(cols)


def fit(theta: np.ndarray, y: np.ndarray, order: int) -> np.ndarray:
    """Least-squares Fourier fit. Raises SamplingFloorError below the 2N+1 floor."""
    theta = np.asarray(theta, dtype=float)
    floor = 2 * order + 1
    if theta.size < floor:
        raise SamplingFloorError(f"{theta.size} fit samples < floor 2*{order}+1 = {floor}")
    A = design_matrix(theta, order)
    coeffs, *_ = np.linalg.lstsq(A, np.asarray(y, dtype=float), rcond=None)
    return coeffs


def expand(coeffs: np.ndarray, theta: np.ndarray) -> np.ndarray:
    coeffs = np.asarray(coeffs, dtype=float)
    order = (coeffs.size - 1) // 2
    return design_matrix(theta, order) @ coeffs


def fit_residual_rms(theta: np.ndarray, y: np.ndarray, coeffs: np.ndarray) -> float:
    r = np.asarray(y, dtype=float) - expand(coeffs, theta)
    return float(np.sqrt(np.mean(r * r)))
