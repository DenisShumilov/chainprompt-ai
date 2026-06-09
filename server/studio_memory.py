"""
ChainPrompt AI — Studio Memory: thread-safe persistence gateway.

Wraps SQLite operations behind a threading.Lock so Waitress worker threads
cannot interleave writes. All public functions acquire the lock before
touching the connection.

Replaces the raw studio_db.py (which had check_same_thread=False with no
serialization around writes — a real concurrency/data-integrity risk).

Schema changes vs studio_db.py:
  - REMOVED: global_mix_state.current_sends (dead column — sends amputated)
  - ADDED:   project_scope migration (carried forward)
"""

import json
import logging
import re
import sqlite3
import os
import threading
from datetime import datetime, timezone, timedelta

logger = logging.getLogger(__name__)

_PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DB_FILE = os.path.join(_PROJECT_ROOT, "studio_memory.db")

_conn: sqlite3.Connection | None = None
_lock = threading.Lock()

CROSS_TRACK_TTL_MINUTES = 10


# ---------------------------------------------------------------------------
# Initialization
# ---------------------------------------------------------------------------
def init_db():
    """Create tables if they don't exist. Call once at server startup."""
    global _conn
    with _lock:
        _conn = sqlite3.connect(DB_FILE, check_same_thread=False)
        _conn.execute("PRAGMA journal_mode=WAL")

        _conn.executescript("""
            CREATE TABLE IF NOT EXISTS track_instances (
                instance_id  TEXT PRIMARY KEY,
                track_name   TEXT,
                current_chain TEXT,
                updated_at   TIMESTAMP
            );

            CREATE TABLE IF NOT EXISTS chat_logs (
                id           INTEGER PRIMARY KEY AUTOINCREMENT,
                instance_id  TEXT,
                role         TEXT,
                message      TEXT,
                timestamp    TIMESTAMP
            );

            CREATE TABLE IF NOT EXISTS global_mix_state (
                instance_id   TEXT PRIMARY KEY,
                track_name    TEXT,
                metrics_json  TEXT,
                current_chain TEXT,
                updated_at    TIMESTAMP
            );

            CREATE TABLE IF NOT EXISTS user_preferences (
                id            INTEGER PRIMARY KEY AUTOINCREMENT,
                preference    TEXT UNIQUE,
                extracted_at  TIMESTAMP
            );
        """)
        _conn.commit()

        # Migration: add project_scope column to global_mix_state
        _migrate_add_column("global_mix_state", "project_scope", "TEXT")

        # Migration: drop dead current_sends column if it exists.
        # SQLite doesn't support DROP COLUMN before 3.35.0, so we just
        # ignore it — the column is harmless (always NULL) and will be
        # excluded from all queries going forward.

    logger.info(f"Studio Memory initialized: {DB_FILE}")


def _migrate_add_column(table: str, column: str, col_type: str):
    """Safely add a column if it doesn't exist (idempotent migration).

    MUST be called while _lock is held.
    """
    if _conn is None:
        return
    try:
        _conn.execute(f"ALTER TABLE {table} ADD COLUMN {column} {col_type}")
        _conn.commit()
        logger.info(f"Migration: added {table}.{column}")
    except sqlite3.OperationalError:
        pass  # column already exists


# ---------------------------------------------------------------------------
# Chain entry helpers (handle both legacy [int] and rich [dict] formats)
# ---------------------------------------------------------------------------
def _normalize_chain_entry(item: dict | int) -> dict:
    """Normalize a single chain item to a full slot entry dict."""
    if isinstance(item, dict):
        return {
            "uid": item.get("uid", 0),
            "slot_id": item.get("slot_id", 0),
            "user_touched": bool(item.get("user_touched", False)),
            "bypassed": bool(item.get("bypassed", False)),
            "user_owned": bool(item.get("user_owned", False)),
        }
    return {"uid": int(item), "slot_id": 0, "user_touched": False,
            "bypassed": False, "user_owned": False}


def normalize_chain(chain_data: list | None) -> list[dict]:
    """Normalize chain data to list of rich entry dicts (handles legacy format)."""
    if not chain_data:
        return []
    return [_normalize_chain_entry(item) for item in chain_data]


def chain_uids(chain_data: list | None) -> list[int]:
    """Extract just UIDs from chain data (handles both formats)."""
    if not chain_data:
        return []
    return [item["uid"] if isinstance(item, dict) else int(item)
            for item in chain_data]


