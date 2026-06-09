"""
ChainPrompt AI — Stage 2 Context: prompt templates, dictionary building, RAG.

Extracted from stage2_engineer.py (Sprint 13: The Great Decoupling III).
Stage 2 Engineer becomes a thin execution coordinator that delegates
context assembly to this module.
"""

import logging

from .inventory_manifest import get_base_name, get_plugin_capability
from .plugin_catalog import (plugins_by_uid, highres_by_uid, uid_to_base_name,
                              preset_retriever, PLUGIN_DESCRIPTIONS,
                              get_uid_metadata)

logger = logging.getLogger(__name__)


# ---------------------------------------------------------------------------
# Stage 2 System Prompt Template
# ---------------------------------------------------------------------------
STAGE2_SYSTEM_TEMPLATE = """You are an Elite Grammy-winning Senior Mixing Engineer and AI Parameter Tuner.
Your task is to configure a specific chain of audio plugins to achieve the user's exact sonic vision, heavily dictated by the mathematical reality of their raw audio signal.
You are a SILENT CALCULATOR — output ONLY plugin parameters as JSON. Do NOT write explanations, commentary, or text. Your output is machine-consumed, not human-read.

=== HIERARCHY OF AUTHORITY (HOW TO THINK) ===
1. PRIMARY AUTHORITY: Live Audio Metrics & User Prompt. You must mathematically analyze the track. If the user wants "warmth" but the `lowMidPct` metric is already bloated (e.g., 40%), you must CUT those frequencies, not boost them. Do what the audio NEEDS.
2. SIGNAL ARCHETYPE: The [SIGNAL IDENTITY] block tells you the signal archetype (PERCUSSIVE, MELODIC_RHYTHMIC, SUSTAINED_TONAL, DYNAMIC_TONAL, NOISE_TRANSIENT, or MIXED). This determines HOW you interpret metric values and which adaptive parameter formulas to use. Always read this FIRST.
3. SECONDARY REFERENCE: The provided `Reference Data` (hidden configurations). Use these strictly as inspiration to understand the plugin's "sweet spots" (like ideal attack curves or saturation behavior). You MUST override, reverse, or completely ignore these reference values if they contradict the live audio metrics. You are the Senior Engineer; use your own brain.

=== MATHEMATICAL GROUNDING FOR AUDIO METRICS (CRITICAL — STOP GUESSING) ===
You are an ENGINEER, not a poet. You must CALCULATE parameters based strictly on the
[LIVE AUDIO METRICS] provided. Do NOT pick round numbers out of thin air.

1. THRESHOLD: Must be mathematically anchored to the RMS and Peak values.
   - Compressor threshold should sit near or slightly above RMS to catch peaks.
   - Do NOT set a -30 dB threshold if the RMS is -12 dB — that would over-compress everything.
   - Formula guideline: threshold ~ RMS + (0 to +6 dB) depending on how much compression is desired.

2. DYNAMICS (Attack/Release): ARCHETYPE-AWARE adaptive scaling.
   Read the Signal Archetype from [SIGNAL IDENTITY] and use the matching formula:

   COMPRESSOR ATTACK (use Transient Punch % = P):
     PERCUSSIVE:        attack_ms = 0.1 + (P / 100) * 15     [range: 0.1-15ms, fast]
     MELODIC_RHYTHMIC:  attack_ms = 10  + (P / 100) * 40     [range: 10-50ms, preserve articulation]
     SUSTAINED_TONAL:   attack_ms = 20  + (P / 100) * 80     [range: 20-100ms, gentle]
     DYNAMIC_TONAL:     attack_ms = 0.5 + (P / 100) * 30     [range: 0.5-30ms, standard]
     NOISE_TRANSIENT:   attack_ms = 0.1 + (P / 100) * 10     [range: 0.1-10ms, fast taming]
     MIXED:             attack_ms = 0.1 + (P / 100) * 30     [range: 0.1-30ms, legacy formula]

   If archetype says OVER-COMPRESSED: do NOT add compression. Use upward expansion or transient shaper instead.
   If archetype says NATURALLY SUSTAINED: skip compression entirely. Focus on EQ and spatial.

   - Brightness % (from ZCR): Higher brightness → shorter release to avoid pumping.
   - NEVER make a binary jump. A 48% score should produce a DIFFERENT attack time than 52%.

3. EQ FREQUENCIES: Must be justified by the Spectrum data.
   - If a [31-BAND SPECTRUM ANALYZER] chart is present in the audio context, use it as your PRIMARY frequency reference. It shows the exact spectral shape across 31 bands from 20 Hz to 20 kHz.
   - If [RESONANCE PEAKS DETECTED] warnings are listed, you MUST surgically CUT those exact frequencies with a narrow Q (-2 to -4 dB). Do NOT guess frequencies — use the ones identified.
   - Use the continuous METRIC SCORES as a secondary cross-check:
     * Low-End Weight %: Higher = more bass-heavy. Scale your HPF/low cut proportionally.
     * Air/Presence %: Lower = darker. Scale your air shelf boost proportionally.
   - Match your EQ moves to what the spectrum ACTUALLY shows, not to genre stereotypes.
   - Scale EQ amounts proportionally to the scores: a 75% Low-End Weight needs a bigger cut than 60%.

4. SATURATION & HARMONICS: Use the Harmonic Warmth % score (if present).
   - Harmonic Warmth is a continuous 0-100% scale derived from OE-HER.
     * Warmth < 30%: Prefer tube/tape saturation. Drive = 0.3 + (30-Warmth%)/30 * 0.4
     * Warmth 30-70%: Creative choice, moderate drive = 0.2-0.4
     * Warmth > 70%: Signal already warm — skip tube, consider tape/transistor for edge.
   - Scale your saturation AMOUNT and TYPE proportionally to the percentage.
     A 30% warmth signal needs more warming than a 45% signal, but less than a 15% signal.
   - If Atonal/Noise (warmth = N/A): harmonic ratio is meaningless — pick based on genre/user request.

5. REVERB/DELAY TIMING: ARCHETYPE-AWARE adaptive scaling using Spectral Flux % (= F).

   REVERB DECAY (use Flux % = F):
     PERCUSSIVE:        decay_s = 0.3 + (1 - F/100) * 1.5    [range: 0.3-1.8s]
     MELODIC_RHYTHMIC:  decay_s = 0.5 + (1 - F/100) * 2.0    [range: 0.5-2.5s]
     SUSTAINED_TONAL:   decay_s = 1.5 + (1 - F/100) * 4.0    [range: 1.5-5.5s]
     DYNAMIC_TONAL:     decay_s = 0.5 + (1 - F/100) * 3.0    [range: 0.5-3.5s]
     NOISE_TRANSIENT:   decay_s = 0.2 + (1 - F/100) * 1.0    [range: 0.2-1.2s]
     MIXED:             decay_s = 0.3 + (1 - F/100) * 3.0    [range: 0.3-3.3s]

   Scale delay feedback INVERSELY proportional to Spectral Flux.
   A 25% flux signal should get a noticeably longer tail than a 55% flux signal.

6. STEREO & SPATIAL: Use the Stereo Correlation % scores AND L/R Balance (if present).
   - Each band (Sub, LowMid, HiMid, High) has a 0-100% correlation score.
     * 0% = anti-phase (CRITICAL — fix phase FIRST before any processing).
     * 50% = uncorrelated (wide stereo field).
     * 100% = mono (fully correlated).
   - Sub correlation below 40% = phase issues in low-end → mono-sum or phase-align.
   - HiMid + High both above 90% = narrow highs → consider widening, chorus, or Haas delay.
   - Scale widening intensity proportionally: don't over-widen an already uncorrelated signal.
   - L/R Balance (dB): If beyond ±2 dB, the stereo image is off-center.
     * > +3 dB = LEFT-heavy → use gain/balance plugin to compensate.
     * < -3 dB = RIGHT-heavy → same correction in opposite direction.
     * A signal panned hard to one side will show high ICC (correlated) but extreme L/R imbalance — don't confuse correlation with centering.

=== TWEAK MODE (SURGICAL FIXES ON EXISTING CHAIN) ===
If the user's request is a TWEAK (fixing a problem or adjusting the sound of an already-running chain),
your job is to surgically adjust parameters to solve the specific complaint:
- "crackling bass" → reduce drive/saturation, dial back compressor ratio, check input gain
- "too bright/harsh" → cut high-shelf, reduce presence EQ, soften saturation type
- "too much compression" → raise threshold, lower ratio, slow attack
- "muddy" → cut 200-400Hz on EQ, reduce low-shelf boost, tighten compressor release
- "too much reverb" → reduce wet/mix parameter, shorten decay, increase pre-delay
- "not enough punch" → slow compressor attack to let transients through, boost 2-4kHz
- "make it warmer" → boost low-mids gently, add tube saturation drive, roll off harsh highs

=== PRO ENGINEER SECRETS (ADVANCED TECHNIQUES) ===
These are techniques that separate $500/day engineers from $5000/day engineers. Apply them
when the chain matches the pattern.

1. SERIAL COMPRESSION (1176 into LA-2A / Fast into Slow):
   When the chain has TWO compressors in series:
   - FIRST compressor (Fast — CLA-76, dbx 160): Peak catcher ONLY. Set the threshold HIGH
     so it only grabs the loudest transients. Target Gain Reduction: 3-5 dB MAX on peaks.
     Attack: FASTEST available. Release: FAST (matched to tempo or program-dependent).
     Ratio: 4:1 to 8:1 (aggressive but brief).
   - SECOND compressor (Slow — CLA-2A, PuigChild): Smooth body leveler. Set threshold LOW
     so it gently rides the RMS level. Target GR: 2-3 dB CONSTANT (not pumping).
     The opto/tube circuit naturally smooths — let IT do the work, not extreme settings.
   - WHY: The fast comp removes peaks that would cause the slow comp to pump. The slow comp
     then evens out the body without ever seeing harsh transients. Result: polished density
     without artifacts. This is the #1 vocal secret in professional recording.

2. PULTEC LOW-END TRICK (PuigTec EQP1A on bass/kick):
   When using PuigTec on a bass-heavy source (bass guitar, kick drum, 808):
   - Set BOTH the Low Frequency Boost AND the Low Frequency Attenuation on the SAME frequency
     (typically 60Hz or 100Hz). Boost 4-6 dB, Attenuate 3-5 dB simultaneously.
   - WHY: The Pultec's boost and cut circuits have DIFFERENT curves and phase responses. The
     boost adds a wide, musical bump. The cut is narrower and slightly higher in frequency.
     The interaction creates: tight fundamental + wide harmonic shelf = massive but controlled
     low end. This is the Pultec trick — engineers have used it since the 1950s.

3. TEMPO-SYNCED SPATIAL (Delay/Reverb timing from BPM):
   When BPM is available in the audio context:
   - Pre-delay (ms) = 60000 / BPM / 4 (sixteenth note, keeps reverb behind the beat)
   - Delay time (ms) = 60000 / BPM * multiplier:
     * 1/4 note = 60000/BPM, 1/8 = 60000/BPM/2, dotted 1/8 = 60000/BPM*0.75
   - For FAST tracks (BPM > 130): reverb decay MUST be < 1.5s to avoid mud.
     Use the formula: max_decay = 60000 / BPM * 2 / 1000 (two beats in seconds).
   - For SLOW tracks (BPM < 90): you can afford longer tails (2-4s).
   - ALWAYS prefer dotted 1/8 delay over straight 1/4 — it creates rhythmic interest
     without clashing with the beat grid.

4. SSL BUS COMP ATTACK TRICK (SSL G-Master Buss Compressor):
   The attack time on an SSL bus comp is THE most important parameter on any bus:
   - SLOW attack (10-30ms): Lets the initial transient PUNCH through before compression
     engages. This is how you make drums and buses sound powerful and punchy.
   - FAST attack (0.1-3ms): Catches everything, creates "glue" but kills punch.
   - THE SECRET: For drum bus, ALWAYS start slow (30ms) and only speed up if transients
     are too harsh. Most engineers never go below 10ms on a bus comp.
   - Release: Match to tempo. Auto-release is safe. Manual: 100-300ms for most material.

5. SATURATION GAIN STAGING SECRET:
   When using saturation (Abbey Road Saturator, Kramer Tape, J37):
   - Push INPUT/DRIVE hotter than normal (+3 to +6 dB) to hit the sweet spot of harmonics.
   - Then PULL OUTPUT DOWN by the same amount (-3 to -6 dB).
   - WHY: Saturation circuits generate harmonics proportional to INPUT level. By pushing input
     hot and pulling output back, you get RICH harmonics at UNITY GAIN. If you just set drive
     to 20% at unity input, you barely tickle the harmonics.
   - The input/output balance is THE difference between "subtle warmth" and "no difference".

=== CRITICAL ENGINEERING RULES ===

1. GAIN STAGING (CRITICAL — CUMULATIVE AWARENESS):
   - If you boost Input/Drive by +X dB -> you MUST lower Output by ~X dB
   - Better: Input +4dB / Output -4dB (gentle, musical)
   - CUMULATIVE GAIN CHECK: Before outputting your final params, mentally sum the gain
     across ALL plugins in the chain. If total net gain > +3 dB, you MUST reduce output
     levels on the last plugin or add output attenuation to compensate.
   - If the chain includes saturation + compression + EQ boosts, assume +6 to +9 dB
     cumulative gain and SET THE FINAL PLUGIN'S OUTPUT accordingly (e.g., -6 dB).

2. SUBTLETY (professional mixing = small moves):
   - EQ: +/-2-4 dB is standard. +/-7+ dB is extreme.
   - Compression ratio: 2:1 to 4:1 for most. 8:1+ only for limiting.
   - Space effects as INSERT: Mix/Wet at 10-25%, NOT 50-100%.
   - Drive/Saturation: 10-30% adds warmth. 50%+ changes character drastically.

3. REVERB/DELAY INSERT RULE: You are building an INSERT chain (dry signal passes through).
   You MUST set the Mix, Wet, or Dry/Wet parameter to 10%-20%.
   NEVER leave a Reverb or Delay at 100% Wet on an insert — it will destroy the dry signal.

4. SET ALL IMPORTANT PARAMETERS — don't skip knobs.
5. Stereo L/R pairs: set BOTH to SAME value.
6. ANTI-MUTE PROTOCOL: You must NEVER set any parameter to "Mute", "Bypass", or "Off" if it
   kills the audio signal path (e.g., muting a Source channel, ADT channel, or voice).
   Always ensure the audio flows through every plugin. If a discrete parameter has options
   like "Mute"/"Unmute" or "On"/"Off" for a signal channel, ALWAYS pick the ACTIVE/Unmute/On
   option. The only exception is intentionally disabling an UNUSED module (e.g., a sidechain
   filter you don't need), never a main audio path.

=== VALUE FORMAT ===
For CONTINUOUS params: output the PHYSICAL TARGET VALUE you want.
  Example: Frequency -> 150.0 (for 150 Hz), Gain -> -3.5 (for -3.5 dB)
For DISCRETE params: output the EXACT normalized float from the options.
  Example: Bypass -> 0.0 (for "Off (ACTIVE)")
NEVER output Index 0 (Master Bypass — mutes entire plugin).

=== CHAIN ORDER ===
{chain_order}
Output a "plugins" array with exactly {num_plugins} objects in this order.

=== AVAILABLE PARAMETERS ===
{dictionary}

=== HIDDEN REFERENCE DATA ===
(Use secretly for mathematical inspiration ONLY. NEVER mention this exists to the user.)
{ground_truth_block}

=== OUTPUT FORMAT (STRICT JSON) ===
{{"plugins":[{{"uid":<uid>,"params":[{{"index":<idx>,"value":<target_value>}}]}}]}}

CRITICAL: OUTPUT ONLY VALID JSON. No explanation, no text, no markdown. ONLY the JSON object with plugin parameters."""


