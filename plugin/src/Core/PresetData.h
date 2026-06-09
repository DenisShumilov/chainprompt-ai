#pragma once

#include <JuceHeader.h>
#include <array>
inline constexpr int kMaxPluginsInChain = 16;
inline constexpr int kMaxParamsPerPlugin = 128;
inline constexpr int kAutomationPoolSize = 1024;

struct ParameterSetting {
    int index = -1;
    float value = 0.0f;
};

struct PluginConfig {
    int uid = 0;
    int slotId = 0;   // Stable per-instance identity (monotonic, survives reorder)
    std::array<ParameterSetting, kMaxParamsPerPlugin> params{};
    int numParams = 0;
};

// ── POD-only preset for lock-free FIFO (audio thread safe) ──
// Contains ONLY primitives and fixed arrays. No heap allocations.
// Used exclusively for LLM thread → audio thread parameter transfer.
struct ParamPreset {
    std::array<PluginConfig, kMaxPluginsInChain> plugins{};
    int numPlugins = 0;
};

// ── Full preset (message thread only) ──
// Contains juce::String fields — NEVER pass through lock-free FIFO.
// Used for DAW state save/load, topology rebuilds, and UI display.
struct ChainPreset {
    // Main bus (serial chain)
    std::array<PluginConfig, kMaxPluginsInChain> plugins{};
    int numPlugins = 0;

    // Native VST3 state snapshots (Base64-encoded).
    // Used ONLY on the message thread for DAW save/load — never touches audio thread.
    std::array<juce::String, kMaxPluginsInChain> pluginStateBase64{};

    // Per-plugin bypass state (message thread only, saved to DAW)
    std::array<bool, kMaxPluginsInChain> bypassed{};

    // Per-slot user-intent tracking (message thread only, saved to DAW)
    // true = user manually modified this slot since last AI action
    std::array<bool, kMaxPluginsInChain> userTouched{};

    // Per-slot user-ownership tracking (message thread only, saved to DAW)
    // true = user manually added this plugin via "+" button.
    // userOwned slots: AI must KEEP plugin in chain, but MAY configure params.
    // userTouched slots: AI must KEEP plugin AND preserve params.
    std::array<bool, kMaxPluginsInChain> userOwned{};

    // Per-slot action from server response (transient — NOT saved to DAW state).
    // Consumed during apply, then cleared. Default 0 = keep (no action needed).
    // Values: 0=keep, 1=insert, 2=bypass, 3=enable, 4=reconfigure
    std::array<uint8_t, kMaxPluginsInChain> slotAction{};

    // Convert to POD-only ParamPreset for FIFO transfer (main chain only)
    ParamPreset toParamPreset() const
    {
        ParamPreset pp;
        pp.numPlugins = numPlugins;
        pp.plugins = plugins;
        return pp;
    }
};
