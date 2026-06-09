"""
Regression tests for server/chain_diff.py — slot-diff and merge behaviour.

These tests do NOT require a running server, Gemini keys, or any database.
They import only pure functions with a single lightweight dependency (db.uid_to_name).
"""

import sys
import os
import types

# ---------------------------------------------------------------------------
# Stub out heavy dependencies so we can import chain_diff in isolation.
# chain_diff.py only calls plugin_catalog.uid_to_name (for logging), so we
# provide a lightweight fake instead of importing config/database modules.
# ---------------------------------------------------------------------------
_fake_catalog = types.ModuleType("server.plugin_catalog")
_fake_catalog.uid_to_name = lambda uid: f"Plugin-{uid}"
sys.modules["server.plugin_catalog"] = _fake_catalog

_fake_db = types.ModuleType("server.db")
_fake_db.uid_to_name = lambda uid: f"Plugin-{uid}"
sys.modules.setdefault("server.db", _fake_db)

# Ensure the project root is on sys.path so `server` package resolves
_project_root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if _project_root not in sys.path:
    sys.path.insert(0, _project_root)

from server.chain_diff import compute_chain_diff, build_merged_preset, _make_entry


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
def _entry(uid, slot_id=0, user_touched=False, bypassed=False, user_owned=False):
    return {
        "uid": uid, "slot_id": slot_id,
        "user_touched": user_touched, "bypassed": bypassed, "user_owned": user_owned,
    }


# ===========================================================================
# compute_chain_diff
# ===========================================================================

class TestIdentity:
    """Same UIDs in same order — should be identity."""

    def test_identity_no_bypass(self):
        current = [_entry(1, slot_id=10), _entry(2, slot_id=20)]
        diff = compute_chain_diff(current, [1, 2])
        assert diff["is_identity"] is True
        assert diff["is_reorder"] is False
        assert diff["new_uids"] == []
        assert diff["removed_uids"] == []
        assert diff["final_uids"] == [1, 2]
        assert all(e["action"] == "keep" for e in diff["final_entries"])

    def test_identity_with_bypass_change(self):
        current = [_entry(1, slot_id=10), _entry(2, slot_id=20)]
        diff = compute_chain_diff(current, [1, 2], desired_bypassed_positions={1})
        assert diff["is_identity"] is False  # bypass change makes it non-identity
        assert diff["has_bypass_changes"] is True
        assert diff["final_entries"][0]["action"] == "keep"
        assert diff["final_entries"][1]["action"] == "bypass"
        assert diff["final_entries"][1]["bypassed"] is True

    def test_identity_enable_previously_bypassed(self):
        current = [_entry(1, slot_id=10, bypassed=True), _entry(2, slot_id=20)]
        diff = compute_chain_diff(current, [1, 2], desired_bypassed_positions=set())
        assert diff["has_bypass_changes"] is True
        assert diff["final_entries"][0]["action"] == "enable"
        assert diff["final_entries"][0]["bypassed"] is False

    def test_identity_preserve_bypass_when_none(self):
        """desired_bypassed_positions=None means 'no change'."""
        current = [_entry(1, slot_id=10, bypassed=True), _entry(2, slot_id=20)]
        diff = compute_chain_diff(current, [1, 2], desired_bypassed_positions=None)
        assert diff["is_identity"] is True
        assert diff["has_bypass_changes"] is False
        # Original bypass state preserved
        assert diff["final_entries"][0]["bypassed"] is True


class TestNewAndRemoved:
    """Adding/removing plugins."""

    def test_add_one(self):
        current = [_entry(1, slot_id=10)]
        diff = compute_chain_diff(current, [1, 3])
        assert diff["new_uids"] == [3]
        assert diff["retained_uids"] == [1]
        assert diff["removed_uids"] == []
        assert diff["final_uids"] == [1, 3]
        # New plugin gets slot_id=0
        assert diff["final_entries"][1]["slot_id"] == 0
        assert diff["final_entries"][1]["action"] == "insert"

    def test_remove_one(self):
        current = [_entry(1, slot_id=10), _entry(2, slot_id=20)]
        diff = compute_chain_diff(current, [1])
        assert diff["removed_uids"] == [2]
        assert diff["final_uids"] == [1]

    def test_swap_plugin(self):
        current = [_entry(1, slot_id=10), _entry(2, slot_id=20)]
        diff = compute_chain_diff(current, [1, 3])
        assert diff["new_uids"] == [3]
        assert diff["removed_uids"] == [2]
        assert diff["retained_uids"] == [1]

    def test_empty_desired(self):
        current = [_entry(1, slot_id=10)]
        diff = compute_chain_diff(current, [])
        assert diff["final_uids"] == []
        assert diff["removed_uids"] == [1]

    def test_empty_current(self):
        diff = compute_chain_diff([], [1, 2])
        assert diff["new_uids"] == [1, 2]
        assert diff["final_uids"] == [1, 2]


