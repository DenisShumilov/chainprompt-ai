"""
Regression tests for server/inventory_manifest.py — fail-closed UID validation.

The inventory gate is the trust boundary between what the host can instantiate
and what the AI is allowed to plan. These tests exercise the REAL module against
temp manifest files: missing manifest, corrupt manifest, valid manifest, FX
eligibility, and capability tiers. No server, Gemini key, or database required.
"""

import json
import os
import sys
import tempfile
import types

_project_root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if _project_root not in sys.path:
    sys.path.insert(0, _project_root)

# ---------------------------------------------------------------------------
# Stub config so the real inventory_manifest reads a manifest path we control.
# ---------------------------------------------------------------------------
_manifest_dir = tempfile.mkdtemp(prefix="chainprompt_test_")
_MANIFEST_PATH = os.path.join(_manifest_dir, "installed_plugins.json")

_fake_config = types.ModuleType("server.config")
_fake_config.INSTALLED_MANIFEST_PATH = _MANIFEST_PATH
_fake_config.MANIFEST_VERSION = 2
_fake_config.METADATA_STALE_HOURS = 720
_fake_config.METADATA_DB_PATHS = {}


class _FakeMetadataStatus:
    READY = "ready"
    NEEDS_EXTRACT = "needs_extract"
    STALE = "stale"
    FAILED = "failed"


_fake_config.MetadataStatus = _FakeMetadataStatus
sys.modules["server.config"] = _fake_config

# Earlier test modules stub server.inventory_manifest during collection.
# Remove any fake so this module imports the real implementation.
if "server.inventory_manifest" in sys.modules:
    del sys.modules["server.inventory_manifest"]

from server.inventory_manifest import (
    is_inventory_available, is_uid_installed, validate_uids,
    reload_installed_manifest, is_fx_eligible, get_plugin_capability,
    set_highres_reference,
)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
def _plugin(uid, name, is_instrument=False, num_out=2):
    return {
        "uid": uid, "name": name, "category": "Fx", "manufacturer": "Test",
        "format": "VST3", "pluginFile": f"{name}.vst3",
        "isInstrument": is_instrument,
        "numInputChannels": 2, "numOutputChannels": num_out,
        "version": "1.0",
    }


def _write_manifest(plugins):
    with open(_MANIFEST_PATH, "w", encoding="utf-8") as f:
        json.dump({"version": 2, "scanMode": "full_vst3",
                   "plugins": plugins, "failedFiles": []}, f)
    reload_installed_manifest()


def _remove_manifest():
    if os.path.isfile(_MANIFEST_PATH):
        os.remove(_MANIFEST_PATH)
    reload_installed_manifest()


# ===========================================================================
# Tests
# ===========================================================================

class TestFailClosedGate:
    """No manifest (or a corrupt one) must mean NO plugins are available."""

    def test_missing_manifest_fails_closed(self):
        _remove_manifest()

        assert not is_inventory_available()
        assert not is_uid_installed(100)
        # Every requested UID must come back as NOT installed.
        assert validate_uids([100, 200, 300]) == [100, 200, 300]
        assert get_plugin_capability(100) == "unavailable"
        assert not is_fx_eligible("AnyPlugin")

    def test_corrupt_manifest_fails_closed(self):
        with open(_MANIFEST_PATH, "w", encoding="utf-8") as f:
            f.write("this is not valid json {{{")
        reload_installed_manifest()

        assert not is_inventory_available()
        assert validate_uids([100]) == [100]


class TestUidValidation:
    """Hallucinated UIDs must be rejected; installed UIDs must pass."""

    def test_installed_uids_pass_unknown_uids_rejected(self):
        _write_manifest([_plugin(100, "TestComp Stereo"),
                         _plugin(200, "TestEQ Stereo")])

        assert is_inventory_available()
        assert is_uid_installed(100)
        assert not is_uid_installed(999)
        assert validate_uids([100, 200]) == []
        assert validate_uids([100, 999]) == [999]
        assert validate_uids([999, -123456]) == [999, -123456]

    def test_empty_manifest_loads_but_rejects_every_uid(self):
        _write_manifest([])

        # The gate opens (a manifest exists) but no UID can validate.
        assert is_inventory_available()
        assert validate_uids([100]) == [100]


class TestFxEligibility:
    """Instruments and surround plugins must be excluded from chain planning."""

    def test_instrument_and_surround_excluded(self):
        _write_manifest([
            _plugin(100, "TestComp Stereo"),
            _plugin(200, "TestSynth", is_instrument=True),
            _plugin(300, "TestSurround", num_out=6),
        ])

        assert is_fx_eligible("TestComp")
        assert not is_fx_eligible("TestSynth")
        assert not is_fx_eligible("TestSurround")


class TestCapabilityTiers:
    """configurable (has metadata) / hostable (installed only) / unavailable."""

    def test_tiers(self):
        _write_manifest([_plugin(100, "TestComp Stereo"),
                         _plugin(200, "TestEQ Stereo")])
        set_highres_reference({100: {"name": "TestComp Stereo", "parameters": []}})
        try:
            assert get_plugin_capability(100) == "configurable"
            assert get_plugin_capability(200) == "hostable"
            assert get_plugin_capability(999) == "unavailable"
        finally:
            set_highres_reference({})
