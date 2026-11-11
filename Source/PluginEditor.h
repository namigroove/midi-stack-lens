#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include <array>
#include <functional>
#include <memory>
#include <vector>

#include "Analysis.h"
#include "ChordDetector.h"
#include "PluginProcessor.h"
#include "Recording.h"

// パネルを並べた分析画面。
//   A  STACK / ROLL      履歴ロール + 鍵盤
//   B  CHORD / VOICING   コード名、積み方(音と音の間隔)、型、転回
//   C  HARMONY / CIRCLE  五度圏、音名の出現量、推定キー
//   D  PROGRESSION       コード進行の履歴(クリックでコピー)
//   E  VELOCITY          押さえた音ごとの強さのバランス
class VideoExporter;

class MidiStackView : public juce::Component, private juce::Timer
{
public:
    // offline = true: 動画の書き出し用。タイマーもライブのMIDIも使わず、仮想の時計で動く
    explicit MidiStackView (MidiStackLensProcessor&, bool offline = false);
    ~MidiStackView() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;

    bool isDark() const { return dark; }
    juce::Colour backgroundColour() const { return theme.bgTop; }

    std::function<void (bool dark)> onThemeChanged;
    std::function<void()> onMidiClick, onVideoClick;

    // ---- 動画の書き出し(offline = true のとき) ----
    void offlineBegin (const msl::Recording&);     // setSize の後に呼ぶ。状態を空にして先頭へ
    void offlineFrame (double t, double dt);       // 時刻 t までのイベントを反映して、表示を t にする

private:
    struct Bar
    {
        int note;
        int vel;
        int rank;      // 押したとき、自分より低く押さえていた音の数(積み順)
        double start;  // 秒
        double end;    // 秒。押している間は -1
    };

    struct ChordLabel
    {
        juce::String text;
        double start;
    };

    struct ProgItem
    {
        msl::ChordResult chord;
        juce::String name;
        double start;
        double end;  // 鳴っている間は -1
    };

    // 押さえている音のスナップショット(離したあとも直前の形を薄く残す)
    struct Snapshot
    {
        std::vector<int> notes;  // 低い順
        std::vector<int> vels;
        msl::ChordResult chord;
        msl::VoicingInfo voicing;
    };

    struct Theme
    {
        juce::Colour bgTop, bgBottom, grid;
        juce::Colour panel, panelShadow, border;
        juce::Colour ink, dim, faint;
        juce::Colour accent, accentSoft, warn;
        juce::Colour whiteKey, blackKey, keyLine, keyInk;
        juce::Colour rollTop, rollBottom, rollLine;
        juce::Colour rank[7];
    };

    struct Transport
    {
        double ppq = -1.0;
        double barStart = -1.0;
        double bpm = 120.0;
        int num = 4;
        int den = 4;
        bool playing = false;
    };

    void timerCallback() override;
    void stepModel (double now, double dt);
    Transport transport() const;
    double bpmNow() const;
    double clockNow() const;
    void paintRecChips (juce::Graphics&);
    void apply (const MidiStackLensProcessor::MidiEvt&, double now);
    void updateChord (double now);
    void updateKey (double now, double dt);
    void rebuildKeys();
    void applyTheme (bool wantDark);
    void clearHistory();
    void runDemo (double now);
    void copyProgression();

    int choiceOf (std::atomic<float>* p) const { return (int) p->load(); }
    bool flagOf (std::atomic<float>* p) const { return p->load() > 0.5f; }
    double secondsPerBeat() const;
    double windowSeconds() const;
    float yOf (double t, double now) const;

    juce::String noteName (int concertPc) const;
    juce::String noteNameWithOctave (int midiNote) const;
    juce::String chordText (const msl::ChordResult&) const;
    juce::String romanText (const msl::ChordResult&) const;
    juce::String keyText() const;
    juce::Colour colourFor (int note, int rank, int vel) const;
    juce::Colour functionColour (const juce::String& fn) const;

    juce::Rectangle<float> paintPanel (juce::Graphics&, juce::Rectangle<float>, char letter,
                                       const juce::String& title, const juce::String& meta);
    void paintChip (juce::Graphics&, juce::Rectangle<float>, const juce::String& text, juce::Colour fill,
                    juce::Colour textColour, bool outlineOnly = false);