# ---------------------------------------------------------------------------
# Track instances
# ---------------------------------------------------------------------------
def upsert_track(instance_id: str, track_name: str,
                 current_chain: list[dict] | list[int]):
    """Insert or update a track instance with its current chain.

    Thread-safe: acquires _lock for the entire write operation.
    """
    entries = normalize_chain(current_chain)
    chain_json = json.dumps(entries)
    now = datetime.now(timezone.utc).isoformat()
    with _lock:
        if _conn is None:
            return
        _conn.execute(
            """INSERT INTO track_instances (instance_id, track_name, current_chain, updated_at)
               VALUES (?, ?, ?, ?)
               ON CONFLICT(instance_id) DO UPDATE SET
                   track_name = excluded.track_name,
                   current_chain = excluded.current_chain,
                   updated_at = excluded.updated_at""",
            (instance_id, track_name, chain_json, now),
        )
        _conn.commit()


# ---------------------------------------------------------------------------
# Chat logs
# ---------------------------------------------------------------------------
def log_chat(instance_id: str, role: str, message: str):
    """Append a chat message to the log."""
    now = datetime.now(timezone.utc).isoformat()
    with _lock:
        if _conn is None:
            return
        _conn.execute(
            "INSERT INTO chat_logs (instance_id, role, message, timestamp) VALUES (?, ?, ?, ?)",
            (instance_id, role, message, now),
        )
        _conn.commit()


def clear_chat_history(instance_id: str):
    """Delete all chat messages for a track instance (used by New Chat)."""
    with _lock:
        if _conn is None:
            return
        _conn.execute("DELETE FROM chat_logs WHERE instance_id = ?", (instance_id,))
        _conn.commit()


def get_chat_history(instance_id: str, limit: int = 30) -> list[dict]:
    """Fetch the last `limit` chat messages for a track instance.

    Returns list of {"role": "user"|"assistant", "text": "..."} dicts,
    ordered oldest-first (chronological).
    Thread-safe: acquires _lock for the read.
    """
    with _lock:
        if _conn is None:
            return []
        cursor = _conn.execute(
            """SELECT role, message FROM chat_logs
               WHERE instance_id = ?
               ORDER BY id DESC LIMIT ?""",
            (instance_id, limit),
        )
        rows = cursor.fetchall()

    # Processing outside lock (no DB access needed)
    rows.reverse()
    result = []
    for role, message in rows:
        if role == "ai":
            api_role = "assistant"
        elif role == "system":
            api_role = "user"
            message = f"[SYSTEM]: {message}"
        else:
            api_role = "user"
        result.append({"role": api_role, "text": message})
    return result


# ---------------------------------------------------------------------------
# Global Mix State (cross-track awareness)
# ---------------------------------------------------------------------------
def upsert_mix_state(instance_id: str, track_name: str,
                     metrics: dict | None = None,
                     current_chain: list[dict] | list[int] | None = None,
                     project_scope: str | None = None):
    """Update the global mix state for a track instance.

    Called from /api/ping (background) or /api/chat (on each request).
    Stores rich slot entries (not just UIDs) for full slot-state awareness.
    Thread-safe: acquires _lock for the entire write.

    NOTE: current_sends column is dead (sends amputated) — excluded from queries.
    """
    now = datetime.now(timezone.utc).isoformat()
    metrics_json = json.dumps(metrics) if metrics is not None else None

    if current_chain is not None:
        entries = normalize_chain(current_chain)
        chain_json = json.dumps(entries)
    else:
        chain_json = None

    with _lock:
        if _conn is None:
            return
        _conn.execute(
            """INSERT INTO global_mix_state
                   (instance_id, track_name, metrics_json, current_chain,
                    project_scope, updated_at)
               VALUES (?, ?, ?, ?, ?, ?)
               ON CONFLICT(instance_id) DO UPDATE SET
                   track_name = excluded.track_name,
                   metrics_json = COALESCE(excluded.metrics_json, global_mix_state.metrics_json),
                   current_chain = CASE
                       WHEN excluded.current_chain IS NOT NULL THEN excluded.current_chain
                       ELSE global_mix_state.current_chain
                   END,
                   project_scope = COALESCE(excluded.project_scope, global_mix_state.project_scope),
                   updated_at = excluded.updated_at""",
            (instance_id, track_name, metrics_json, chain_json, project_scope, now),
        )
        _conn.commit()


