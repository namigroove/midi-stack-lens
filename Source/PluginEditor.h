#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include <array>
#include <memory>
#include <vector>

#include "ChordDetector.h"
#include "PluginProcessor.h"

// 履歴ロール + 鍵盤 + 小さなコード表示をまとめて描く部品
class MidiStackView : public juce::Component, private juce::Timer
{
public:
    explicit MidiStackView (MidiStackLensProcessor&);
    ~MidiStackView() override;

    void paint (juce::Graphics&) override;
    void resized() override;

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

    void timerCallback() override;
    void apply (const MidiStackLensProcessor::MidiEvt&, double now);
    void updateChord (double now);
    void rebuildKeys();

    int choiceOf (std::atomic<float>* p) const { return (int) p->load(); }
    bool flagOf (std::atomic<float>* p) const { return p->load() > 0.5f; }
    double windowSeconds() const;
    float yOf (double t, double now) const;

    juce::String noteName (int concertPc) const;
    juce::String noteNameWithOctave (int midiNote) const;
    juce::String chordText (const msl::ChordResult&) const;
    juce::Colour colourFor (int note, int rank, int vel) const;

    void paintHeader (juce::Graphics&, juce::Rectangle<float>);
    void paintRoll (juce::Graphics&, double now);
    void paintKeyboard (juce::Graphics&);

    MidiStackLensProcessor& proc;

    std::atomic<float>* pNotation;
    std::atomic<float>* pAccidental;
    std::atomic<float>* pKey;
    std::atomic<float>* pKeyLabels;
    std::atomic<float>* pRange;
    std::atomic<float>* pWindow;
    std::atomic<float>* pColor;
    std::atomic<float>* pDegrees;
    std::atomic<float>* pChordHistory;

    std::vector<Bar> bars;
    std::vector<ChordLabel> labels;
    std::array<uint8_t, 128> held {};

    std::vector<int> lastNotes;
    double changedAt = 0.0;
    bool committed = true;
    msl::ChordResult cur;
    juce::String lastLabelName;

    juce::Rectangle<float> headerRect, rollRect, keyRect;
    std::array<float, 128> kx {}, kw {};
    int loNote = 21, hiNote = 108;
    int builtRange = -1;
    float builtWidth = -1.0f;

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

    MidiStackLensProcessor& proc;
    juce::LookAndFeel_V4 lnf;

    juce::ComboBox cbNotation, cbAccidental, cbKey, cbKeyLabels, cbRange, cbWindow, cbColor;
    juce::ToggleButton tbDegrees, tbHistory;
    std::vector<std::unique_ptr<ComboAttach>> comboAttachments;
    std::unique_ptr<ButtonAttach> degreesAttachment, historyAttachment;

    MidiStackView view;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiStackLensEditor)
};