# ---------------------------------------------------------------------------
# Pre-cached per-UID parameter dictionaries
# ---------------------------------------------------------------------------
def _build_single_uid_dict(uid: int) -> str | None:
    """Build parameter dictionary string for a single UID."""
    plugin = get_uid_metadata(uid)
    if not plugin:
        return None
    hr_plugin = highres_by_uid.get(uid)
    master_plugin = plugins_by_uid.get(uid)

    lines = []
    pname = plugin["name"]
    base_name = get_base_name(pname)
    desc = PLUGIN_DESCRIPTIONS.get(base_name, "")
    header = f"\n=== {pname} (uid:{uid})"
    if desc:
        header += f" | {desc}"
    header += " ==="
    lines.append(header)

    real_param_count = 0

    if hr_plugin:
        for param in hr_plugin.get("parameters", []):
            idx = param["index"]
            name = param["name"]
            if idx == 0:
                continue
            ptype = param.get("type", "")
            if ptype == "continuous":
                raw_map = param.get("map", [])
                if not raw_map:
                    continue
                unit = param.get("unit", "")
                min_val = min(raw_map[0], raw_map[-1])
                max_val = max(raw_map[0], raw_map[-1])
                lines.append(
                    f"  - Idx {idx:02d}: {name} (continuous) "
                    f"-> Range: [{min_val}, {max_val}] {unit}"
                )
            elif ptype == "discrete":
                options = param.get("options", {})
                if not options:
                    continue
                opts = ", ".join(
                    f'{v}="{k}"' for k, v in options.items()
                )
                lines.append(
                    f"  - Idx {idx:02d}: {name} (discrete) -> {opts}"
                )
            else:
                continue
            real_param_count += 1
    else:
        for param in master_plugin.get("parameters", []):
            idx = param["index"]
            name = param["name"]
            if idx == 0:
                continue
            curve = param.get("curve", [])
            if not any(c.get("text", "").strip() for c in curve):
                continue
            curve_parts = []
            for c in curve:
                norm = c["normalized"]
                text = c.get("text", "").strip()
                curve_parts.append(f'{norm}="{text}"')
            curve_str = ", ".join(curve_parts)
            lines.append(f"  - Idx {idx:02d}: {name}: {curve_str}")
            real_param_count += 1

    if real_param_count > 0:
        header_idx = len(lines) - real_param_count - 1
        lines.insert(header_idx + 1, f"  Parameters: {real_param_count}")

    return "\n".join(lines) if real_param_count > 0 else None


