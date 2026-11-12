cmake_minimum_required(VERSION 3.22)
project(MidiStackLens VERSION 0.3.0 LANGUAGES C CXX)

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

# 画面のフォント(マルミーニャ)を実行ファイルに埋め込む
juce_add_binary_data(MslFonts SOURCES assets/x12y12pxMaruMinya.ttf)
set_target_properties(MslFonts PROPERTIES POSITION_INDEPENDENT_CODE TRUE)

target_sources(MidiStackLens PRIVATE
    Source/ChordDetector.cpp
    Source/Analysis.cpp
    Source/Recording.cpp
    Source/VideoExport.cpp
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
        MslFonts
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# コード判定・ボイシング分析の単体テスト: ctest で実行
enable_testing()
add_executable(chord_test tests/chord_test.cpp Source/ChordDetector.cpp)
target_include_directories(chord_test PRIVATE Source)
target_compile_options(chord_test PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_test(NAME chord_test COMMAND chord_test)

add_executable(analysis_test tests/analysis_test.cpp Source/Analysis.cpp Source/ChordDetector.cpp)
target_include_directories(analysis_test PRIVATE Source)
target_compile_options(analysis_test PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_test(NAME analysis_test COMMAND analysis_test)

add_executable(recording_test tests/recording_test.cpp Source/Recording.cpp)
target_include_directories(recording_test PRIVATE Source)
target_compile_options(recording_test PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_test(NAME recording_test COMMAND recording_test)

# 動画書き出しの動作確認用(画面なしで、デモの演奏を mp4 にする)。普段のビルドには入れない:
#   cmake -DMSL_BUILD_EXPORT_DEMO=ON ...  →  MslExportDemo <out.mp4> [land|portrait|square] [fps] [wav]
option(MSL_BUILD_EXPORT_DEMO "Build the headless video export demo" OFF)
if(MSL_BUILD_EXPORT_DEMO)
    juce_add_console_app(MslExportDemo PRODUCT_NAME "MslExportDemo")
    target_sources(MslExportDemo PRIVATE
        tests/export_demo.cpp
        Source/ChordDetector.cpp
        Source/Analysis.cpp
        Source/Recording.cpp
        Source/VideoExport.cpp
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp)
    target_include_directories(MslExportDemo PRIVATE Source)
    target_compile_definitions(MslExportDemo PRIVATE
        JucePlugin_Name="MIDI Stack Lens"
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_DISPLAY_SPLASH_SCREEN=0)
    target_link_libraries(MslExportDemo
        PRIVATE MslFonts juce::juce_audio_utils
        PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

    # 録音処理(再生連動・開始/停止・サンプル位置)の確認。ctest でも動く
    juce_add_console_app(MslProcessorRecTest PRODUCT_NAME "MslProcessorRecTest")
    target_sources(MslProcessorRecTest PRIVATE
        tests/processor_rec_test.cpp
        Source/ChordDetector.cpp
        Source/Analysis.cpp
        Source/Recording.cpp
        Source/VideoExport.cpp
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp)
    target_include_directories(MslProcessorRecTest PRIVATE Source)
    target_compile_definitions(MslProcessorRecTest PRIVATE
        JucePlugin_Name="MIDI Stack Lens"
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_DISPLAY_SPLASH_SCREEN=0)
    target_link_libraries(MslProcessorRecTest
        PRIVATE MslFonts juce::juce_audio_utils
        PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
    add_test(NAME processor_rec_test COMMAND MslProcessorRecTest)
endif()
