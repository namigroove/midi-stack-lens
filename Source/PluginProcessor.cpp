#include "PluginProcessor.h"

#include <cmath>

#include "PluginEditor.h"

namespace
{
juce::StringArray items (std::initializer_list<const char*> list)
{
    juce::StringArray a;
    for (auto* s : list)
        a.add (juce::String::fromUTF8 (s));
    return a;
}

std::unique_ptr<juce::AudioParameterChoice> choice (const char* id, const char* name,
                                                    std::initializer_list<const char*> list, int def)
{
    return std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { id, 1 }, juce::String::fromUTF8 (name), items (list), def,
        juce::AudioParameterChoiceAttributes().withAutomatable (false));
}
}  // namespace

juce::AudioProcessorValueTreeState::ParameterLayout MidiStackLensProcessor::createLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (choice ("notation", "音名の表記", { "表記 CDE", "表記 ドレミ" }, 0));
    layout.add (choice ("accidental", "臨時記号", { "♭ (G♭)", "♯ (F♯)" }, 0));
    layout.add (choice ("key", "キー(移調楽器)",
                        { "キー C", "キー D♭", "キー D", "キー E♭", "キー E", "キー F",
                          "キー G♭", "キー G", "キー A♭", "キー A", "キー B♭", "キー B" }, 0));
    layout.add (choice ("keyLabels", "鍵盤の音名", { "音名 Cだけ", "音名 全部", "音名 なし" }, 0));
    layout.add (choice ("range", "鍵盤の範囲", { "鍵盤 88", "鍵盤 61", "鍵盤 49", "鍵盤 37" }, 0));
    layout.add (choice ("window", "履歴の長さ", { "履歴 2拍", "履歴 4拍", "履歴 8拍", "履歴 16拍" }, 2));
    layout.add (choice ("colorMode", "色の付け方", { "色 積み順", "色 音名", "色 強さ", "色 スケール" }, 0));
    layout.add (choice ("theme", "テーマ", { "テーマ ライト", "テーマ ダーク" }, 0));

    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "degrees", 1 }, juce::String::fromUTF8 ("度数"), true,
        juce::AudioParameterBoolAttributes().withAutomatable (false)));
    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "chordHistory", 1 }, juce::String::fromUTF8 ("コード履歴"), true,
        juce::AudioParameterBoolAttributes().withAutomatable (false)));

    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "scaleHint", 1 }, juce::String::fromUTF8 ("スケール"), true,
        juce::AudioParameterBoolAttributes().withAutomatable (false)));

    // 録音をDAWの再生・停止に連動させる
    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "recFollow", 1 }, juce::String::fromUTF8 ("録音を再生に連動"), true,
        juce::AudioParameterBoolAttributes().withAutomatable (false)));

    return layout;
}

MidiStackLensProcessor::MidiStackLensProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", createLayout())
{
    for (auto& v : heldVel)
        v.store (0);

    pRecFollow = apvts.getRawParameterValue ("recFollow");
}

bool MidiStackLensProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono();
}

void MidiStackLensProcessor::push (MidiEvt e) noexcept
{
    const auto scope = fifo.write (1);
    if (scope.blockSize1 > 0)
        fifoBuf[(size_t) scope.startIndex1] = e;
    // いっぱいのときは捨てる(画面が止まっているだけなので問題なし)
}

bool MidiStackLensProcessor::popEvent (MidiEvt& out)
{
    const auto scope = fifo.read (1);
    if (scope.blockSize1 <= 0)
        return false;
    out = fifoBuf[(size_t) scope.startIndex1];
    return true;
}

void MidiStackLensProcessor::armRecording()
{
    {
        const juce::SpinLock::ScopedLockType lock (recLock);
        if (recState.load() != RecIdle)
            return;

        // オーディオスレッドで確保しないよう、先に容量を取っておく。
        // 前回の録音は、新しい録音が実際に始まるまで残す(待機をやめたら元に戻る)
        rec.events.reserve (kMaxRecEvents);
        rec.keys.reserve (kMaxRecKeys);
        recFull.store (false);
    }
    recState.store (RecArmed);
}

void MidiStackLensProcessor::stopRecording()
{
    const juce::SpinLock::ScopedLockType lock (recLock);
    const auto st = recState.load();
    if (st == RecRunning)
    {
        rec.duration = (double) recSamples / sampleRate.load();
        recSeconds.store (rec.duration);
    }
    recState.store (RecIdle);
}

msl::Recording MidiStackLensProcessor::copyRecording()
{
    const juce::SpinLock::ScopedLockType lock (recLock);
    return rec;
}

// recLock を持った状態で呼ぶ
void MidiStackLensProcessor::startRecordingLocked (double bpmNow, bool hostPos, double ppqNow)
{
    rec.events.clear();
    rec.keys.clear();
    rec.duration = 0.0;
    rec.startPpq = hostPos ? ppqNow : -1.0;
    rec.bpm = bpmNow;
    rec.num = tsNum.load();
    rec.den = tsDen.load();
    recSamples = 0;
    recLastKeySec = -1.0;
    recHostDriven = hostPos;
    recPrevPlaying = true;
    recSeconds.store (0.0);

    // 録音を始めた時点で押さえている音は、時刻 0 のノートオンとして入れる
    for (int n = 0; n < 128; ++n)
    {
        const auto v = heldVel[(size_t) n].load();
        if (v > 0)
            rec.events.push_back ({ 0.0, ppqNow, (uint8_t) n, v, 1 });
    }
    recState.store (RecRunning);
}

void MidiStackLensProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();  // 音は出さない。MIDIはそのまま素通し

    // ---- 再生位置・テンポ ----
    bool gotPosition = false;
    bool isPlaying = false;
    if (auto* ph = getPlayHead())
    {
        if (auto pos = ph->getPosition())
        {
            if (auto b = pos->getBpm())
                if (*b > 20.0 && *b < 400.0)
                    bpm.store (*b);

            if (auto ts = pos->getTimeSignature())
            {
                if (ts->numerator > 0 && ts->denominator > 0)
                {
                    tsNum.store (ts->numerator);
                    tsDen.store (ts->denominator);
                }
            }

            if (auto q = pos->getPpqPosition())
            {
                ppq.store (*q);
                gotPosition = true;

                if (auto bs = pos->getPpqPositionOfLastBarStart())
                    barStartPpq.store (*bs);
                else
                    barStartPpq.store (-1.0);
            }

            isPlaying = pos->getIsPlaying();
            playing.store (isPlaying);
        }
    }

    if (! gotPosition)
    {
        ppq.store (-1.0);
        barStartPpq.store (-1.0);
        playing.store (false);
        isPlaying = false;
    }

    // ---- 録音の開始・停止の判断 ----
    const double sr = sampleRate.load();
    const double bpmNow = bpm.load();
    const double ppqBlock = gotPosition ? ppq.load() : -1.0;
    const bool follow = pRecFollow != nullptr && pRecFollow->load() > 0.5f;

    const juce::SpinLock::ScopedTryLockType recGuard (recLock);
    bool recording = false;
    if (recGuard.isLocked())
    {
        if (recState.load() == RecArmed && (! follow || ! gotPosition || isPlaying))
            startRecordingLocked (bpmNow, follow && gotPosition, ppqBlock);

        if (recState.load() == RecRunning)
        {
            if (recHostDriven && recPrevPlaying && ! isPlaying)
            {
                // DAWが止まった → 録音も止める
                rec.duration = (double) recSamples / sr;
                recSeconds.store (rec.duration);
                recState.store (RecIdle);
            }
            else
            {
                recording = true;
                recPrevPlaying = isPlaying;

                const double sec = (double) recSamples / sr;
                const bool changed = recLastKeySec < 0.0 || std::abs (recLastKey.bpm - bpmNow) > 0.01
                                     || recLastKey.num != tsNum.load() || recLastKey.den != tsDen.load()
                                     || recLastKey.playing != isPlaying;
                if ((changed || sec - recLastKeySec >= 0.05) && rec.keys.size() < kMaxRecKeys)
                {
                    recLastKey = { sec, ppqBlock, gotPosition ? barStartPpq.load() : -1.0, bpmNow,
                                   tsNum.load(), tsDen.load(), isPlaying };
                    rec.keys.push_back (recLastKey);
                    recLastKeySec = sec;
                }
            }
        }
    }

    auto recordEvent = [&] (int samplePos, uint8_t note, uint8_t vel, uint8_t kind)
    {
        if (! recording)
            return;
        if (rec.events.size() >= kMaxRecEvents)
        {
            rec.duration = (double) (recSamples + samplePos) / sr;
            recSeconds.store (rec.duration);
            recFull.store (true);
            recState.store (RecIdle);
            recording = false;
            return;
        }
        const double sec = (double) (recSamples + samplePos) / sr;
        const double q = ppqBlock >= 0.0 ? ppqBlock + (double) samplePos / sr * bpmNow / 60.0 : -1.0;
        rec.events.push_back ({ sec, q, note, vel, kind });
    };

    for (const auto meta : midi)
    {
        const auto m = meta.getMessage();

        if (m.isNoteOn())
        {
            const auto n = (size_t) juce::jlimit (0, 127, m.getNoteNumber());
            const auto v = (uint8_t) juce::jlimit (1, 127, (int) m.getVelocity());
            heldVel[n].store (v);
            push ({ (uint8_t) n, v, 1 });
            recordEvent (meta.samplePosition, (uint8_t) n, v, 1);
        }
        else if (m.isNoteOff())
        {
            const auto n = (size_t) juce::jlimit (0, 127, m.getNoteNumber());
            heldVel[n].store (0);
            push ({ (uint8_t) n, 0, 0 });
            recordEvent (meta.samplePosition, (uint8_t) n, 0, 0);
        }
        else if (m.isAllNotesOff() || m.isAllSoundOff())
        {
            for (auto& v : heldVel)
                v.store (0);
            push ({ 0, 0, 2 });
            recordEvent (meta.samplePosition, 0, 0, 2);
        }
    }

    if (recording)
    {
        recSamples += buffer.getNumSamples();
        recSeconds.store ((double) recSamples / sr);
    }
}

juce::AudioProcessorEditor* MidiStackLensProcessor::createEditor()
{
    return new MidiStackLensEditor (*this);
}

void MidiStackLensProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, dest);
}

void MidiStackLensProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new MidiStackLensProcessor();
}
