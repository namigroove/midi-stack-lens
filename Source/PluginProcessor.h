#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>

class MidiStackLensProcessor : public juce::AudioProcessor
{
public:
    // オーディオスレッド → 画面へ渡すMIDIイベント
    struct MidiEvt
    {
        uint8_t note = 0;
        uint8_t vel = 0;
        uint8_t kind = 0;  // 0 = ノートオフ, 1 = ノートオン, 2 = 全ノートオフ
    };

    MidiStackLensProcessor();
    ~MidiStackLensProcessor() override = default;

    void prepareToPlay (double, int) override {}
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return true; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    bool popEvent (MidiEvt& out);

    juce::AudioProcessorValueTreeState apvts;

    // 今押さえているノートのベロシティ(0 = 離している)。エディタを開いた瞬間の状態復元に使う
    std::array<std::atomic<uint8_t>, 128> heldVel;
    std::atomic<double> bpm { 120.0 };

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void push (MidiEvt e) noexcept;

    static constexpr int kFifoSize = 2048;
    juce::AbstractFifo fifo { kFifoSize };
    std::array<MidiEvt, kFifoSize> fifoBuf;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiStackLensProcessor)
};
