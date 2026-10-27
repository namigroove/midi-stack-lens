#include "PluginProcessor.h"

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
    layout.add (choice ("colorMode", "色の付け方", { "色 積み順", "色 音名", "色 強さ" }, 0));

    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "degrees", 1 }, juce::String::fromUTF8 ("度数"), true,
        juce::AudioParameterBoolAttributes().withAutomatable (false)));
    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "chordHistory", 1 }, juce::String::fromUTF8 ("コード履歴"), true,
        juce::AudioParameterBoolAttributes().withAutomatable (false)));

    return layout;
}

MidiStackLensProcessor::MidiStackLensProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", createLayout())
{
    for (auto& v : heldVel)
        v.store (0);
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

void MidiStackLensProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();  // 音は出さない。MIDIはそのまま素通し

    for (const auto meta : midi)
    {
        const auto m = meta.getMessage();

        if (m.isNoteOn())
        {
            const auto n = (size_t) juce::jlimit (0, 127, m.getNoteNumber());
            const auto v = (uint8_t) juce::jlimit (1, 127, (int) m.getVelocity());
            heldVel[n].store (v);
            push ({ (uint8_t) n, v, 1 });
        }
        else if (m.isNoteOff())
        {
            const auto n = (size_t) juce::jlimit (0, 127, m.getNoteNumber());
            heldVel[n].store (0);
            push ({ (uint8_t) n, 0, 0 });
        }
        else if (m.isAllNotesOff() || m.isAllSoundOff())
        {
            for (auto& v : heldVel)
                v.store (0);
            push ({ 0, 0, 2 });
        }
    }

    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
            if (auto b = pos->getBpm())
                if (*b > 20.0 && *b < 400.0)
                    bpm.store (*b);
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