class TestReorder:
    """Same plugins, different order."""

    def test_simple_reorder(self):
        current = [_entry(1, slot_id=10), _entry(2, slot_id=20)]
        diff = compute_chain_diff(current, [2, 1])
        assert diff["is_reorder"] is True
        assert diff["new_uids"] == []
        assert diff["removed_uids"] == []
        assert diff["final_uids"] == [2, 1]


class TestDuplicateUIDs:
    """Chain with the same plugin UID appearing multiple times."""

    def test_duplicate_uid_retained(self):
        current = [_entry(1, slot_id=10), _entry(1, slot_id=20)]
        diff = compute_chain_diff(current, [1, 1])
        assert diff["is_identity"] is True
        assert len(diff["final_entries"]) == 2
        # Both slot_ids preserved via greedy matching
        assert diff["final_entries"][0]["slot_id"] == 10
        assert diff["final_entries"][1]["slot_id"] == 20

    def test_duplicate_uid_add_third(self):
        current = [_entry(1, slot_id=10), _entry(1, slot_id=20)]
        diff = compute_chain_diff(current, [1, 1, 1])
        assert len(diff["new_uids"]) == 1
        assert diff["new_uids"] == [1]
        assert diff["final_entries"][2]["slot_id"] == 0  # new


class TestIntentPropagation:
    """User intent flags survive diff."""

    def test_user_touched_preserved(self):
        current = [_entry(1, slot_id=10, user_touched=True)]
        diff = compute_chain_diff(current, [1])
        assert diff["final_entries"][0]["user_touched"] is True

    def test_user_owned_preserved(self):
        current = [_entry(1, slot_id=10, user_owned=True)]
        diff = compute_chain_diff(current, [1])
        assert diff["final_entries"][0]["user_owned"] is True

    def test_new_plugin_no_intent(self):
        diff = compute_chain_diff([], [5])
        e = diff["final_entries"][0]
        assert e["user_touched"] is False
        assert e["user_owned"] is False
        assert e["bypassed"] is False


# ===========================================================================
# build_merged_preset
# ===========================================================================

class TestBuildMergedPreset:

    def test_basic_merge(self):
        entries = [
            _make_entry(1, slot_id=10, action="keep"),
            _make_entry(2, slot_id=0, action="insert"),
        ]
        s2_positions = {1}
        s2_params = {1: [{"index": 1, "value": 0.5}]}
        preset = build_merged_preset(entries, s2_positions, s2_params)
        plugins = preset["plugins"]
        assert len(plugins) == 2
        assert plugins[0]["params"] == []
        assert plugins[0]["action"] == "keep"
        assert plugins[1]["params"] == [{"index": 1, "value": 0.5}]
        assert plugins[1]["action"] == "insert"

    def test_reconfigure_action(self):
        """Retained plugin getting S2 params → action='reconfigure'."""
        entries = [_make_entry(1, slot_id=10, action="keep")]
        s2_positions = {0}
        s2_params = {0: [{"index": 2, "value": 0.8}]}
        preset = build_merged_preset(entries, s2_positions, s2_params)
        assert preset["plugins"][0]["action"] == "reconfigure"

    def test_bypass_action_preserved(self):
        entries = [_make_entry(1, slot_id=10, action="bypass")]
        preset = build_merged_preset(entries, set(), {})
        assert preset["plugins"][0]["action"] == "bypass"

    def test_empty_chain(self):
        preset = build_merged_preset([], set(), {})
        assert preset["plugins"] == []

    def test_slot_id_in_output(self):
        entries = [_make_entry(1, slot_id=42, action="keep")]
        preset = build_merged_preset(entries, set(), {})
        assert preset["plugins"][0]["slot_id"] == 42
