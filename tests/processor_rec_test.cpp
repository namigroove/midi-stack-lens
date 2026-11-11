// プロセッサの録音処理(再生連動・開始/停止・サンプル位置)を、偽のDAWで確認する
#include <juce_audio_utils/juce_audio_utils.h>

#include <cmath>
#include <cstdio>

#include "PluginProcessor.h"

static int failures = 0;
#define CHECK(c) do { if (! (c)) { std::printf ("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

struct FakeHost : juce::AudioPlayHead
{
    bool playing = false;
    double ppq = 0.0;
    double bpm = 120.0;
    bool hasPosition = true;

    juce::Optional<PositionInfo> getPosition() const override
    {
        if (! hasPosition)
            return {};
        PositionInfo p;
        p.setBpm (bpm);
        p.setPpqPosition (ppq);
        p.setIsPlaying (playing);
        p.setTimeSignature (juce::AudioPlayHead::TimeSignature { 4, 4 });
        return p;
    }
};

static const int kBlock = 480;  // 48000 Hz で 10ms

static void run (MidiStackLensProcessor& p, FakeHost& host, int blocks, juce::MidiBuffer* first = nullptr)
{
    juce::AudioBuffer<float> buf (2, kBlock);
    for (int i = 0; i < blocks; ++i)
    {
        juce::MidiBuffer m;
        if (i == 0 && first != nullptr)
            m = *first;
        p.processBlock (buf, m);
        if (host.playing)
            host.ppq += (double) kBlock / 48000.0 * host.bpm / 60.0;
    }
}

int main()
{
    juce::ScopedJuceInitialiser_GUI init;

    // ---- 1. 連動オフ(=すぐ録音)、DAWなし ----
    {
        MidiStackLensProcessor p;
        p.setPlayHead (nullptr);
        p.prepareToPlay (48000.0, kBlock);
        FakeHost host;
        host.hasPosition = false;

        p.armRecording();
        CHECK (p.recState.load() == MidiStackLensProcessor::RecArmed);

        juce::MidiBuffer m;
        m.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 120);
        // 録音は最初のブロックで始まる。ホストなしなので待たない
        juce::AudioBuffer<float> buf (2, kBlock);
        p.processBlock (buf, m);
        CHECK (p.recState.load() == MidiStackLensProcessor::RecRunning);

        // 50 ブロック(0.5 秒)後にノートオフ
        host.playing = false;
        for (int i = 1; i < 50; ++i)
        {
            juce::MidiBuffer e;
            p.processBlock (buf, e);
        }
        juce::MidiBuffer off;
        off.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
        p.processBlock (buf, off);
        p.stopRecording();

        const auto r = p.copyRecording();
        CHECK (p.recState.load() == MidiStackLensProcessor::RecIdle);
        CHECK (r.events.size() == 2);
        if (r.events.size() == 2)
        {
            CHECK (std::abs (r.events[0].sec - 120.0 / 48000.0) < 1e-9);     // 0.0025 秒
            CHECK (r.events[0].note == 60 && r.events[0].kind == 1);
            CHECK (std::abs (r.events[1].sec - 50.0 * kBlock / 48000.0) < 1e-9);  // 0.5 秒
            CHECK (r.events[1].kind == 0);
        }
        CHECK (r.startPpq < 0.0);
        CHECK (std::abs (r.duration - 51.0 * kBlock / 48000.0) < 1e-9);
        CHECK (p.hasRecording());
    }

    // ---- 2. 連動オン: 再生が始まるまで待ち、止まったら自動で止まる ----
    {
        MidiStackLensProcessor p;
        FakeHost host;
        p.setPlayHead (&host);
        p.prepareToPlay (48000.0, kBlock);

        host.playing = false;
        host.ppq = 8.0;  // 3 小節目あたり
        p.armRecording();
        run (p, host, 10);
        CHECK (p.recState.load() == MidiStackLensProcessor::RecArmed);  // まだ待機

        host.playing = true;
        juce::MidiBuffer on;
        on.addEvent (juce::MidiMessage::noteOn (1, 64, (juce::uint8) 90), 0);
        run (p, host, 20, &on);
        CHECK (p.recState.load() == MidiStackLensProcessor::RecRunning);

        host.playing = false;  // DAW 停止
        run (p, host, 2);
        CHECK (p.recState.load() == MidiStackLensProcessor::RecIdle);

        const auto r = p.copyRecording();
        CHECK (r.events.size() == 1);
        CHECK (std::abs (r.startPpq - 8.0) < 1e-6);
        CHECK (! r.keys.empty());
        CHECK (std::abs (r.duration - 20.0 * kBlock / 48000.0) < 1e-6);
        // 鳴りっぱなしのノートは、.mid に書くときに最後に止まる
        CHECK (! msl::toMidiFile (r).empty());
    }

    // ---- 3. 連動オフにすると、DAWが止まっていてもすぐ録音 ----
    {
        MidiStackLensProcessor p;
        FakeHost host;
        p.setPlayHead (&host);
        p.prepareToPlay (48000.0, kBlock);
        if (auto* f = p.apvts.getParameter ("recFollow"))
            f->setValueNotifyingHost (0.0f);

        p.armRecording();
        run (p, host, 3);
        CHECK (p.recState.load() == MidiStackLensProcessor::RecRunning);
        run (p, host, 30);  // DAW が止まっていても止まらない
        CHECK (p.recState.load() == MidiStackLensProcessor::RecRunning);
        p.stopRecording();
        CHECK (p.recSeconds.load() > 0.3);
    }

    // ---- 4. 録音の開始時に押さえていた音は時刻 0 のノートオンになる ----
    {
        MidiStackLensProcessor p;
        p.setPlayHead (nullptr);
        p.prepareToPlay (48000.0, kBlock);
        FakeHost host;
        host.hasPosition = false;

        juce::AudioBuffer<float> buf (2, kBlock);
        juce::MidiBuffer on;
        on.addEvent (juce::MidiMessage::noteOn (1, 67, (juce::uint8) 80), 0);
        p.processBlock (buf, on);  // まだ録音していない
        p.armRecording();
        juce::MidiBuffer none;
        p.processBlock (buf, none);
        p.stopRecording();
        const auto r = p.copyRecording();
        CHECK (r.events.size() == 1 && r.events[0].note == 67 && r.events[0].sec == 0.0);
    }

    if (failures == 0)
        std::printf ("processor_rec_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