def get_all_tracks(project_scope: str | None = None,
                   max_age_minutes: int | None = None) -> list[dict]:
    """Get all known tracks with their latest state.

    Thread-safe: acquires _lock for the read.
    """
    with _lock:
        if _conn is None:
            return []

        query = """SELECT instance_id, track_name, metrics_json, current_chain,
                          project_scope, updated_at
                   FROM global_mix_state"""
        conditions = []
        params: list = []

        if project_scope:
            conditions.append("project_scope = ?")
            params.append(project_scope)

        if max_age_minutes is not None:
            cutoff = (datetime.now(timezone.utc) - timedelta(minutes=max_age_minutes)).isoformat()
            conditions.append("updated_at >= ?")
            params.append(cutoff)

        if conditions:
            query += " WHERE " + " AND ".join(conditions)
        query += " ORDER BY updated_at DESC"

        cursor = _conn.execute(query, params)
        rows = cursor.fetchall()

    # Process outside lock
    result = []
    for row in rows:
        raw_chain = json.loads(row[3]) if row[3] else []
        entry = {
            "instance_id": row[0],
            "track_name": row[1],
            "metrics": json.loads(row[2]) if row[2] else None,
            "current_chain": chain_uids(raw_chain),
            "current_chain_entries": normalize_chain(raw_chain),
            "project_scope": row[4],
            "updated_at": row[5],
        }
        result.append(entry)
    return result


def find_track_by_name(name: str, exclude_instance: str | None = None,
                       project_scope: str | None = None,
                       max_age_minutes: int | None = None) -> dict | None:
    """Find a track by name (case-insensitive partial match).

    Thread-safe: acquires _lock for the read.
    """
    with _lock:
        if _conn is None:
            return None

        query = """SELECT instance_id, track_name, metrics_json, current_chain,
                          project_scope
                   FROM global_mix_state"""
        conditions = []
        params: list = []

        if project_scope:
            conditions.append("project_scope = ?")
            params.append(project_scope)

        if max_age_minutes is not None:
            cutoff = (datetime.now(timezone.utc) - timedelta(minutes=max_age_minutes)).isoformat()
            conditions.append("updated_at >= ?")
            params.append(cutoff)

        if conditions:
            query += " WHERE " + " AND ".join(conditions)
        query += " ORDER BY updated_at DESC"

        cursor = _conn.execute(query, params)
        rows = cursor.fetchall()

    name_lower = name.lower().strip()
    for row in rows:
        inst_id, track_name_db = row[0], row[1]
        if exclude_instance and inst_id == exclude_instance:
            continue
        if track_name_db and name_lower in track_name_db.lower():
            raw_chain = json.loads(row[3]) if row[3] else []
            return {
                "instance_id": inst_id,
                "track_name": track_name_db,
                "metrics": json.loads(row[2]) if row[2] else None,
                "current_chain": chain_uids(raw_chain),
                "current_chain_entries": normalize_chain(raw_chain),
                "project_scope": row[4],
            }
    return None


# ---------------------------------------------------------------------------
# User Preferences (persistent memory)
# ---------------------------------------------------------------------------
_PREF_PATTERNS = [
    re.compile(r"\b(?:i\s+(?:like|prefer|want|love|hate|always|never)|"
               r"я\s+(?:люблю|хочу|ненавиджу|завжди|ніколи|волію|полюбляю))\b",
               re.IGNORECASE),
    re.compile(r"\b(?:always\s+use|never\s+use|завжди\s+використовуй|ніколи\s+не)\b",
               re.IGNORECASE),
]


def extract_and_store_preferences(user_prompt: str):
    """If the user states a preference, extract and store it."""
    for pattern in _PREF_PATTERNS:
        if pattern.search(user_prompt):
            pref = user_prompt.strip()[:500]
            now = datetime.now(timezone.utc).isoformat()
            with _lock:
                if _conn is None:
                    return
                try:
                    _conn.execute(
                        """INSERT OR IGNORE INTO user_preferences (preference, extracted_at)
                           VALUES (?, ?)""",
                        (pref, now),
                    )
                    _conn.commit()
                    logger.info(f"Stored preference: {pref[:80]}...")
                except sqlite3.IntegrityError:
                    pass
            return


def get_user_preferences(limit: int = 10) -> list[str]:
    """Fetch the most recent user preferences."""
    with _lock:
        if _conn is None:
            return []
        cursor = _conn.execute(
            "SELECT preference FROM user_preferences ORDER BY id DESC LIMIT ?",
            (limit,),
        )
        return [row[0] for row in cursor.fetchall()]
