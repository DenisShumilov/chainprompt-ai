"""
ChainPrompt AI — Audio context builder, metric scoring, archetype classification.
"""

import logging

logger = logging.getLogger(__name__)


# ---------------------------------------------------------------------------
# 31-Band ASCII Spectrum Analyzer + Peak Detection
# ---------------------------------------------------------------------------
_SPECTRUM_FREQS = [
    "20", "25", "31", "40", "50", "63", "80", "100",
    "125", "160", "200", "250", "315", "400", "500",
    "630", "800", "1k", "1.2k", "1.6k", "2k", "2.5k",
    "3.1k", "4k", "5k", "6.3k", "8k", "10k", "12.5k", "16k", "20k"
]

_BAR_CHARS = " ▁▂▃▄▅▆▇█"


def build_spectrum_ascii(spectrum31: list[float]) -> str:
    """Build an ASCII bar chart from 31 dB values + detect resonance peaks."""
    if not spectrum31 or len(spectrum31) < 31:
        return ""

    vals = spectrum31[:31]

    max_db = max(vals)
    floor_db = max(max_db - 60.0, -100.0)

    lines = []
    lines.append("\n[31-BAND 1/3-OCTAVE SPECTRUM ANALYZER]")

    bar_line = ""
    for i, db in enumerate(vals):
        normalized = (db - floor_db) / max(max_db - floor_db, 1.0)
        normalized = max(0.0, min(1.0, normalized))
        idx = int(normalized * 8)
        bar_line += _BAR_CHARS[idx]

    lines.append(f"  {bar_line}")

    label_indices = [0, 4, 8, 12, 16, 20, 24, 28, 30]
    label_line = ""
    for i in range(31):
        if i in label_indices:
            label_line += _SPECTRUM_FREQS[i]
            if i < 30:
                next_label = next((j for j in label_indices if j > i), 31)
                gap = next_label - i - len(_SPECTRUM_FREQS[i]) + 1
                label_line += " " * max(gap, 0)
        elif i not in label_indices:
            pass
    lines.append(f"  {label_line}")

    key_bands = [(0, "20Hz"), (7, "100Hz"), (11, "250Hz"), (14, "500Hz"),
                 (17, "1kHz"), (20, "2kHz"), (24, "5kHz"), (27, "10kHz"), (30, "20kHz")]
    db_summary = " | ".join(f"{name}:{vals[i]:+.1f}dB" for i, name in key_bands)
    lines.append(f"  {db_summary}")

    peaks = []
    for i in range(5, 30):
        if vals[i] > vals[i - 1] and vals[i] > vals[i + 1]:
            prominence = vals[i] - (vals[i - 1] + vals[i + 1]) / 2.0
            if prominence > 2.0:
                peaks.append((i, vals[i], prominence))

    peaks.sort(key=lambda x: -x[2])
    top_peaks = peaks[:2]

    if top_peaks:
        freq_labels = [_SPECTRUM_FREQS[p[0]] + f"Hz({p[1]:+.1f}dB, +{p[2]:.1f}dB)" for p in top_peaks]
        lines.append(f"  [!] RESONANCE PEAKS DETECTED AT: {', '.join(freq_labels)}")
        lines.append(f"  -> Use parametric EQ to surgically CUT these frequencies (-2 to -4 dB, narrow Q 4-8).")

    return "\n".join(lines)


def _detect_resonance_peaks(spectrum31: list[float]) -> str:
    """Return resonance warning string, or empty if no peaks found."""
    vals = spectrum31[:31]
    peaks = []
    for i in range(5, 30):
        if vals[i] > vals[i - 1] and vals[i] > vals[i + 1]:
            prominence = vals[i] - (vals[i - 1] + vals[i + 1]) / 2.0
            if prominence > 2.0:
                peaks.append((i, vals[i], prominence))
    peaks.sort(key=lambda x: -x[2])
    top = peaks[:2]
    if top:
        labels = [f"{_SPECTRUM_FREQS[p[0]]}Hz({p[1]:+.1f}dB, +{p[2]:.1f}dB)" for p in top]
        return f"RESONANCE PEAKS at {', '.join(labels)} — surgically CUT (-2 to -4 dB, Q=4-8)"
    return ""


