"""
Regression tests for server/iron_dome.py — safety filter and value translation.

Stubs out heavy db dependencies so tests run without database files or API keys.
"""

import sys
import os
import types

# ---------------------------------------------------------------------------
# Stub the db module with minimal data structures needed by iron_dome
# ---------------------------------------------------------------------------
_fake_catalog = types.ModuleType("server.plugin_catalog")

# Minimal highres data for a test plugin (UID=100)
_fake_catalog.highres_by_uid = {
    100: {
        "name": "TestPlugin Stereo",
        "uid": 100,
        "parameters": [
            {"index": 0, "name": "Master Bypass", "type": "discrete",
             "options": {"Off (ACTIVE)": 0.0, "On (Bypass)": 1.0}},
            {"index": 1, "name": "Preset Select", "type": "discrete",
             "options": {"Default": 0.0, "Warm": 0.5, "Bright": 1.0}},
            {"index": 2, "name": "Gain", "type": "continuous",
             "map": [-24.0, -12.0, 0.0, 12.0, 24.0], "unit": "dB"},
            {"index": 3, "name": "Frequency", "type": "continuous",
             "map": [20.0, 200.0, 2000.0, 20000.0], "unit": "Hz"},
            {"index": 4, "name": "Mode", "type": "discrete",
             "options": {"Clean": 0.0, "Warm": 0.333, "Hot": 0.667, "Melt": 1.0}},
        ],
    }
}

# Build highres_params from the fake plugin
_fake_catalog.highres_params = {}
for _p in _fake_catalog.highres_by_uid[100]["parameters"]:
    _fake_catalog.highres_params[(100, _p["index"])] = _p

_fake_catalog.plugins_by_uid = {
    100: {"name": "TestPlugin Stereo", "uid": 100, "parameters": []},
    200: {"name": "UnknownPlugin", "uid": 200, "parameters": []},
}

sys.modules["server.plugin_catalog"] = _fake_catalog
# Also stub server.db for backward compat (chain_diff still uses it via shim in some paths)
sys.modules.setdefault("server.db", _fake_catalog)

# test_chat_pipeline stubs server.iron_dome during collection. Remove that fake
# module here so this test imports the real implementation with our fake catalog.
if "server.iron_dome" in sys.modules:
    del sys.modules["server.iron_dome"]

_project_root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if _project_root not in sys.path:
    sys.path.insert(0, _project_root)

from server.iron_dome import (
    translate_to_vst_float, iron_dome_filter,
    anti_bloat_uids, anti_bloat_filter,
    MAX_PLUGINS_IN_CHAIN,
)


# ===========================================================================
# translate_to_vst_float
# ===========================================================================

class TestTranslateToVstFloat:

    def test_continuous_midpoint(self):
        """Gain=0dB should map to 0.5 (midpoint of [-24, -12, 0, 12, 24])."""
        result = translate_to_vst_float(100, 2, 0.0)
        assert result is not None
        assert abs(result - 0.5) < 0.01

    def test_continuous_min(self):
        """Gain=-24dB should map to 0.0."""
        result = translate_to_vst_float(100, 2, -24.0)
        assert result is not None
        assert abs(result - 0.0) < 0.01

    def test_continuous_max(self):
        """Gain=24dB should map to 1.0."""
        result = translate_to_vst_float(100, 2, 24.0)
        assert result is not None
        assert abs(result - 1.0) < 0.01

    def test_continuous_below_min(self):
        """Below range should clamp to min position."""
        result = translate_to_vst_float(100, 2, -100.0)
        assert result is not None
        assert abs(result - 0.0) < 0.01

    def test_continuous_above_max(self):
        """Above range should clamp to max position."""
        result = translate_to_vst_float(100, 2, 100.0)
        assert result is not None
        assert abs(result - 1.0) < 0.01

    def test_continuous_interpolation(self):
        """Gain=6dB should interpolate between 0 and 12 dB."""
        result = translate_to_vst_float(100, 2, 6.0)
        assert result is not None
        assert 0.5 < result < 0.75

    def test_discrete_float(self):
        """Discrete param with float value → clamp to [0, 1]."""
        result = translate_to_vst_float(100, 4, 0.333)
        assert result is not None
        assert abs(result - 0.333) < 0.01

    def test_discrete_string_match(self):
        """Discrete param by label name."""
        result = translate_to_vst_float(100, 4, "Warm")
        assert result is not None
        assert abs(result - 0.333) < 0.01

    def test_discrete_string_partial_match(self):
        """Discrete param with partial case-insensitive match."""
        result = translate_to_vst_float(100, 4, "warm")
        assert result is not None
        assert abs(result - 0.333) < 0.01

    def test_unknown_uid(self):
        """Unknown UID → None."""
        result = translate_to_vst_float(999, 2, 0.0)
        assert result is None

    def test_unknown_param_index(self):
        """Unknown param index → None."""
        result = translate_to_vst_float(100, 99, 0.0)
        assert result is None


