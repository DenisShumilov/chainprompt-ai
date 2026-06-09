"""
ChainPrompt AI — Database facade (SHIM).

This module is a backward-compatibility re-export layer.
Real logic now lives in:
  - server/inventory_manifest.py (manifest, capability, metadata control plane)
  - server/plugin_catalog.py (master DB, metadata, catalogs, name/UID resolution)

Existing imports from other modules continue to work unchanged.
DEPRECATED: New code should import from the specific module directly.
"""

# --- Inventory Authority ---
from .inventory_manifest import (  # noqa: F401
    get_base_name as _get_base_name,
    is_uid_installed,
    is_base_name_installed,
    validate_uids,
    is_inventory_available,
    get_inventory_status,
    get_plugin_capability,
    get_metadata_status,
    get_metadata_provider,
    get_metadata_backlog,
    reload_installed_manifest,
    _manifest_loaded,
    _installed_uids,
)

# --- Plugin Catalog ---
from .plugin_catalog import (  # noqa: F401
    plugins_list,
    plugins_by_uid,
    uid_to_base_name,
    highres_by_uid,
    highres_params,
    META_GROUNDTRUTH,
    PLUGIN_DESCRIPTIONS,
    META_BY_NAME,
    FULL_CATALOG_TEXT,
    _total_catalog,
    _described_count,
    _all_variants,
    _uid_format_map,
    preset_retriever,
    get_uid_metadata,
    build_filtered_catalog,
    get_cached_catalog,
    _rebuild_cached_catalogs,
    enforce_plugin_format,
    enforce_plugin_format_list,
    resolve_name_to_uid,
    uid_to_name,
    uids_to_names,
    names_to_uids,
)

# Re-export get_base_name under its original private name for consumers
# that imported `from .db import _get_base_name`
# (already aliased above)


def get_base_name_capability(base_name: str) -> str:
    """Shim: wraps inventory_manifest.get_base_name_capability with _uid_format_map."""
    from .inventory_manifest import get_base_name_capability as _gbnc
    return _gbnc(base_name, _uid_format_map)
