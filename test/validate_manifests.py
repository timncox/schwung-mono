#!/usr/bin/env python3
"""Guard Mono Voice's complete self-describing eight-knob hierarchy."""

import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "modules/sound_generators/mono-voice/module.json"

data = json.loads(MANIFEST.read_text())
assert data["component_type"] == "sound_generator"
levels = data["capabilities"]["ui_hierarchy"]["levels"]
assert len(levels) == 15, f"expected root + 14 parameter pages, got {len(levels)}"

all_keys = set()
for level_name, level in levels.items():
    knobs = level.get("knobs", [])
    expected_count = 1 if level_name == "root" else 8
    assert len(knobs) == expected_count, (
        f"{level_name}: expected {expected_count} knobs, got {len(knobs)}"
    )

    definitions = {
        param["key"]
        for param in level.get("params", [])
        if isinstance(param, dict) and "key" in param
    }
    assert set(knobs) == definitions, (
        f"{level_name}: knob/parameter drift: {sorted(set(knobs) ^ definitions)}"
    )
    assert not (all_keys & definitions), (
        f"{level_name}: duplicate parameter keys {sorted(all_keys & definitions)}"
    )
    all_keys.update(definitions)

    for param in level.get("params", []):
        if isinstance(param, dict) and "level" in param:
            assert param["level"] in levels, (
                f"{level_name}: missing navigation target {param['level']}"
            )

assert len(all_keys) == 113, f"expected 113 reachable controls, got {len(all_keys)}"
print("manifest UI validation passed")