# ===========================================================================
# iron_dome_filter
# ===========================================================================

class TestIronDomeFilter:

    def test_blocks_index_zero(self):
        """Master bypass (index 0) should be stripped."""
        preset = {"plugins": [{"uid": 100, "params": [
            {"index": 0, "value": 1.0},
            {"index": 2, "value": 0.0},
        ]}]}
        result = iron_dome_filter(preset)
        params = result["plugins"][0]["params"]
        assert all(p["index"] != 0 for p in params)
        assert len(params) == 1

    def test_blocks_negative_index(self):
        preset = {"plugins": [{"uid": 100, "params": [
            {"index": -1, "value": 0.5},
            {"index": 2, "value": 0.0},
        ]}]}
        result = iron_dome_filter(preset)
        assert len(result["plugins"][0]["params"]) == 1

    def test_translates_physical_values(self):
        """Physical target value should be translated to normalized float."""
        preset = {"plugins": [{"uid": 100, "params": [
            {"index": 2, "value": 12.0},  # Gain = 12dB → should be ~0.75
        ]}]}
        result = iron_dome_filter(preset)
        val = result["plugins"][0]["params"][0]["value"]
        assert 0.7 < val < 0.8

    def test_clamps_unknown_to_01(self):
        """Unknown UID param → clamp raw float to [0, 1]."""
        preset = {"plugins": [{"uid": 200, "params": [
            {"index": 5, "value": 1.5},
        ]}]}
        result = iron_dome_filter(preset)
        assert result["plugins"][0]["params"][0]["value"] == 1.0

    def test_preset_selector_forced_zero(self):
        """Preset selector parameters should be forced to 0.0."""
        preset = {"plugins": [{"uid": 100, "params": [
            {"index": 1, "value": 0.5},  # "Preset Select" → force 0.0
        ]}]}
        result = iron_dome_filter(preset)
        assert result["plugins"][0]["params"][0]["value"] == 0.0

    def test_empty_preset(self):
        assert iron_dome_filter({}) == {}
        assert iron_dome_filter({"plugins": []}) == {"plugins": []}

    def test_no_params_key(self):
        preset = {"plugins": [{"uid": 100}]}
        result = iron_dome_filter(preset)
        assert result == preset


# ===========================================================================
# anti_bloat_uids
# ===========================================================================

class TestAntiBloatUids:

    def test_no_change(self):
        assert anti_bloat_uids([1, 2, 3]) == [1, 2, 3]

    def test_consecutive_duplicates_removed(self):
        assert anti_bloat_uids([1, 1, 2]) == [1, 2]

    def test_non_consecutive_duplicates_kept(self):
        """Deliberate dual-use (e.g., pre/post EQ) is valid."""
        assert anti_bloat_uids([1, 2, 1]) == [1, 2, 1]

    def test_max_chain_size(self):
        uids = list(range(1, 25))  # 24 plugins
        result = anti_bloat_uids(uids)
        assert len(result) == MAX_PLUGINS_IN_CHAIN

    def test_empty(self):
        assert anti_bloat_uids([]) == []


# ===========================================================================
# anti_bloat_filter (preset-level)
# ===========================================================================

class TestAntiBloatFilter:

    def test_removes_consecutive_dupes(self):
        preset = {"plugins": [
            {"uid": 1, "params": []},
            {"uid": 1, "params": []},
            {"uid": 2, "params": []},
        ]}
        result = anti_bloat_filter(preset)
        assert len(result["plugins"]) == 2

    def test_enforces_max_size(self):
        preset = {"plugins": [{"uid": i, "params": []} for i in range(20)]}
        result = anti_bloat_filter(preset)
        assert len(result["plugins"]) == MAX_PLUGINS_IN_CHAIN

    def test_empty(self):
        assert anti_bloat_filter({"plugins": []}) == {"plugins": []}
        assert anti_bloat_filter({}) == {}
