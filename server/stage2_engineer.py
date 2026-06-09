"""
ChainPrompt AI — Stage 2: The Senior Engineer (Parameter Configuration).

Thin execution coordinator. Context assembly (prompt templates, dictionary
building, RAG ground truth, param cache) lives in stage2_context.py.
"""

import json
import logging
import re
from concurrent.futures import ThreadPoolExecutor, as_completed

from .config import STAGE2_SCHEMA, STAGE2_CHUNK_SIZE
from .inventory_manifest import get_plugin_capability
from .plugin_catalog import plugins_by_uid
from .stage2_context import (STAGE2_SYSTEM_TEMPLATE,
                              build_plugin_dictionary,
                              build_ground_truth_block,
                              rebuild_param_cache)
from .gemini_client import call_llm
from .iron_dome import iron_dome_filter

logger = logging.getLogger(__name__)

# Re-export for app.py backward compatibility
rebuild_param_cache = rebuild_param_cache


# ---------------------------------------------------------------------------
# Parse Stage 2 response
# ---------------------------------------------------------------------------
def parse_stage2_response(raw: str) -> dict:
    """Extract UI text and preset JSON from Stage 2 response."""
    result = {"text": "", "preset": None}

    json_str = raw.strip()
    md_match = re.search(r'```(?:json)?\s*\n?([\s\S]*?)```', json_str)
    if md_match:
        json_str = md_match.group(1).strip()

    parsed = None
    try:
        parsed = json.loads(json_str)
    except json.JSONDecodeError:
        obj_match = re.search(r'\{[\s\S]*\}', raw)
        if obj_match:
            try:
                parsed = json.loads(obj_match.group())
            except json.JSONDecodeError:
                logger.error(f"FAILED to parse JSON from: {raw[:300]}")

    if parsed:
        parsed.pop("explanation", None)
        parsed.pop("reasoning", None)
        preset = iron_dome_filter(parsed)
        result["preset"] = preset
        logger.info(f"Parsed preset with {len(preset.get('plugins', []))} plugins")

    return result


# ---------------------------------------------------------------------------
# Single-chunk Stage 2 call
# ---------------------------------------------------------------------------
def _stage2_single_chunk(prompt: str, audio_context: str,
                         history: list[dict], chunk_uids: list[int],
                         audio_parts: list | None,
                         stage1_strategy: str,
                         full_chain_uids: list[int]) -> dict:
    """Run Stage 2 for a single chunk of plugins. Called in parallel threads."""
    dictionary = build_plugin_dictionary(chunk_uids)
    ground_truth_block = build_ground_truth_block(chunk_uids, prompt)

    chunk_display = []
    chunk_names = []
    for i, uid in enumerate(chunk_uids):
        name = plugins_by_uid[uid]["name"] if uid in plugins_by_uid else f"UID {uid}"
        chunk_display.append(f"  {i+1}. {name} (uid:{uid})")
        chunk_names.append(name)
    chain_order_str = "\n".join(chunk_display)

    system = STAGE2_SYSTEM_TEMPLATE.format(
        ground_truth_block=ground_truth_block,
        chain_order=chain_order_str,
        num_plugins=len(chunk_uids),
        dictionary=dictionary,
    )

    system += (f"\n\n=== USER INTENT / CONTEXT ===\n"
               f"{prompt}\n"
               f"(Use this to understand the instrument, genre, and sonic goal. "
               f"Match your parameter choices to this intent.)")

    if len(full_chain_uids) > len(chunk_uids):
        full_chain_names = []
        for uid in full_chain_uids:
            n = plugins_by_uid[uid]["name"] if uid in plugins_by_uid else f"UID {uid}"
            full_chain_names.append(n)
        full_chain_str = " -> ".join(full_chain_names)

        system += (f"\n\n=== PARALLEL CHAIN CONTEXT ===\n"
                   f"You are part of a larger mixing chain: {full_chain_str}\n")
        if stage1_strategy:
            system += f"The global mixing strategy is: {stage1_strategy}\n"
        system += (f"Assume the plugins before yours have already perfectly executed "
                   f"their part of the plan. Do not overcompensate.\n"
                   f"Your specific job is ONLY to configure: {', '.join(chunk_names)}.")

    if audio_parts:
        system += ("\n\n=== MULTIMODAL AUDIO ===\n"
                   "Listen to the attached audio to understand the genre, mood, and timbral "
                   "characteristics. Combine this subjective listening with the objective "
                   "metrics to tweak the baseline presets.")

    messages = []
    for msg in history:
        role = msg.get("role", "user")
        if role not in ("user", "assistant"):
            role = "user"
        messages.append({"role": role, "content": msg.get("text", "")})
    messages.append({"role": "user", "content": prompt + audio_context})

    raw = call_llm(system, messages, max_tokens=4096,
                   temperature=0.2,
                   response_schema=STAGE2_SCHEMA,
                   thinking_budget=0,
                   extra_parts=audio_parts)
    return parse_stage2_response(raw)