# ---------------------------------------------------------------------------
# Signal Archetype Classifier
# ---------------------------------------------------------------------------
def classify_signal_archetype(scores: dict, is_tonal: bool) -> tuple[str, str]:
    """Classify signal into archetype + mixing guidance string."""
    punch      = scores.get("transient_punch", 50)
    flux       = scores.get("spectral_flux", 50)
    warmth     = scores.get("harmonic_warmth", None)
    crest      = scores.get("transient_sharpness", 50)
    brightness = scores.get("brightness", 50)

    if not is_tonal and punch > 55:
        return ("PERCUSSIVE",
                "Signal is percussive/atonal (drums, percussion, SFX). "
                "Prioritize: transient shaping, punch, tonal EQ. "
                "Transient Punch % directly maps to compressor attack time. "
                "Spectral Flux is expected to be high — do NOT shorten reverb just because of flux.")

    if is_tonal and flux < 35 and punch < 40:
        return ("SUSTAINED_TONAL",
                "Signal is a sustained tonal source (pad, strings, organ, choir). "
                "Prioritize: frequency balance, warmth/saturation, spatial depth. "
                "Long reverb tails are appropriate. "
                "Transient metrics are NOT meaningful — ignore Punch/Crest for dynamics decisions.")

    if is_tonal and flux > 50 and punch > 35:
        return ("MELODIC_RHYTHMIC",
                "Signal is melodic but rhythmic (arpeggiator, strummed guitar, plucked synth). "
                "CRITICAL: Do NOT treat high Punch as 'needs compression'. "
                "The transients ARE the musical content — preserve them. "
                "Use gentle compression (ratio <= 3:1, slow attack >15ms). "
                "Short-to-medium reverb (0.5-2s) is appropriate despite high flux.")

    if is_tonal and 25 <= flux <= 65:
        return ("DYNAMIC_TONAL",
                "Signal is a dynamic tonal source (vocal, lead instrument, solo). "
                "Standard mixing hierarchy applies. "
                "Use Transient Punch for compressor attack, Flux for reverb decay, Warmth for saturation.")

    if not is_tonal and punch < 40 and brightness > 55:
        return ("NOISE_TRANSIENT",
                "Signal is broadband/noise-like (cymbals, hi-hats, noise layers). "
                "Prioritize: high-frequency taming, de-essing/de-harshening. "
                "Harmonic metrics are meaningless. Spectral EQ is your primary tool.")

    return ("MIXED",
            "Signal has mixed characteristics. "
            "Follow standard mixing hierarchy: fix problems first, then enhance.")


# ---------------------------------------------------------------------------
# Dynamics Diagnosis
# ---------------------------------------------------------------------------
def diagnose_dynamics(scores: dict) -> str:
    """Detect if low dynamics are from compression vs natural sustain."""
    punch    = scores.get("transient_punch", 50)
    crest    = scores.get("transient_sharpness", 50)
    headroom = scores.get("headroom", 50)
    loudness = scores.get("loudness", 50)

    if punch < 25 and crest < 25 and loudness > 70:
        return ("OVER-COMPRESSED: Signal shows signs of heavy prior compression "
                "(low crest + low punch + high loudness). Adding more compression "
                "will make it worse. Consider: transient restoration, upward expansion, "
                "or simply EQ/saturation for character.")

    if punch < 25 and crest < 25 and loudness < 40:
        return ("NATURALLY SUSTAINED: Low transients are natural for this signal type. "
                "Compression is unnecessary. Focus on tonal shaping and spatial effects.")

    return ""


# ---------------------------------------------------------------------------
# Continuous metric scoring (0-100% scale)
# ---------------------------------------------------------------------------
def _pct(value: float, lo: float, hi: float) -> int:
    """Linearly map *value* from [lo, hi] -> [0, 100], clamped."""
    if hi == lo:
        return 50
    return int(max(0.0, min(100.0, (value - lo) / (hi - lo) * 100.0)))


