"""
Regression tests for server/chat_pipeline.py — early fail-closed gate.

These tests do NOT require a running server, Gemini keys, or any database.
They stub all heavy dependencies and test only the pipeline orchestration logic.
"""

import sys
import os
import types
from unittest import mock

# ---------------------------------------------------------------------------
# Stub out ALL heavy dependencies so we can import chat_pipeline in isolation.
# ---------------------------------------------------------------------------
_project_root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if _project_root not in sys.path:
    sys.path.insert(0, _project_root)

# Stub config (must be first — imported by many modules)
_fake_config = types.ModuleType("server.config")
_fake_config.DB_PATH = "/nonexistent/master.json"
_fake_config.HIGHRES_DB_PATH = "/nonexistent/highres.json"
_fake_config.META_GT_PATH = "/nonexistent/meta.json"
_fake_config.METADATA_DB_PATHS = {}
_fake_config.METADATA_STALE_HOURS = 720
_fake_config.INSTALLED_MANIFEST_PATH = "/nonexistent/manifest.json"
_fake_config.MANIFEST_VERSION = 2
_fake_config.GEMINI_API_KEYS = ["fake"]
_fake_config.GEMINI_MODEL = "fake-model"
_fake_config.UNIFIED_SCHEMA = {}
_fake_config.STAGE2_CHUNK_SIZE = 3

class _FakeMetadataStatus:
    READY = "ready"
    NEEDS_EXTRACT = "needs_extract"
    STALE = "stale"
    FAILED = "failed"
_fake_config.MetadataStatus = _FakeMetadataStatus
_fake_config.KNOWN_PROVIDERS = {}
sys.modules["server.config"] = _fake_config

# Stub dotenv
_fake_dotenv = types.ModuleType("dotenv")
_fake_dotenv.load_dotenv = lambda: None
sys.modules["dotenv"] = _fake_dotenv

# Stub inventory_manifest
_fake_inventory = types.ModuleType("server.inventory_manifest")
_inventory_available = False  # default: unavailable

def _is_inventory_available():
    return _inventory_available

_fake_inventory.is_inventory_available = _is_inventory_available
_fake_inventory.validate_uids = lambda uids: []
_fake_inventory.get_base_name = lambda n: n
_fake_inventory.is_uid_installed = lambda uid: True
_fake_inventory.is_base_name_installed = lambda bn: True
_fake_inventory.get_plugin_capability = lambda uid: "configurable"
_fake_inventory.get_base_name_capability = lambda bn, m: "configurable"
_fake_inventory.get_installed_uids = lambda: set()
_fake_inventory.get_all_plugin_info = lambda: {}
_fake_inventory.is_fx_eligible = lambda bn: True
_fake_inventory.set_highres_reference = lambda h: None
_fake_inventory.set_metadata_maps = lambda p, t: None
_fake_inventory.recompute_metadata_status = lambda: None
_fake_inventory.get_metadata_backlog = lambda: {"total_installed": 0, "ready": 0, "needs_extract": 0, "stale": 0, "failed": 0, "by_provider": {}, "provider_timestamps": {}}
_fake_inventory._manifest_loaded = False
_fake_inventory._installed_uids = set()
sys.modules["server.inventory_manifest"] = _fake_inventory

# Stub plugin_catalog
_fake_catalog = types.ModuleType("server.plugin_catalog")
_fake_catalog.uid_to_name = lambda uid: f"Plugin-{uid}"
_fake_catalog.uids_to_names = lambda uids: [f"Plugin-{u}" for u in uids]
_fake_catalog.enforce_plugin_format_list = lambda uids, s: uids
_fake_catalog.resolve_name_to_uid = lambda n, s: None
_fake_catalog.names_to_uids = lambda names, s: []
_fake_catalog.get_cached_catalog = lambda s: ""
_fake_catalog.plugins_by_uid = {}
_fake_catalog.plugins_list = []
_fake_catalog.META_GROUNDTRUTH = []
_fake_catalog._total_catalog = 0
_fake_catalog._described_count = 0
_fake_catalog.preset_retriever = None
_fake_catalog._rebuild_cached_catalogs = lambda: None
_fake_catalog.reload_metadata_envelopes = lambda: None
_fake_catalog.highres_by_uid = {}
_fake_catalog.uid_to_base_name = {}
_fake_catalog.PLUGIN_DESCRIPTIONS = {}
_fake_catalog.get_uid_metadata = lambda uid: None
sys.modules["server.plugin_catalog"] = _fake_catalog

# Stub db (backward compat shim)
_fake_db = types.ModuleType("server.db")
_fake_db.uid_to_name = _fake_catalog.uid_to_name
sys.modules["server.db"] = _fake_db

# Stub audio_context
_fake_audio_ctx = types.ModuleType("server.audio_context")
_fake_audio_ctx.build_audio_context = lambda m: ""
sys.modules["server.audio_context"] = _fake_audio_ctx

# Stub unified_agent
_unified_agent_called = False
def _fake_unified_agent(*args, **kwargs):
    global _unified_agent_called
    _unified_agent_called = True
    return {
        "route": "chat",
        "message": "test",
        "routing": {"main_uids": [], "main_names": []},
        "configure_uids": [],
        "configure_names": [],
        "desired_uids": [],
        "desired_names": [],
        "bypassed_uids": [],
        "bypassed_names": [],
        "_bypass_explicit": False,
    }

