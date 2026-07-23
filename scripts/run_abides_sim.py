#!/usr/bin/env python3
"""Run an ABIDES-JPMC reference market simulation."""

from __future__ import annotations

import argparse
import importlib
import json
import re
import sys
import types
from pathlib import Path
from typing import Any

import numpy as np


ROOT = Path(__file__).resolve().parents[1]
ABIDES_ROOT = ROOT / "components" / "abides"
LOCAL_PACKAGE_PATHS = (
    ROOT,
    ABIDES_ROOT / "abides-core",
    ABIDES_ROOT / "abides-markets",
    ABIDES_ROOT / "extensions" / "market_data_emitter" / "src",
    ROOT / "packages" / "market-data-protocol" / "src",
)

for package_path in reversed(LOCAL_PACKAGE_PATHS):
    sys.path.insert(0, str(package_path))


class _GeneralMixtureModel:
    """Compatibility implementation of the legacy pomegranate API ABIDES uses."""

    def __init__(self, spec: dict[str, Any]) -> None:
        self.spec = spec
        self.weights = np.asarray(spec["weights"], dtype=float)
        self.weights /= self.weights.sum()

    @classmethod
    def from_json(cls, payload: str) -> "_GeneralMixtureModel":
        return cls(json.loads(payload))

    def sample(self, random_state: np.random.RandomState | None = None) -> float:
        rng = random_state if random_state is not None else np.random
        index = int(rng.choice(len(self.weights), p=self.weights))
        distribution = self.spec["distributions"][index]
        mean, deviation = distribution["parameters"]

        if distribution["name"] == "LogNormalDistribution":
            return float(rng.lognormal(mean, deviation))
        if distribution["name"] == "NormalDistribution":
            return float(rng.normal(mean, deviation))
        raise ValueError(
            f"Unsupported ABIDES order-size distribution: {distribution['name']}"
        )


def _install_pomegranate_compatibility() -> None:
    try:
        from pomegranate import GeneralMixtureModel  # noqa: F401
    except (ImportError, AttributeError):
        compatibility_module = types.ModuleType("pomegranate")
        compatibility_module.GeneralMixtureModel = _GeneralMixtureModel
        sys.modules["pomegranate"] = compatibility_module


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run an ABIDES-JPMC reference market simulation."
    )
    parser.add_argument("--scenario", default="rmsc04")
    parser.add_argument("--seed", type=int, default=0)
    parser.add_argument("--end-time", default="10:00:00")
    args = parser.parse_args()

    if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", args.scenario):
        parser.error("scenario must be a Python module name such as rmsc04")
    return args


def main() -> None:
    args = _parse_args()
    _install_pomegranate_compatibility()

    from abides_core import abides

    config_module = importlib.import_module(
        f"abides_markets.configs.{args.scenario}"
    )
    config = config_module.build_config(seed=args.seed, end_time=args.end_time)

    print(
        f"Launching {args.scenario.upper()}: seed={args.seed}, "
        f"agents={len(config['agents'])}, end_time={args.end_time}",
        flush=True,
    )
    abides.run(config)
    print("ABIDES_SIM_COMPLETE", flush=True)


if __name__ == "__main__":
    main()
