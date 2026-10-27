#include "PluginEditor.h"

#include <algorithm>
#include <iterator>

namespace
{
// ---- 配色(namigroove.com/tools/piano と同じ: ブルー #396cdf、押さえた鍵盤はオレンジ) ----
const juce::Colour kBgTop      { 0xff0e1220 };
const juce::Colour kBgBottom   { 0xff161c30 };
const juce::Colour kBlue       { 0xff396cdf };
const juce::Colour kOrange     { 0xffff9a2e };
const juce::Colour kWhiteKey   { 0xfff4f6fb };
const juce::Colour kBlackKey   { 0xff1a1f2e };
const juce::Colour kInk        { 0xff1b2030 };

juce::Font makeFont (float height, bool bold = false)
{
#if JUCE_MAJOR_VERSION >= 8
    juce::Font f { juce::FontOptions { height } };
#else
    juce::Font f { height };
#endif
    return bold ? f.boldened() : f;
}

bool isBlackKey (int note)
{
    const int pc = note % 12;
    return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
}

// [表記: CDE/ドレミ][臨時記号: ♭/♯][音名 0..11]
const char* kNames[2][2][12] = {
    { { "C", "D♭", "D", "E♭", "E", "F", "G♭", "G", "A♭", "A", "B♭", "B" },
      { "C", "C♯", "D", "D♯", "E", "F", "F♯", "G", "G♯", "A", "A♯", "B" } },
    { { "ド", "レ♭", "レ", "ミ♭", "ミ", "ファ", "ソ♭", "ソ", "ラ♭", "ラ", "シ♭", "シ" },
      { "ド", "ド♯", "レ", "レ♯", "ミ", "ファ", "ファ♯", "ソ", "ソ♯", "ラ", "ラ♯", "シ" } }
};

const juce::uint32 kRankPalette[7] = { 0xff5aa2ff, 0xff4fd1c5, 0xff7bd88f, 0xffe5d85c,
                                       0xffffb347, 0xffff7a59, 0xfff06ba8 };

double nowSeconds() { return juce::Time::getMillisecondCounterHiRes() * 0.001; }
}  // namespace

// =====================================================================================
//  MidiStackView
// =====================================================================================
MidiStackView::MidiStackView (MidiStackLensProcessor& p) : proc (p)
{
    auto& s = proc.apvts;
    pNotation     = s.getRawParameterValue ("notation");
    pAccidental   = s.getRawParameterValue ("accidental");
    pKey          = s.getRawParameterValue ("key");
    pKeyLabels    = s.getRawParameterValue ("keyLabels");
    pRange        = s.getRawParameterValue ("range");
    pWindow       = s.getRawParameterValue ("window");
    pColor        = s.getRawParameterValue ("colorMode");
    pDegrees      = s.getRawParameterValue ("degrees");
    pChordHistory = s.getRawParameterValue ("chordHistory");

    // すでに押さえているノートを引き継ぐ(エディタを開いた瞬間)
    const double now = nowSeconds();
    for (int n = 0; n < 128; ++n)
    {
        const auto v = proc.heldVel[(size_t) n].load();
        if (v > 0)
            apply ({ (uint8_t) n, v, 1 }, now);
    }
    updateChord (now + 1.0);

    setOpaque (true);
    startTimerHz (60);
}

MidiStackView::~MidiStackView() { stopTimer(); }

void MidiStackView::resized()
{
    auto b = getLocalBounds().toFloat();
    const float headerH = 30.0f;
    const float keyH = juce::jlimit (70.0f, 140.0f, b.getHeight() * 0.24f);

    headerRect = b.removeFromTop (headerH);
    keyRect = b.removeFromBottom (keyH);
    rollRect = b;
    rebuildKeys();
}