    void paintBackground (juce::Graphics&);
    void paintTitle (juce::Graphics&);
    void paintRoll (juce::Graphics&, double now);
    void paintKeyboard (juce::Graphics&);
    void paintChordPanel (juce::Graphics&);
    void paintHarmonyPanel (juce::Graphics&);
    void paintProgressionPanel (juce::Graphics&, double now);
    void paintVelocityPanel (juce::Graphics&);

    MidiStackLensProcessor& proc;
    const bool offline;
    const msl::Recording* offRec = nullptr;
    size_t offIdx = 0;
    double offNow = 0.0;
    msl::TransportKey offTp;

    std::atomic<float>* pNotation;
    std::atomic<float>* pAccidental;
    std::atomic<float>* pKey;
    std::atomic<float>* pKeyLabels;
    std::atomic<float>* pRange;
    std::atomic<float>* pWindow;
    std::atomic<float>* pColor;
    std::atomic<float>* pDegrees;
    std::atomic<float>* pChordHistory;
    std::atomic<float>* pTheme;
    std::atomic<float>* pScaleHint;
    std::atomic<float>* pRecFollow;

    Theme theme;
    bool dark = false;

    std::vector<Bar> bars;
    std::vector<ChordLabel> labels;
    std::vector<ProgItem> progression;
    std::array<uint8_t, 128> held {};
    std::array<double, 128> heldSince {};

    std::vector<int> lastNotes;
    double changedAt = 0.0;
    bool committed = true;
    msl::ChordResult cur;
    juce::String lastLabelName;

    Snapshot snap;
    bool snapLive = false;

    // キー推定
    std::array<double, 12> pcWeight {};
    msl::KeyEstimate key;
    msl::KeyEstimate keyCandidate;
    double keyCandidateSince = 0.0;
    double lastKeyCheck = 0.0;
    double lastTick = 0.0;

    double copiedAt = -10.0;

    // 環境変数 MSL_DEMO=1 で起動すると、MIDIなしでコードを順に鳴らした状態を表示する(画面確認用)
    bool demo = false;
    double demoStart = 0.0;
    int demoStep = -1;

    // レイアウト
    juce::Rectangle<float> titleRect, panelA, panelB, panelC, panelD, panelE;
    juce::Rectangle<float> rollRect, keyRect;
    juce::Rectangle<float> copyChip, clearChip;
    juce::Rectangle<float> recChip, followChip, midChip, mp4Chip;
    std::array<float, 128> kx {}, kw {};
    int loNote = 21, hiNote = 108;
    int builtRange = -1;
    float builtWidth = -1.0f;
    float builtX = -1.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiStackView)
};

class MidiStackLensEditor : public juce::AudioProcessorEditor
{
public:
    explicit MidiStackLensEditor (MidiStackLensProcessor&);
    ~MidiStackLensEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    using ComboAttach = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    using ButtonAttach = juce::AudioProcessorValueTreeState::ButtonAttachment;

    void addCombo (juce::ComboBox&, const char* paramId);
    void applyTheme (bool dark);

    // ---- 書き出し(.mid / mp4) ----
    struct VideoRequest;
    void exportMidi();
    void exportVideo();
    void withFfmpeg (std::function<void (juce::File)> then);
    void askWavThenOutput (std::shared_ptr<VideoRequest>);
    void askOutput (std::shared_ptr<VideoRequest>);
    void runExport (std::shared_ptr<VideoRequest>);
    void showResult (bool ok, const juce::String& message);

    MidiStackLensProcessor& proc;
    juce::LookAndFeel_V4 lnf;

    juce::ComboBox cbNotation, cbAccidental, cbKey, cbKeyLabels, cbRange, cbWindow, cbColor, cbTheme;
    juce::ToggleButton tbDegrees, tbHistory, tbScale;
    std::vector<std::unique_ptr<ComboAttach>> comboAttachments;
    std::unique_ptr<ButtonAttach> degreesAttachment, historyAttachment, scaleAttachment;

    MidiStackView view;

    std::unique_ptr<juce::FileChooser> chooser;
    std::unique_ptr<VideoExporter> exporter;
    std::unique_ptr<juce::AlertWindow> progressWindow;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiStackLensEditor)
};