_fake_unified = types.ModuleType("server.unified_agent")
_fake_unified.unified_agent = _fake_unified_agent
_fake_unified.rebuild_cached_prompts = lambda: None
sys.modules["server.unified_agent"] = _fake_unified

# Stub chain_diff
_fake_chain_diff = types.ModuleType("server.chain_diff")
_fake_chain_diff.compute_chain_diff = lambda *a, **kw: {"is_identity": True, "is_reorder": False, "final_uids": [], "final_entries": [], "new_uids": [], "retained_uids": [], "removed_uids": []}
_fake_chain_diff.build_merged_preset = lambda *a, **kw: {"plugins": []}
sys.modules["server.chain_diff"] = _fake_chain_diff

# Stub stage2_engineer
_fake_stage2 = types.ModuleType("server.stage2_engineer")
_fake_stage2.stage2_engineer = lambda *a, **kw: {"preset": {"plugins": []}}
sys.modules["server.stage2_engineer"] = _fake_stage2

# Stub stage2_context
_fake_stage2_ctx = types.ModuleType("server.stage2_context")
_fake_stage2_ctx.rebuild_param_cache = lambda: None
sys.modules["server.stage2_context"] = _fake_stage2_ctx

# Stub iron_dome
_fake_iron = types.ModuleType("server.iron_dome")
_fake_iron.anti_bloat_uids = lambda uids: uids
sys.modules["server.iron_dome"] = _fake_iron

# Stub studio_memory
_fake_memory = types.ModuleType("server.studio_memory")
_fake_memory.upsert_track = lambda *a, **kw: None
_fake_memory.log_chat = lambda *a, **kw: None
_fake_memory.get_chat_history = lambda *a, **kw: []
_fake_memory.clear_chat_history = lambda *a, **kw: None
_fake_memory.upsert_mix_state = lambda *a, **kw: None
_fake_memory.find_track_by_name = lambda *a, **kw: None
_fake_memory.get_all_tracks = lambda *a, **kw: []
_fake_memory.extract_and_store_preferences = lambda *a, **kw: None
_fake_memory.get_user_preferences = lambda *a, **kw: []
_fake_memory.normalize_chain = lambda raw: []
_fake_memory.chain_uids = lambda entries: []
_fake_memory.CROSS_TRACK_TTL_MINUTES = 10
_fake_memory.init_db = lambda: None
sys.modules["server.studio_memory"] = _fake_memory

# Stub studio_db shim
sys.modules["server.studio_db"] = _fake_memory

# Stub gemini_client
_fake_gemini = types.ModuleType("server.gemini_client")
_fake_gemini.call_llm = lambda *a, **kw: '{"route":"chat","message":"test","routing":{"main":[]},"plugins_to_configure":[],"audio_analysis":""}'
sys.modules["server.gemini_client"] = _fake_gemini

# Stub google.genai
_fake_genai_types = types.ModuleType("google.genai.types")
class _FakePart:
    @staticmethod
    def from_bytes(**kw):
        return None
_fake_genai_types.Part = _FakePart
sys.modules["google"] = types.ModuleType("google")
sys.modules["google.genai"] = types.ModuleType("google.genai")
sys.modules["google.genai.types"] = _fake_genai_types

# NOW we can import the pipeline
from server.chat_pipeline import run_chat_pipeline


# ===========================================================================
# Tests
# ===========================================================================

class TestEarlyFailClosedGate:
    """Inventory must be available BEFORE any LLM tokens are spent."""

    def _make_request(self, prompt="test build request"):
        return {
            "prompt": prompt,
            "instance_id": "test-001",
            "track_name": "Test Track",
        }

    def test_blocks_when_inventory_unavailable(self):
        """Pipeline returns error immediately when no manifest — no LLM call."""
        global _inventory_available, _unified_agent_called
        _inventory_available = False
        _unified_agent_called = False

        result = run_chat_pipeline(self._make_request())

        assert "⚠️" in result["reply"]
        assert "inventory" in result["reply"].lower() or "scan" in result["reply"].lower()
        assert not _unified_agent_called, \
            "unified_agent should NOT be called when inventory is unavailable"
        assert "preset" not in result, \
            "No preset should be returned when inventory is unavailable"

    def test_proceeds_when_inventory_available(self):
        """Pipeline calls unified_agent when manifest is loaded."""
        global _inventory_available, _unified_agent_called
        _inventory_available = True
        _unified_agent_called = False

        result = run_chat_pipeline(self._make_request())

        assert _unified_agent_called, \
            "unified_agent SHOULD be called when inventory is available"
        assert "reply" in result

    def test_no_llm_tokens_wasted_on_missing_manifest(self):
        """Regression: the old code called unified_agent first, then checked inventory.
        This burned LLM tokens. The new code checks inventory FIRST."""
        global _inventory_available, _unified_agent_called
        _inventory_available = False
        _unified_agent_called = False

        # Even with a build-like prompt, no LLM call should happen
        result = run_chat_pipeline(self._make_request("add reverb and compression"))

        assert not _unified_agent_called
        assert "⚠️" in result["reply"]