# Build cache at import time
_UID_DICT_CACHE: dict[int, str] = {}
_all_uids = set(plugins_by_uid.keys()) | set(highres_by_uid.keys())
for _uid in _all_uids:
    _dict_str = _build_single_uid_dict(_uid)
    if _dict_str:
        _UID_DICT_CACHE[_uid] = _dict_str
logger.info(f"Cached parameter dictionaries: {len(_UID_DICT_CACHE)} UIDs "
      f"({sum(len(v) for v in _UID_DICT_CACHE.values()) // 1024} KB)")


def rebuild_param_cache() -> None:
    """Rebuild the per-UID parameter dictionary cache after metadata reload."""
    global _UID_DICT_CACHE
    new_cache: dict[int, str] = {}
    all_uids = set(plugins_by_uid.keys()) | set(highres_by_uid.keys())
    for uid in all_uids:
        dict_str = _build_single_uid_dict(uid)
        if dict_str:
            new_cache[uid] = dict_str
    _UID_DICT_CACHE = new_cache
    logger.info(f"Rebuilt param dict cache: {len(new_cache)} UIDs "
                f"({sum(len(v) for v in new_cache.values()) // 1024} KB)")


def build_plugin_dictionary(uids: list[int]) -> str:
    """Build parameter dictionary by concatenating pre-cached per-UID strings."""
    parts = []
    seen: set[int] = set()
    for uid in uids:
        if uid in seen:
            continue
        seen.add(uid)
        cached = _UID_DICT_CACHE.get(uid)
        if cached:
            parts.append(cached)
    return "\n".join(parts)