# ---------------------------------------------------------------------------
# Re-inject host-only plugin stubs into Stage 2 results
# ---------------------------------------------------------------------------
def _reinject_hostonly(result: dict, hostonly_results: list[dict]) -> dict:
    """Re-inject host-only plugin stubs at their original positions in the result."""
    preset = result.get("preset")
    if not preset or "plugins" not in preset:
        hostonly_sorted = sorted(hostonly_results, key=lambda x: x["_orig_idx"])
        for stub in hostonly_sorted:
            stub.pop("_orig_idx", None)
        return {"text": result.get("text", ""), "preset": {"plugins": hostonly_sorted}}

    configured_plugins = preset["plugins"]
    hostonly_sorted = sorted(hostonly_results, key=lambda x: x["_orig_idx"])
    ho_positions = {hr["_orig_idx"] for hr in hostonly_sorted}
    ho_iter = iter(hostonly_sorted)

    total = len(configured_plugins) + len(hostonly_sorted)
    all_plugins: list[dict] = []
    config_idx = 0

    for pos in range(total):
        if pos in ho_positions:
            stub = next(ho_iter)
            stub.pop("_orig_idx", None)
            all_plugins.append(stub)
        elif config_idx < len(configured_plugins):
            all_plugins.append(configured_plugins[config_idx])
            config_idx += 1

    logger.info(f"Re-injected {len(hostonly_sorted)} host-only plugins into Stage 2 result")
    return {"text": result.get("text", ""), "preset": {"plugins": all_plugins}}


# ---------------------------------------------------------------------------
# Main entry point: Parallel Batch Processing (Divide & Conquer)
# ---------------------------------------------------------------------------
def stage2_engineer(prompt: str, audio_context: str,
                    history: list[dict], uids: list[int],
                    audio_parts: list | None = None,
                    stage1_strategy: str = "") -> dict:
    """Run Stage 2 with Parallel Batch Processing."""
    # Separate configurable vs host-only UIDs
    configurable_uids: list[int] = []
    hostonly_results: list[dict] = []
    for i, uid in enumerate(uids):
        cap = get_plugin_capability(uid)
        if cap == "configurable":
            configurable_uids.append(uid)
        else:
            name = plugins_by_uid[uid]["name"] if uid in plugins_by_uid else f"UID {uid}"
            logger.info(f"Stage 2: UID {uid} ({name}) is host-only — using factory defaults")
            hostonly_results.append({"_orig_idx": i, "uid": uid, "params": []})

    if not configurable_uids:
        logger.info(f"Stage 2: all {len(uids)} plugins are host-only — skipping LLM call")
        return {
            "text": "",
            "preset": {"plugins": [{"uid": uid, "params": []} for uid in uids]},
        }

    uids = configurable_uids

    if len(uids) <= STAGE2_CHUNK_SIZE:
        result = _stage2_single_chunk(prompt, audio_context, history, uids,
                                      audio_parts, stage1_strategy,
                                      full_chain_uids=uids)
        if hostonly_results:
            result = _reinject_hostonly(result, hostonly_results)
        return result

    chunks: list[list[int]] = []
    for i in range(0, len(uids), STAGE2_CHUNK_SIZE):
        chunks.append(uids[i:i + STAGE2_CHUNK_SIZE])

    logger.info(f"Parallel batching: {len(uids)} plugins -> "
          f"{len(chunks)} chunks of {[len(c) for c in chunks]}")

    results_map: dict[int, dict] = {}
    max_retries = 2

    with ThreadPoolExecutor(max_workers=len(chunks)) as pool:
        future_to_idx = {}
        for idx, chunk in enumerate(chunks):
            future = pool.submit(
                _stage2_single_chunk,
                prompt, audio_context, history, chunk,
                audio_parts, stage1_strategy,
                full_chain_uids=uids,
            )
            future_to_idx[future] = idx

        for future in as_completed(future_to_idx):
            idx = future_to_idx[future]
            try:
                result = future.result()
                results_map[idx] = result
                plugin_count = len(result.get("preset", {}).get("plugins", []))
                logger.info(f"Chunk {idx+1}/{len(chunks)} OK: "
                      f"{plugin_count} plugins configured")
            except Exception as e:
                logger.error(f"Chunk {idx+1}/{len(chunks)} FAILED: {e}")

    # Retry failed chunks
    for idx, chunk in enumerate(chunks):
        if idx in results_map:
            continue
        for attempt in range(max_retries):
            logger.warning(f"Retrying chunk {idx+1} (attempt {attempt+1}/{max_retries})")
            try:
                result = _stage2_single_chunk(
                    prompt, audio_context, history, chunk,
                    audio_parts, stage1_strategy,
                    full_chain_uids=uids,
                )
                results_map[idx] = result
                logger.info(f"Retry chunk {idx+1} OK")
                break
            except Exception as e:
                logger.error(f"Retry chunk {idx+1} failed: {e}")

    # Merge results
    merged_plugins = []
    global_offset = 0
    for idx in range(len(chunks)):
        r = results_map.get(idx)
        if not r:
            logger.warning(f"chunk {idx+1} has no result, skipping")
            global_offset += len(chunks[idx])
            continue
        preset = r.get("preset")
        if preset and "plugins" in preset:
            chunk_plugins = preset["plugins"]
            expected_uids = chunks[idx]
            used = [False] * len(chunk_plugins)
            for local_i, expected_uid in enumerate(expected_uids):
                for pi, p in enumerate(chunk_plugins):
                    if not used[pi] and p.get("uid") == expected_uid:
                        p["_pos"] = global_offset + local_i
                        merged_plugins.append(p)
                        used[pi] = True
                        break
            for pi, p in enumerate(chunk_plugins):
                if not used[pi]:
                    p["_pos"] = global_offset + len(expected_uids) + pi
                    merged_plugins.append(p)
        global_offset += len(chunks[idx])

    merged_plugins.sort(key=lambda p: p.get("_pos", 999))
    for p in merged_plugins:
        p.pop("_pos", None)

    logger.info(f"Merged: {len(merged_plugins)} plugins from {len(results_map)} chunks")

    result = {
        "text": "",
        "preset": {"plugins": merged_plugins} if merged_plugins else None,
    }

    if hostonly_results:
        result = _reinject_hostonly(result, hostonly_results)

    return result