void MidiStackView::rebuildKeys()
{
    static const int lows[]  = { 21, 36, 36, 48 };
    static const int highs[] = { 108, 96, 84, 84 };

    builtRange = juce::jlimit (0, 3, choiceOf (pRange));
    builtWidth = keyRect.getWidth();
    loNote = lows[builtRange];
    hiNote = highs[builtRange];

    int whites = 0;
    for (int n = loNote; n <= hiNote; ++n)
        if (! isBlackKey (n))
            ++whites;

    const float whiteW = keyRect.getWidth() / (float) juce::jmax (1, whites);
    const float blackW = whiteW * 0.62f;

    int wi = 0;
    for (int n = loNote; n <= hiNote; ++n)
    {
        if (! isBlackKey (n))
        {
            kx[(size_t) n] = keyRect.getX() + (float) wi * whiteW;
            kw[(size_t) n] = whiteW;
            ++wi;
        }
        else
        {
            kx[(size_t) n] = keyRect.getX() + (float) wi * whiteW - blackW * 0.5f;
            kw[(size_t) n] = blackW;
        }
    }
}

double MidiStackView::windowSeconds() const
{
    static const double beats[] = { 2.0, 4.0, 8.0, 16.0 };
    const double spb = 60.0 / juce::jlimit (20.0, 400.0, proc.bpm.load());
    return beats[juce::jlimit (0, 3, choiceOf (pWindow))] * spb;
}

float MidiStackView::yOf (double t, double now) const
{
    return rollRect.getBottom() - (float) ((now - t) / windowSeconds()) * rollRect.getHeight();
}

// ---- 表示名 --------------------------------------------------------------------------
juce::String MidiStackView::noteName (int concertPc) const
{
    const int key = juce::jlimit (0, 11, choiceOf (pKey));
    const int dpc = ((concertPc - key) % 12 + 12) % 12;  // 移調楽器の楽譜どおりに
    return juce::String::fromUTF8 (kNames[juce::jlimit (0, 1, choiceOf (pNotation))]
                                         [juce::jlimit (0, 1, choiceOf (pAccidental))][dpc]);
}

juce::String MidiStackView::noteNameWithOctave (int midiNote) const
{
    const int key = juce::jlimit (0, 11, choiceOf (pKey));
    const int shifted = midiNote + ((12 - key) % 12);
    return noteName (midiNote % 12) + juce::String (shifted / 12 - 2);  // C3 = 60
}

juce::String MidiStackView::chordText (const msl::ChordResult& r) const
{
    auto s = noteName (r.rootPc) + juce::String::fromUTF8 (r.suffix.c_str());
    if (r.isSlash())
        s << "/" << noteName (r.bassPc);
    return s;
}

juce::Colour MidiStackView::colourFor (int note, int rank, int vel) const
{
    switch (juce::jlimit (0, 2, choiceOf (pColor)))
    {
        case 0:  return juce::Colour (kRankPalette[(size_t) rank % 7]);
        case 1:
        {
            const int key = juce::jlimit (0, 11, choiceOf (pKey));
            const int dpc = ((note % 12 - key) % 12 + 12) % 12;
            return juce::Colour::fromHSV ((float) dpc / 12.0f, 0.55f, 0.97f, 1.0f);
        }
        default: return kBlue.interpolatedWith (kOrange, juce::jlimit (0.0f, 1.0f, (float) vel / 127.0f));
    }
}

// ---- MIDI イベント -------------------------------------------------------------------
void MidiStackView::apply (const MidiStackLensProcessor::MidiEvt& e, double now)
{
    auto closeOpen = [this, now] (int note)
    {
        for (auto it = bars.rbegin(); it != bars.rend(); ++it)
            if (it->note == note && it->end < 0.0)
            {
                it->end = now;
                break;
            }
    };

    if (e.kind == 1)
    {
        const int n = juce::jlimit (0, 127, (int) e.note);
        if (held[(size_t) n])
            closeOpen (n);

        int rank = 0;
        for (int i = 0; i < n; ++i)
            if (held[(size_t) i])
                ++rank;

        held[(size_t) n] = e.vel;
        bars.push_back ({ n, (int) e.vel, rank, now, -1.0 });
    }
    else if (e.kind == 0)
    {
        const int n = juce::jlimit (0, 127, (int) e.note);
        held[(size_t) n] = 0;
        closeOpen (n);
    }
    else
    {
        for (auto& b : bars)
            if (b.end < 0.0)
                b.end = now;
        held.fill (0);
    }
}

