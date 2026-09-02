import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from truing_model_prep import artifact as art_mod  # noqa: E402
from truing_model_prep import fixtures  # noqa: E402
from truing_model_prep.generator import generate  # noqa: E402


@pytest.fixture(scope="session")
def sym32():
    wheel = fixtures.wheel_sym32()
    solver = fixtures.solver_params(wheel.n_spokes)
    gen = fixtures.generator_params()
    raw = generate(wheel, solver, gen)
    art = art_mod.assemble(raw, wheel, solver, gen, artifact_id="test_sym32")
    return wheel, solver, gen, raw, art


@pytest.fixture(scope="session")
def asym36():
    wheel = fixtures.wheel_asym36()
    solver = fixtures.solver_params(wheel.n_spokes)
    gen = fixtures.generator_params()
    raw = generate(wheel, solver, gen)
    art = art_mod.assemble(raw, wheel, solver, gen, artifact_id="test_asym36")
    return wheel, solver, gen, raw, art
