cmake_minimum_required(VERSION 3.24)
project(ChainPromptAI VERSION 0.3.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Canonical repo data directory (dev builds resolve relative to repo root)
get_filename_component(_REPO_ROOT "${CMAKE_SOURCE_DIR}" DIRECTORY)
set(CHAINPROMPT_DATA_DIR "${_REPO_ROOT}/data")

# Fetch JUCE via FetchContent
include(FetchContent)
FetchContent_Declare(
    juce
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.12
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(juce)

# Declare audio plugin target
juce_add_plugin(ChainPromptAI
    COMPANY_NAME "Shtokman"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    FORMATS VST3 AU Standalone
    PRODUCT_NAME "ChainPrompt AI"
    VST3_AUTO_MANIFEST FALSE
    VST_NUM_MIDI_INS 0
    VST_NUM_MIDI_OUTS 0
)

# Compile definitions
target_compile_definitions(ChainPromptAI PUBLIC
    JUCE_FORCE_USE_LEGACY_PARAM_IDS=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_PLUGINHOST_VST3=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    CHAINPROMPT_DATA_DIR="${CHAINPROMPT_DATA_DIR}"
)

# Generate JuceHeader.h
juce_generate_juce_header(ChainPromptAI)

# Target sources
target_sources(ChainPromptAI PRIVATE
    src/Core/PresetData.h
    src/Core/AudioMetrics.h
    src/Core/DynamicSlotParameter.h
    src/Core/SlotStateOps.h
    src/Core/LLMCoordinator.h
    src/Core/LLMCoordinator.cpp
    src/Core/AudioCaptureManager.h
    src/Core/AudioCaptureManager.cpp
    src/Core/OfflineAudioAnalyzer.h
    src/Core/OfflineAudioAnalyzer.cpp
    src/Core/PerceptualAutoGain.h
    src/Core/PerceptualAutoGain.cpp
    src/Core/ChainStateSerializer.h
    src/Core/AIIntentTracker.h
    src/Core/StudioTelemetryBridge.h
    src/Core/BackendHealthChecker.h
    src/Core/AutomationPoolRouter.h
    src/Core/ChainGraphRebuilder.h
    src/Core/ChainGraphRebuilder.cpp
    src/Core/ChainPromptAudioProcessor.cpp
    src/GUI/ChainPromptLookAndFeel.h
    src/GUI/SidebarPresenter.h
    src/GUI/SidebarPresenter.cpp
    src/GUI/RescanWorkflowController.h
    src/GUI/RescanWorkflowController.cpp
    src/GUI/HostedEditorController.h
    src/GUI/UIRefreshController.h
    src/GUI/ChainPromptAudioProcessorEditor.cpp
)

# Include directories
target_include_directories(ChainPromptAI PRIVATE src/Core src/GUI src)

# Link modules
target_link_libraries(ChainPromptAI PRIVATE
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_audio_formats
    juce::juce_core
    juce::juce_events
    juce::juce_data_structures
    juce::juce_gui_basics
    juce::juce_gui_extra
    juce::juce_dsp
    juce::juce_audio_devices
    juce::juce_audio_plugin_client
)

# Platform specific configurations
if(APPLE)
    target_compile_options(ChainPromptAI PRIVATE -Werror=vla -Werror=return-type)
elseif(MSVC)
    target_compile_options(ChainPromptAI PRIVATE /W4 /WX-)
endif()

# ─── WavesScanner: 1001-point VST3 parameter metadata dumper ─────────────────
juce_add_console_app(WavesScanner PRODUCT_NAME "WavesScanner")

target_compile_definitions(WavesScanner PRIVATE
    JUCE_PLUGINHOST_VST3=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    CHAINPROMPT_DATA_DIR="${CHAINPROMPT_DATA_DIR}"
)

juce_generate_juce_header(WavesScanner)

target_sources(WavesScanner PRIVATE
    src/Scanner/WavesScanner.cpp
)

target_link_libraries(WavesScanner PRIVATE
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_audio_formats
    juce::juce_core
    juce::juce_events
    juce::juce_data_structures
    juce::juce_gui_basics
    juce::juce_gui_extra
    juce::juce_audio_devices
)

if(MSVC)
    target_compile_options(WavesScanner PRIVATE /W3 /WX-)
endif()
