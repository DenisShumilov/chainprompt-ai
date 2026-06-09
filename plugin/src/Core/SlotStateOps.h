#pragma once

/**
 * SlotStateOps — Slot-state reconciliation helpers (header-only).
 *
 * Extracted from ChainPromptAudioProcessor::performRebuild() to provide
 * a single authoritative path for preserving slot identity and intent
 * flags across topology changes, UI mutations, and undo/redo.
 *
 * All functions operate on POD ChainPreset — no JUCE dependencies,
 * no heap allocations, no state. Pure functions.
 */

#include "PresetData.h"
#include <algorithm>
#include <cstddef>

namespace SlotState
{

// ─────────────────────────────────────────────────────────────────────────────
// copySlotFlags: Copy all intent flags from one preset slot to another.
// Used by Editor UI mutations (delete, reorder, add) and rebuild reconciliation.
// ─────────────────────────────────────────────────────────────────────────────
inline void copySlotFlags (const ChainPreset& src, size_t srcIdx,
                           ChainPreset& dst, size_t dstIdx)
{
    dst.bypassed[dstIdx]    = src.bypassed[srcIdx];
    dst.userTouched[dstIdx] = src.userTouched[srcIdx];
    dst.userOwned[dstIdx]   = src.userOwned[srcIdx];
}

// ─────────────────────────────────────────────────────────────────────────────
// copySlotIdentity: Copy UID + slotId + flags (everything except params/state).
// ─────────────────────────────────────────────────────────────────────────────
inline void copySlotIdentity (const ChainPreset& src, size_t srcIdx,
                              ChainPreset& dst, size_t dstIdx)
{
    dst.plugins[dstIdx].uid    = src.plugins[srcIdx].uid;
    dst.plugins[dstIdx].slotId = src.plugins[srcIdx].slotId;
    dst.plugins[dstIdx].numParams = 0;  // preserve existing params (don't overwrite)
    copySlotFlags (src, srcIdx, dst, dstIdx);
}

// ─────────────────────────────────────────────────────────────────────────────
// buildPresetWithout: Build new preset excluding one slot (delete operation).
// Returns the number of plugins in the new preset.
// ─────────────────────────────────────────────────────────────────────────────
inline int buildPresetWithout (const ChainPreset& src, int srcCount,
                               int excludeIdx, ChainPreset& dst)
{
    int writeIdx = 0;
    for (int s = 0; s < srcCount; ++s)
    {
        if (s == excludeIdx)
            continue;
        if (writeIdx >= kMaxPluginsInChain)
            break;

        copySlotIdentity (src, static_cast<size_t> (s), dst, static_cast<size_t> (writeIdx));
        ++writeIdx;
    }
    dst.numPlugins = writeIdx;
    return writeIdx;
}

// ─────────────────────────────────────────────────────────────────────────────
// reconcileFlags: After a topology rebuild, reconcile flags from old preset
// into new preset using recycledOldIndex mapping.
// recycledOldIndex[newIdx] = oldIdx (SIZE_MAX = new plugin, no old match).
// ─────────────────────────────────────────────────────────────────────────────
inline void reconcileFlags (const ChainPreset& oldPreset,
                            ChainPreset& newPreset,
                            const size_t* recycledOldIndex,
                            size_t count)
{
    for (size_t i = 0; i < count && i < static_cast<size_t> (newPreset.numPlugins); ++i)
    {
        if (recycledOldIndex[i] != SIZE_MAX
            && recycledOldIndex[i] < static_cast<size_t> (kMaxPluginsInChain))
        {
            newPreset.bypassed[i]    = oldPreset.bypassed[recycledOldIndex[i]];
            newPreset.userTouched[i] = oldPreset.userTouched[recycledOldIndex[i]];
            newPreset.userOwned[i]   = oldPreset.userOwned[recycledOldIndex[i]];
        }
        else
        {
            // New plugin from AI — not user-touched/owned
            newPreset.userTouched[i] = false;
            newPreset.userOwned[i]   = false;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// clearTrailingFlags: Zero out flags beyond numPlugins (prevents stale data).
// ─────────────────────────────────────────────────────────────────────────────
inline void clearTrailingFlags (ChainPreset& preset)
{
    for (int i = preset.numPlugins; i < kMaxPluginsInChain; ++i)
    {
        auto si = static_cast<size_t> (i);
        preset.bypassed[si]    = false;
        preset.userTouched[si] = false;
        preset.userOwned[si]   = false;
        preset.slotAction[si]  = 0;
        preset.plugins[si]     = {};
    }
}

} // namespace SlotState