void MidiStackView::updateChord (double now)
{
    std::vector<int> notes;
    for (int n = 0; n < 128; ++n)
        if (held[(size_t) n])
            notes.push_back (n);

    if (notes != lastNotes)
    {
        lastNotes = notes;
        changedAt = now;
        committed = false;
        cur = msl::detectChord (notes);
    }

    // 押さえ方が 60ms 落ち着いたら履歴に確定(ストラムで1音ずつ増えても履歴が荒れない)
    if (! committed && now - changedAt >= 0.06)
    {
        committed = true;
        if (cur.named)
        {
            const auto name = chordText (cur);
            if (name != lastLabelName)
            {
                labels.push_back ({ name, changedAt });
                lastLabelName = name;
            }
        }
        else if (notes.size() < 2)
        {
            lastLabelName = {};
        }
    }
}

void MidiStackView::timerCallback()
{
    const double now = nowSeconds();

    MidiStackLensProcessor::MidiEvt e;
    while (proc.popEvent (e))
        apply (e, now);

    updateChord (now);

    const double keep = windowSeconds() * 1.1;
    bars.erase (std::remove_if (bars.begin(), bars.end(),
                                [&] (const Bar& b) { return b.end >= 0.0 && now - b.end > keep; }),
                bars.end());
    labels.erase (std::remove_if (labels.begin(), labels.end(),
                                  [&] (const ChordLabel& l) { return now - l.start > keep; }),
                  labels.end());

    if (juce::jlimit (0, 3, choiceOf (pRange)) != builtRange || keyRect.getWidth() != builtWidth)
        rebuildKeys();

    repaint();
}

// ---- 描画 ----------------------------------------------------------------------------
void MidiStackView::paint (juce::Graphics& g)
{
    g.fillAll (kBgTop);
    paintRoll (g, nowSeconds());
    paintKeyboard (g);
    paintHeader (g, headerRect);
}

void MidiStackView::paintHeader (juce::Graphics& g, juce::Rectangle<float> area)
{
    g.setColour (kBgTop);
    g.fillRect (area);

    int count = 0;
    for (auto v : held)
        if (v)
            ++count;

    auto a = area.reduced (10.0f, 0.0f);

    if (count == 0)
    {
        g.setColour (juce::Colours::white.withAlpha (0.28f));
        g.setFont (makeFont (12.0f));
        g.drawText (juce::String::fromUTF8 ("MIDI を流すとここにコード名が出ます"), a,
                    juce::Justification::centredLeft);
        return;
    }

    // コード名(小さめ)
    juce::String name;
    if (cur.named)
        name = chordText (cur);
    else if (count == 1)
        for (int n = 0; n < 128; ++n)
            if (held[(size_t) n])
                name = noteName (n % 12);

    g.setColour (juce::Colours::white);
    g.setFont (makeFont (17.0f, true));
    g.drawText (name.isEmpty() ? juce::String::fromUTF8 ("—") : name, a.removeFromLeft (150.0f),
                juce::Justification::centredLeft);

    // 押さえている音(低い順、オクターブ付き)をもっと小さく
    juce::String notes;
    for (int n = 0; n < 128; ++n)
        if (held[(size_t) n])
            notes << noteNameWithOctave (n) << "  ";

    g.setColour (juce::Colours::white.withAlpha (0.55f));
    g.setFont (makeFont (11.0f));
    g.drawText (notes.trim(), a, juce::Justification::centredLeft);
}

