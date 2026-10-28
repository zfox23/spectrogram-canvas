cmake_minimum_required(VERSION 3.25)

project(SpectrogramCanvas VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.15
    GIT_SHALLOW    ON)
FetchContent_MakeAvailable(JUCE)

option(SC_COPY_PLUGIN_AFTER_BUILD "Copy the VST3 into the system VST3 folder after building" OFF)

juce_add_plugin(SpectrogramCanvas
    COMPANY_NAME                "zfox23"
    BUNDLE_ID                   "com.zfox23.spectrogramcanvas"
    PLUGIN_MANUFACTURER_CODE    Zfox
    PLUGIN_CODE                 Spcv
    PRODUCT_NAME                "Spectrogram Canvas"
    FORMATS                     VST3
    IS_SYNTH                    FALSE
    NEEDS_MIDI_INPUT            FALSE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    VST3_CATEGORIES             Fx Filter Analyzer
    COPY_PLUGIN_AFTER_BUILD     ${SC_COPY_PLUGIN_AFTER_BUILD})

# Core sources shared by the plugin and the unit tests.
set(SC_CORE_SOURCES
    src/analysis/DetailRenderer.cpp
    src/analysis/SpectrogramStore.cpp
    src/capture/CaptureBuffer.cpp
    src/dsp/SpectralMaskProcessor.cpp
    src/model/MaskGrid.cpp)

target_sources(SpectrogramCanvas PRIVATE
    ${SC_CORE_SOURCES}
    src/host/HostTransport.cpp
    src/session/Session.cpp
    src/ui/OverviewView.cpp
    src/ui/SpectrogramView.cpp
    src/PluginProcessor.cpp
    src/PluginEditor.cpp)

target_include_directories(SpectrogramCanvas PRIVATE src)

target_compile_definitions(SpectrogramCanvas PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(SpectrogramCanvas
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# ---- Editor snapshot tool (renders the UI to a PNG without a host) -------------
juce_add_console_app(ScSnapshot PRODUCT_NAME "ScSnapshot")
target_sources(ScSnapshot PRIVATE tools/Snapshot.cpp)
get_target_property(SC_PLUGIN_SOURCES SpectrogramCanvas SOURCES)
target_sources(ScSnapshot PRIVATE ${SC_PLUGIN_SOURCES})
target_include_directories(ScSnapshot PRIVATE src)
target_compile_definitions(ScSnapshot PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1
    JucePlugin_Name="Spectrogram Canvas")
target_link_libraries(ScSnapshot
    PRIVATE juce::juce_audio_utils juce::juce_dsp
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

# ---- Unit tests ---------------------------------------------------------------
include(CTest)
if(BUILD_TESTING)
    juce_add_console_app(ScTests PRODUCT_NAME "ScTests")
    target_sources(ScTests PRIVATE ${SC_CORE_SOURCES} tests/TestMain.cpp tests/DspTests.cpp tests/MaskGridTests.cpp tests/CaptureTests.cpp tests/SessionTests.cpp tests/DetailRendererTests.cpp src/session/Session.cpp)
    target_include_directories(ScTests PRIVATE src)
    target_compile_definitions(ScTests PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
    target_link_libraries(ScTests
        PRIVATE juce::juce_audio_processors juce::juce_audio_formats juce::juce_dsp juce::juce_graphics
        PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
    add_test(NAME ScTests COMMAND ScTests)
endif()