def compute_metric_scores(m: dict) -> dict:
    """Return a dict of 0-100 % scores for every available metric."""
    s: dict = {}

    rms  = m.get("rmsDB", -90.0)
    peak = m.get("peakDB", -90.0)

    s["headroom"] = _pct(-peak, 0.0, 20.0)
    s["loudness"] = _pct(rms, -40.0, -6.0)

    crest = m.get("crestFactor", 0.0)
    s["transient_sharpness"] = _pct(crest, 4.0, 20.0)

    zcr = m.get("zcr", 0.0)
    s["brightness"] = _pct(zcr, 0.02, 0.25)

    sub = m.get("subPct", 0.0)
    low = m.get("lowPct", 0.0)
    s["low_end_weight"] = _pct(sub + low, 15.0, 65.0)

    air = m.get("airPct", 0.0)
    s["air_presence"] = _pct(air, 3.0, 25.0)

    md_tser = m.get("mdTserDb")
    if md_tser is not None:
        s["transient_punch"] = _pct(md_tser, -5.0, 8.0)

    spec_flux = m.get("spectralFlux")
    if spec_flux is not None:
        s["spectral_flux"] = _pct(spec_flux, 0.0, 1.0)

    oe_her = m.get("oeHerDb")
    if oe_her is not None and m.get("isTonal", False):
        s["harmonic_warmth"] = _pct(-oe_her, -10.0, 10.0)

    bw_icc = m.get("bwIcc")
    if bw_icc and len(bw_icc) >= 4:
        band_names = ["sub", "low_mid", "hi_mid", "high"]
        for i, name in enumerate(band_names):
            s[f"icc_{name}"] = _pct(bw_icc[i], -1.0, 1.0)

    lr_bal = m.get("lrBalanceDb")
    if lr_bal is not None:
        s["lr_imbalance"] = _pct(abs(lr_bal), 0.0, 12.0)

    return s


def _score_label(pct: int) -> str:
    """Human-readable word for a 0-100 % score."""
    if pct <= 10:
        return "very low"
    if pct <= 30:
        return "low"
    if pct <= 45:
        return "moderate-low"
    if pct <= 55:
        return "moderate"
    if pct <= 70:
        return "moderate-high"
    if pct <= 90:
        return "high"
    return "very high"