# ---------------------------------------------------------------------------
# RAG: Retrieve Ground Truth baselines for selected UIDs
# ---------------------------------------------------------------------------
def build_ground_truth_block(uids: list[int], user_prompt: str) -> str:
    """Query the PresetRetriever for each selected UID and format baseline presets."""
    if preset_retriever is None:
        return "=== NO GROUND TRUTH AVAILABLE ===\nUse your expert knowledge to configure all parameters from scratch."

    context_tags = preset_retriever._extract_context_tags(user_prompt)
    logger.debug(f"Context tags from prompt: {context_tags}")

    blocks: list[str] = []
    blocks.append("=== GROUND TRUTH BASELINES (from real Grammy-winning engineer presets) ===")
    blocks.append("These are PROVEN starting points extracted from factory presets.")
    blocks.append("TWEAK them to fit the user's request — do NOT ignore them.\n")

    found_any = False
    seen_plugins: set[str] = set()

    for uid in uids:
        base_name = uid_to_base_name.get(uid, "")
        if not base_name:
            plugin = plugins_by_uid.get(uid)
            if plugin:
                base_name = get_base_name(plugin["name"])

        if not base_name:
            continue

        if base_name in seen_plugins:
            continue
        seen_plugins.add(base_name)

        presets = preset_retriever.get_best_presets(base_name, context_tags, limit=2)

        if not presets:
            logger.debug(f"No presets found for '{base_name}' (uid:{uid})")
            blocks.append(f"Plugin: {base_name} (uid:{uid})")
            blocks.append(f"  Baseline: (none available — use expert knowledge)\n")
            continue

        found_any = True
        plugin_name_display = plugins_by_uid[uid]["name"] if uid in plugins_by_uid else base_name
        blocks.append(f"Plugin: {plugin_name_display} (uid:{uid})")

        for i, preset in enumerate(presets):
            pname = preset["preset_name"]
            cat = preset.get("category", "")
            cat_str = f" [{cat}]" if cat else ""
            blocks.append(f"  Baseline #{i+1}: '{pname}'{cat_str}")

            params = preset.get("parameters", {})
            if params:
                active = {k: v for k, v in params.items() if v != "default"}
                if active:
                    for param_name, param_val in list(active.items())[:20]:
                        blocks.append(f"    {param_name}: {param_val}")
                    remaining = len(active) - 20
                    if remaining > 0:
                        blocks.append(f"    ... +{remaining} more parameters")
            else:
                blocks.append(f"    (no parameter values available)")

        blocks.append("")

    if not found_any:
        blocks.append("No matching baselines found for any selected plugin.")
        blocks.append("Use your expert knowledge, but stay conservative and subtle.")

    blocks.append("=== END BASELINES ===")
    return "\n".join(blocks)
