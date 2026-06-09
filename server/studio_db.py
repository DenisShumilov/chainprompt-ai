"""
ChainPrompt AI — Studio Memory Database (SHIM).

This module is a backward-compatibility re-export layer.
All real logic now lives in server/studio_memory.py (thread-safe gateway).
Existing imports from other modules continue to work unchanged.

DEPRECATED: New code should import from server.studio_memory directly.
"""

from .studio_memory import (  # noqa: F401
    init_db,
    normalize_chain,
    chain_uids,
    upsert_track,
    log_chat,
    clear_chat_history,
    get_chat_history,
    upsert_mix_state,
    get_all_tracks,
    find_track_by_name,
    extract_and_store_preferences,
    get_user_preferences,
    CROSS_TRACK_TTL_MINUTES,
)