void MidiStackView::paintRoll (juce::Graphics& g, double now)
{
    juce::Graphics::ScopedSaveState saved (g);
    g.reduceClipRegion (rollRect.toNearestInt());

    g.setGradientFill (juce::ColourGradient (kBgTop, 0.0f, rollRect.getY(), kBgBottom, 0.0f,
                                             rollRect.getBottom(), false));
    g.fillRect (rollRect);

    // オクターブ(C)の縦ガイド
    g.setColour (juce::Colours::white.withAlpha (0.06f));
    for (int n = loNote; n <= hiNote; ++n)
        if (n % 12 == 0)
            g.drawVerticalLine ((int) kx[(size_t) n], rollRect.getY(), rollRect.getBottom());

    const double win = windowSeconds();
    const auto top = rollRect.getY();

    for (const auto& b : bars)
    {
        if (b.note < loNote || b.note > hiNote)
            continue;

        const double t1 = b.end < 0.0 ? now : b.end;
        const float yBot = yOf (t1, now);
        const float yTop = juce::jmax (top, yOf (b.start, now));
        if (yBot < top)
            continue;

        const float h = juce::jmax (2.0f, yBot - yTop);
        const bool black = isBlackKey (b.note);
        const float inset = black ? 1.0f : 1.5f;

        const float age = b.end < 0.0 ? 0.0f : (float) ((now - b.end) / win);
        const float fade = juce::jlimit (0.12f, 1.0f, 1.0f - age * 0.85f);
        const float velA = 0.6f + 0.4f * (float) b.vel / 127.0f;

        auto c = colourFor (b.note, b.rank, b.vel).withAlpha (fade * (b.end < 0.0 ? 1.0f : velA));
        g.setColour (c);
        g.fillRoundedRectangle (kx[(size_t) b.note] + inset, yBot - h, kw[(size_t) b.note] - inset * 2.0f,
                                h, 2.0f);
    }

    // コード履歴(小さく)
    if (flagOf (pChordHistory))
    {
        g.setFont (makeFont (11.0f, true));
        for (const auto& l : labels)
        {
            const float y = yOf (l.start, now);
            if (y < top + 6.0f || y > rollRect.getBottom())
                continue;

            const float fade = juce::jlimit (0.2f, 1.0f, 1.0f - (float) ((now - l.start) / win) * 0.8f);
            auto r = juce::Rectangle<float> (rollRect.getX() + 6.0f, y - 8.0f, 78.0f, 16.0f);
            g.setColour (juce::Colours::black.withAlpha (0.35f * fade));
            g.fillRoundedRectangle (r, 4.0f);
            g.setColour (juce::Colours::white.withAlpha (0.9f * fade));
            g.drawText (l.text, r.reduced (5.0f, 0.0f), juce::Justification::centredLeft, true);
        }
    }
}

void MidiStackView::paintKeyboard (juce::Graphics& g)
{
    // 低い順の位置を積み順として使う
    std::array<int, 128> rankOf {};
    int r = 0;
    for (int n = 0; n < 128; ++n)
        if (held[(size_t) n])
            rankOf[(size_t) n] = r++;

    const bool showDeg = flagOf (pDegrees) && cur.hasDegrees && r >= 2;
    const int labelMode = juce::jlimit (0, 2, choiceOf (pKeyLabels));
    const float blackH = keyRect.getHeight() * 0.62f;

    g.setColour (kBlackKey);
    g.fillRect (keyRect);

    // 白鍵
    for (int n = loNote; n <= hiNote; ++n)
    {
        if (isBlackKey (n))
            continue;

        auto k = juce::Rectangle<float> (kx[(size_t) n], keyRect.getY(), kw[(size_t) n], keyRect.getHeight())
                     .reduced (0.5f, 0.0f)
                     .withTrimmedBottom (1.0f);
        const bool on = held[(size_t) n] != 0;

        g.setColour (on ? colourFor (n, rankOf[(size_t) n], held[(size_t) n]) : kWhiteKey);
        g.fillRoundedRectangle (k, 2.0f);

        const int pc = n % 12;
        const bool showName = labelMode == 1 || (labelMode == 0 && pc == 0);
        if (showName && kw[(size_t) n] >= 11.0f)
        {
            g.setColour (kInk.withAlpha (on ? 0.9f : 0.5f));
            g.setFont (makeFont (juce::jlimit (8.0f, 11.0f, kw[(size_t) n] * 0.6f)));
            const auto txt = pc == 0 ? noteNameWithOctave (n) : noteName (pc);
            g.drawText (txt, k.withTrimmedBottom (3.0f), juce::Justification::centredBottom, false);
        }

        if (showDeg && on && ! cur.degree[(size_t) pc].empty())
        {
            g.setColour (kInk);
            g.setFont (makeFont (juce::jlimit (9.0f, 13.0f, kw[(size_t) n] * 0.75f), true));
            g.drawText (juce::String::fromUTF8 (cur.degree[(size_t) pc].c_str()),
                        k.withTop (keyRect.getY() + blackH + 2.0f).withHeight (16.0f),
                        juce::Justification::centred, false);
        }
    }

    // 黒鍵
    for (int n = loNote; n <= hiNote; ++n)
    {
        if (! isBlackKey (n))
            continue;

        auto k = juce::Rectangle<float> (kx[(size_t) n], keyRect.getY(), kw[(size_t) n], blackH);
        const bool on = held[(size_t) n] != 0;

        g.setColour (on ? colourFor (n, rankOf[(size_t) n], held[(size_t) n]) : kBlackKey);
        g.fillRoundedRectangle (k, 2.0f);
        g.setColour (juce::Colours::black.withAlpha (0.6f));
        g.drawRoundedRectangle (k, 2.0f, 0.8f);

        const int pc = n % 12;
        if (labelMode == 1 && kw[(size_t) n] >= 15.0f)
        {
            g.setColour (juce::Colours::white.withAlpha (on ? 0.0f : 0.55f));
            g.setFont (makeFont (8.0f));
            g.drawText (noteName (pc), k.withTrimmedBottom (3.0f), juce::Justification::centredBottom, false);
        }

        if (showDeg && on && ! cur.degree[(size_t) pc].empty())
        {
            g.setColour (kInk);
            g.setFont (makeFont (juce::jlimit (8.0f, 12.0f, kw[(size_t) n] * 0.8f), true));
            g.drawText (juce::String::fromUTF8 (cur.degree[(size_t) pc].c_str()),
                        k.withTrimmedBottom (4.0f), juce::Justification::centredBottom, false);
        }
    }
}

