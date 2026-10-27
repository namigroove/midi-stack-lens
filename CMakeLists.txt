cmake_minimum_required(VERSION 3.22)
project(MidiStackLens VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "")
set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING "")

include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.16-security
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

# AU(Mac)も欲しいときは: cmake -DMSL_BUILD_AU=ON
option(MSL_BUILD_AU "Build Audio Unit (macOS only)" OFF)
set(MSL_FORMATS VST3 Standalone)
if(APPLE AND MSL_BUILD_AU)
    list(APPEND MSL_FORMATS AU)
endif()

juce_add_plugin(MidiStackLens
    COMPANY_NAME "namigroove"
    PRODUCT_NAME "MIDI Stack Lens"
    PLUGIN_MANUFACTURER_CODE Nmgv
    PLUGIN_CODE Msl1
    FORMATS ${MSL_FORMATS}
    # 音は出さず、来たMIDIをそのまま素通しする。
    # 「インストゥルメント」扱いにしているのは、Ableton Live や FL Studio など
    # MIDI を直接受けられる DAW が多いため(エフェクト扱いだと MIDI が届かない DAW がある)。
    IS_SYNTH TRUE
    IS_MIDI_EFFECT FALSE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT TRUE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    VST3_CATEGORIES Instrument Tools
    COPY_PLUGIN_AFTER_BUILD FALSE)

target_sources(MidiStackLens PRIVATE
    Source/ChordDetector.cpp
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_compile_definitions(MidiStackLens PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

# ソース内の日本語・♭♯をUTF-8として扱う
target_compile_options(MidiStackLens PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

target_link_libraries(MidiStackLens
    PRIVATE
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# コード判定だけの単体テスト: ctest で実行
enable_testing()
add_executable(chord_test tests/chord_test.cpp Source/ChordDetector.cpp)
target_include_directories(chord_test PRIVATE Source)
target_compile_options(chord_test PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_test(NAME chord_test COMMAND chord_test)
