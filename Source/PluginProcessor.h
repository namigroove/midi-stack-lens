#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>

#include "Recording.h"

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

    void prepareToPlay (double sr, int) override { if (sr > 0.0) sampleRate.store (sr); }
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

    // ---- 録音(MIDIを時刻つきで記録。.mid や動画の書き出しに使う) ----
    enum RecState { RecIdle = 0, RecArmed = 1, RecRunning = 2 };
    std::atomic<int> recState { RecIdle };
    std::atomic<double> recSeconds { 0.0 };  // 録音中・録音済みの長さ
    std::atomic<bool> recFull { false };     // 記録の上限に達して止まった

    // 録音ボタン: 「再生に連動」がオンでDAWが止まっているときは、再生が始まるまで待機する
    void armRecording();
    void stopRecording();
    bool hasRecording() const { return recSeconds.load() > 0.0 && recState.load() == RecIdle; }
    msl::Recording copyRecording();  // 録音していないときに呼ぶ

    juce::AudioProcessorValueTreeState apvts;

    // 今押さえているノートのベロシティ(0 = 離している)。エディタを開いた瞬間の状態復元に使う
    std::array<std::atomic<uint8_t>, 128> heldVel;
    std::atomic<double> bpm { 120.0 };

    // DAW の再生位置(取れないときは ppq = -1)
    std::atomic<double> ppq { -1.0 };
    std::atomic<double> barStartPpq { -1.0 };
    std::atomic<int> tsNum { 4 };
    std::atomic<int> tsDen { 4 };
    std::atomic<bool> playing { false };

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void startRecordingLocked (double bpmNow, bool hostPos, double ppqNow);
    void push (MidiEvt e) noexcept;

    static constexpr int kFifoSize = 2048;
    juce::AbstractFifo fifo { kFifoSize };
    std::array<MidiEvt, kFifoSize> fifoBuf;

    std::atomic<double> sampleRate { 44100.0 };
    std::atomic<float>* pRecFollow = nullptr;

    juce::SpinLock recLock;  // オーディオスレッドは tryLock のみ(待たない)
    msl::Recording rec;
    juce::int64 recSamples = 0;
    bool recHostDriven = false;
    bool recPrevPlaying = false;
    double recLastKeySec = -1.0;
    msl::TransportKey recLastKey;

    static constexpr size_t kMaxRecEvents = 600000;
    static constexpr size_t kMaxRecKeys = 120000;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiStackLensProcessor)
};