# ---------------------------------------------------------------------------
# Audio context builder v2 — Tiered Mixing Hierarchy
# ---------------------------------------------------------------------------
def build_audio_context(m: dict) -> str:
    rms  = m.get("rmsDB", -90.0)
    peak = m.get("peakDB", -90.0)
    crest = m.get("crestFactor", 0.0)
    zcr  = m.get("zcr", 0.0)
    bpm  = m.get("bpm", 120.0)
    sub  = m.get("subPct", 0.0)
    low  = m.get("lowPct", 0.0)
    low_mid = m.get("lowMidPct", 0.0)
    hi_mid  = m.get("highMidPct", 0.0)
    air  = m.get("airPct", 0.0)
    is_stereo = m.get("isStereo", True)
    is_tonal  = m.get("isTonal", False)
    fmt = "STEREO" if is_stereo else "MONO"
    bw_icc = m.get("bwIcc")
    spectrum31 = m.get("spectrum31")

    if rms <= -90.0:
        return "\n\n[AUDIO STATUS: SILENT — no signal detected]"

    scores = compute_metric_scores(m)
    archetype, archetype_guidance = classify_signal_archetype(scores, is_tonal)
    dynamics_note = diagnose_dynamics(scores)

    sections = []

    # TIER 0: Signal Identity
    tier0 = (
        f"[SIGNAL IDENTITY]\n"
        f"  Format: {fmt} | Archetype: {archetype} | BPM: {bpm:.0f}\n"
        f"  {archetype_guidance}"
    )
    if dynamics_note:
        tier0 += f"\n  >> {dynamics_note}"
    sections.append(tier0)

    # TIER 1: CRITICAL
    critical_alerts = []
    if peak > 0.0:
        critical_alerts.append(
            f"CLIPPING (Peak {peak:+.1f} dBFS). "
            f"First plugin MUST be a limiter or gain reduction.")

    if bw_icc and any(v < 0.0 for v in bw_icc[:4]):
        band_labels = ["Sub", "LowMid", "HiMid", "High"]
        anti = [f"{band_labels[i]}={bw_icc[i]:.2f}"
                for i in range(4) if bw_icc[i] < 0.0]
        critical_alerts.append(
            f"ANTI-PHASE ({', '.join(anti)}). "
            f"Insert phase correction BEFORE any other processing.")

    if spectrum31 and len(spectrum31) >= 31:
        reson_str = _detect_resonance_peaks(spectrum31)
        if reson_str:
            critical_alerts.append(reson_str)

    if critical_alerts:
        tier1 = "[TIER 1 — CRITICAL FIXES (must address first)]\n"
        tier1 += "\n".join(f"  ! {a}" for a in critical_alerts)
        sections.append(tier1)

    # TIER 2: STRUCTURAL
    tier2_lines = [
        f"  Headroom:        {scores['headroom']:3d}%  — Peak {peak:+.1f} dBFS",
        f"  Loudness:        {scores['loudness']:3d}%  — RMS {rms:+.1f} dBFS",
        f"  Transient Sharp: {scores['transient_sharpness']:3d}%  — Crest {crest:.1f} dB",
        f"  Low-End Weight:  {scores['low_end_weight']:3d}%  — Sub+Low {sub+low:.0f}%",
        f"  Brightness:      {scores['brightness']:3d}%  — ZCR {zcr:.3f}",
        f"  Air/Presence:    {scores['air_presence']:3d}%  — Air {air:.1f}%",
    ]
    if "transient_punch" in scores:
        md_tser = m.get("mdTserDb", 0.0)
        tier2_lines.append(
            f"  Transient Punch: {scores['transient_punch']:3d}%  — MD-TSER {md_tser:+.1f} dB")
    tier2 = "[TIER 2 — STRUCTURAL (dynamics, frequency, headroom)]\n"
    tier2 += "\n".join(tier2_lines)
    sections.append(tier2)

    # TIER 3: CHARACTER
    tier3_lines = []
    if "spectral_flux" in scores:
        tier3_lines.append(
            f"  Spectral Flux:   {scores['spectral_flux']:3d}%  — raw {m.get('spectralFlux', 0):.3f}")

    oe_her = m.get("oeHerDb")
    if oe_her is not None:
        if is_tonal and "harmonic_warmth" in scores:
            tier3_lines.append(
                f"  Harmonic Warmth: {scores['harmonic_warmth']:3d}%  — OE-HER {oe_her:+.1f} dB (tonal)")
        else:
            tier3_lines.append(
                f"  Harmonic Warmth: N/A (atonal)  — OE-HER {oe_her:+.1f} dB")

    if bw_icc and len(bw_icc) >= 4:
        band_labels = ["Sub", "LowMid", "HiMid", "High"]
        icc_keys = ["sub", "low_mid", "hi_mid", "high"]
        icc_parts = [f"{bl}={scores.get(f'icc_{k}', 50)}%"
                     for bl, k in zip(band_labels, icc_keys)]
        tier3_lines.append(f"  Stereo Coherence: {' | '.join(icc_parts)}")

    lr_balance = m.get("lrBalanceDb")
    if lr_balance is not None and is_stereo:
        if abs(lr_balance) > 2.0:
            direction = "LEFT" if lr_balance > 0 else "RIGHT"
            tier3_lines.append(
                f"  L/R Balance:     {lr_balance:+.1f} dB ({direction}-heavy) — consider pan correction")
        else:
            tier3_lines.append(
                f"  L/R Balance:     {lr_balance:+.1f} dB (centered)")

    if tier3_lines:
        tier3 = "[TIER 3 — CHARACTER (harmonic, spatial, motion)]\n"
        tier3 += "\n".join(tier3_lines)
        sections.append(tier3)

    # TIER 4: DETAIL — 31-band spectrum
    if spectrum31 and len(spectrum31) >= 31:
        logger.debug(f"Received {len(spectrum31)} bands, "
              f"range: [{min(spectrum31):.1f}, {max(spectrum31):.1f}] dB")
        sections.append(build_spectrum_ascii(spectrum31))
    else:
        logger.debug(f"Spectrum31 NOT present in metrics (keys: {list(m.keys())})")

    header = (
        "(Process tiers in order: fix TIER 1 critical issues first, "
        "then address TIER 2 structural balance, "
        "then add TIER 3 character/color. "
        "Scale processing intensity proportionally to the percentage values.)"
    )

    return f"\n\n[LIVE AUDIO ANALYSIS]\n{header}\n\n" + "\n\n".join(sections)