// =====================================================================================
//  MidiStackLensEditor
// =====================================================================================
void MidiStackLensEditor::addCombo (juce::ComboBox& cb, const char* paramId)
{
    if (auto* p = dynamic_cast<juce::AudioParameterChoice*> (proc.apvts.getParameter (paramId)))
        cb.addItemList (p->choices, 1);

    cb.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (cb);
    comboAttachments.push_back (std::make_unique<ComboAttach> (proc.apvts, paramId, cb));
}

MidiStackLensEditor::MidiStackLensEditor (MidiStackLensProcessor& p)
    : AudioProcessorEditor (&p), proc (p), view (p)
{
    lnf.setColourScheme (juce::LookAndFeel_V4::getMidnightColourScheme());
    setLookAndFeel (&lnf);

    addCombo (cbNotation, "notation");
    addCombo (cbAccidental, "accidental");
    addCombo (cbKey, "key");
    addCombo (cbKeyLabels, "keyLabels");
    addCombo (cbRange, "range");
    addCombo (cbWindow, "window");
    addCombo (cbColor, "colorMode");

    tbDegrees.setButtonText (juce::String::fromUTF8 ("度数"));
    tbHistory.setButtonText (juce::String::fromUTF8 ("コード履歴"));
    addAndMakeVisible (tbDegrees);
    addAndMakeVisible (tbHistory);
    degreesAttachment = std::make_unique<ButtonAttach> (proc.apvts, "degrees", tbDegrees);
    historyAttachment = std::make_unique<ButtonAttach> (proc.apvts, "chordHistory", tbHistory);

    addAndMakeVisible (view);

    setResizable (true, true);
    setResizeLimits (560, 300, 2000, 900);
    setSize (900, 420);
}

MidiStackLensEditor::~MidiStackLensEditor() { setLookAndFeel (nullptr); }

void MidiStackLensEditor::paint (juce::Graphics& g) { g.fillAll (juce::Colour (0xff12182a)); }

void MidiStackLensEditor::resized()
{
    auto area = getLocalBounds();
    auto bar = area.removeFromTop (30).reduced (6, 3);
    view.setBounds (area);

    struct Item { juce::Component* c; float weight; };
    const Item items[] = { { &cbNotation, 86 }, { &cbAccidental, 82 }, { &cbKey, 72 },
                           { &cbKeyLabels, 100 }, { &cbRange, 76 },    { &cbWindow, 84 },
                           { &cbColor, 88 },      { &tbDegrees, 56 },  { &tbHistory, 92 } };

    const int gap = 4;
    float total = 0.0f;
    for (auto& i : items)
        total += i.weight;

    const float avail = (float) (bar.getWidth() - gap * ((int) std::size (items) - 1));
    for (auto& i : items)
    {
        const int w = juce::roundToInt (avail * i.weight / total);
        i.c->setBounds (bar.removeFromLeft (w));
        bar.removeFromLeft (gap);
    }
}
