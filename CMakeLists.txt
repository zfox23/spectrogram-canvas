cmake_minimum_required(VERSION 3.25)

project(SpectrogramDrawFilter VERSION 0.1.0 LANGUAGES C CXX)

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

option(SDF_COPY_PLUGIN_AFTER_BUILD "Copy the VST3 into the system VST3 folder after building" OFF)

juce_add_plugin(SpectrogramDrawFilter
    COMPANY_NAME                "zfox23"
    BUNDLE_ID                   "com.zfox23.spectrogramdrawfilter"
    PLUGIN_MANUFACTURER_CODE    Zfox
    PLUGIN_CODE                 Sdft
    PRODUCT_NAME                "Spectrogram Draw Filter"
    FORMATS                     VST3
    IS_SYNTH                    FALSE
    NEEDS_MIDI_INPUT            FALSE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    VST3_CATEGORIES             Fx Filter Analyzer
    COPY_PLUGIN_AFTER_BUILD     ${SDF_COPY_PLUGIN_AFTER_BUILD})

# Core sources shared by the plugin and the unit tests.
set(SDF_CORE_SOURCES
    src/analysis/DetailRenderer.cpp
    src/analysis/SpectrogramStore.cpp
    src/capture/CaptureBuffer.cpp
    src/dsp/SpectralMaskProcessor.cpp
    src/model/MaskGrid.cpp)

target_sources(SpectrogramDrawFilter PRIVATE
    ${SDF_CORE_SOURCES}
    src/host/HostTransport.cpp
    src/session/Session.cpp
    src/ui/OverviewView.cpp
    src/ui/SpectrogramView.cpp
    src/PluginProcessor.cpp
    src/PluginEditor.cpp)

target_include_directories(SpectrogramDrawFilter PRIVATE src)

target_compile_definitions(SpectrogramDrawFilter PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(SpectrogramDrawFilter
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# ---- Editor snapshot tool (renders the UI to a PNG without a host) -------------
juce_add_console_app(SdfSnapshot PRODUCT_NAME "SdfSnapshot")
target_sources(SdfSnapshot PRIVATE tools/Snapshot.cpp)
get_target_property(SDF_PLUGIN_SOURCES SpectrogramDrawFilter SOURCES)
target_sources(SdfSnapshot PRIVATE ${SDF_PLUGIN_SOURCES})
target_include_directories(SdfSnapshot PRIVATE src)
target_compile_definitions(SdfSnapshot PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1
    JucePlugin_Name="Spectrogram Draw Filter")
target_link_libraries(SdfSnapshot
    PRIVATE juce::juce_audio_utils juce::juce_dsp
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

# ---- Unit tests ---------------------------------------------------------------
include(CTest)
if(BUILD_TESTING)
    juce_add_console_app(SdfTests PRODUCT_NAME "SdfTests")
    target_sources(SdfTests PRIVATE ${SDF_CORE_SOURCES} tests/TestMain.cpp tests/DspTests.cpp tests/MaskGridTests.cpp tests/CaptureTests.cpp tests/SessionTests.cpp tests/DetailRendererTests.cpp src/session/Session.cpp)
    target_include_directories(SdfTests PRIVATE src)
    target_compile_definitions(SdfTests PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
    target_link_libraries(SdfTests
        PRIVATE juce::juce_audio_processors juce::juce_audio_formats juce::juce_dsp juce::juce_graphics
        PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
    add_test(NAME SdfTests COMMAND SdfTests)
endif()
