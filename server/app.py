"""
ChainPrompt AI — Flask HTTP transport layer.

Thin orchestrator: parses HTTP requests, delegates to service modules,
returns JSON responses. No business logic lives here.

Declarative Chain Architecture:
  Unified Agent (1 LLM call) → Diff Engine → targeted Stage 2 → Iron Dome → C++
"""

import json
import logging
import traceback

from flask import Flask, request, jsonify
from flask_cors import CORS

logging.basicConfig(level=logging.DEBUG, format='%(asctime)s [%(name)s] %(levelname)s %(message)s')
logger = logging.getLogger(__name__)

from .config import GEMINI_MODEL, GEMINI_API_KEYS
from .inventory_manifest import (is_inventory_available, get_inventory_status,
                                  reload_installed_manifest)
from .plugin_catalog import (plugins_list, META_GROUNDTRUTH,
                              _total_catalog, _described_count, preset_retriever,
                              uid_to_name, uids_to_names,
                              _rebuild_cached_catalogs,
                              reload_metadata_envelopes)
from .unified_agent import rebuild_cached_prompts
from .stage2_context import rebuild_param_cache
from .chat_pipeline import run_chat_pipeline
from .studio_memory import (init_db, upsert_mix_state, get_all_tracks,
                             normalize_chain,
                             CROSS_TRACK_TTL_MINUTES)

app = Flask(__name__)
CORS(app)
init_db()


# ---------------------------------------------------------------------------
# API Endpoint: /api/chat
# ---------------------------------------------------------------------------
@app.route("/api/chat", methods=["POST"])
def api_chat():
    try:
        raw_data = request.get_data()
        try:
            text = raw_data.decode('utf-8')
        except UnicodeDecodeError:
            text = raw_data.decode('latin-1')
        data = json.loads(text)

        result = run_chat_pipeline(data)
        return jsonify(result)

    except Exception as e:
        logger.error(f"Request failed: {e}", exc_info=True)
        return jsonify({
            "reply": f"[Server error: {str(e)}]",
            "preset": None,
        }), 500


# ---------------------------------------------------------------------------
# API Endpoint: /api/ping (background metrics from C++)
# ---------------------------------------------------------------------------
@app.route("/api/ping", methods=["POST"])
def api_ping():
    """Lightweight endpoint for C++ background metrics pinging."""
    try:
        data = request.get_json(silent=True) or {}
        instance_id = data.get("instance_id", "unknown")
        track_name = data.get("track_name", "Unknown Track")
        project_scope = data.get("project_scope")
        metrics = data.get("metrics")
        current_chain_raw = data.get("current_chain")

        current_chain_entries: list[dict] | None = None
        if current_chain_raw is not None:
            current_chain_entries = normalize_chain(current_chain_raw)

        upsert_mix_state(instance_id, track_name, metrics,
                         current_chain_entries, project_scope)

        return jsonify({"status": "ok"})
    except Exception as e:
        return jsonify({"status": "error", "error": str(e)}), 500


# ---------------------------------------------------------------------------
# API Endpoint: /api/tracks
# ---------------------------------------------------------------------------
@app.route("/api/tracks", methods=["GET"])
def api_tracks():
    scope = request.args.get("scope")
    fresh = request.args.get("fresh")
    max_age = CROSS_TRACK_TTL_MINUTES if fresh else None
    tracks = get_all_tracks(project_scope=scope, max_age_minutes=max_age)
    return jsonify({"tracks": tracks})


# ---------------------------------------------------------------------------
# API Endpoint: /api/diag
# ---------------------------------------------------------------------------
@app.route("/api/diag", methods=["GET"])
def api_diag():
    """Diagnostics: expose slot-state contract for observability."""
    scope = request.args.get("scope")
    include_detail = request.args.get("detail", "0") == "1"
    tracks = get_all_tracks(project_scope=scope)
    diag = []
    for t in tracks:
        entries = t.get("current_chain_entries", [])
        track_diag: dict = {
            "instance_id": t["instance_id"],
            "track_name": t["track_name"],
            "slots": len(entries),
            "bypassed": sum(1 for e in entries if e.get("bypassed")),
            "user_touched": sum(1 for e in entries if e.get("user_touched")),
            "user_owned": sum(1 for e in entries if e.get("user_owned")),
            "project_scope": t.get("project_scope"),
            "updated_at": t.get("updated_at"),
        }
        if include_detail:
            slot_detail = []
            for i, e in enumerate(entries):
                name = uid_to_name(e.get("uid", 0)) or f"UID {e.get('uid', 0)}"
                slot_detail.append({
                    "index": i,
                    "slot_id": e.get("slot_id", 0),
                    "uid": e.get("uid", 0),
                    "name": name,
                    "bypassed": e.get("bypassed", False),
                    "user_touched": e.get("user_touched", False),
                    "user_owned": e.get("user_owned", False),
                })
            track_diag["chain"] = slot_detail
        diag.append(track_diag)
    return jsonify({"tracks": diag})


# ---------------------------------------------------------------------------
# API Endpoint: /api/rescan
# ---------------------------------------------------------------------------
@app.route("/api/rescan", methods=["POST"])
def api_rescan():
    """Hot-reload installed manifest + metadata envelopes after C++ rescan."""
    try:
        summary = reload_installed_manifest(rebuild_catalogs_fn=_rebuild_cached_catalogs)
        reload_metadata_envelopes()
        rebuild_cached_prompts()
        rebuild_param_cache()
        logger.info(f"Rescan reload complete: {summary}")
        return jsonify({"status": "ok", **summary})
    except Exception as e:
        logger.error(f"Rescan reload failed: {e}", exc_info=True)
        return jsonify({"status": "error", "error": str(e)}), 500


# ---------------------------------------------------------------------------
# Health check
# ---------------------------------------------------------------------------
@app.route("/health", methods=["GET"])
def health():
    return jsonify({
        "status": "ok",
        "plugins_loaded": len(plugins_list),
        "meta_groundtruth": len(META_GROUNDTRUTH),
        "full_catalog": _total_catalog,
        "inventory": get_inventory_status(),
        "rag_retriever": preset_retriever is not None,
        "model": GEMINI_MODEL,
        "api_keys": len(GEMINI_API_KEYS),
    })


# ---------------------------------------------------------------------------
# Main entry point
# ---------------------------------------------------------------------------
def main():
    from waitress import serve

    logger.info(f"ChainPrompt AI backend (Declarative Chain Architecture)")
    logger.info(f"  Model:  {GEMINI_MODEL} (google-genai SDK)")
    logger.info(f"  Keys:   {len(GEMINI_API_KEYS)} API keys configured")
    logger.info(f"  RAG:    {'READY' if preset_retriever else 'DISABLED'}")
    logger.info(f"  Meta:   {len(META_GROUNDTRUTH)} entries ({_total_catalog} in catalog, {_described_count} with tags)")
    logger.info(f"Unified Agent: temp=0.5, schema=UNIFIED_SCHEMA, thinking_budget=0")
    logger.info(f"Stage 2: temp=0.2, schema=STAGE2_SCHEMA, thinking_budget=0")
    logger.info(f"Starting on http://127.0.0.1:5000 (waitress, 6 threads)")
    serve(app, host="127.0.0.1", port=5000, threads=6)


if __name__ == "__main__":
    main()
