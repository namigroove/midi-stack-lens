#include "PluginEditor.h"

#include "VideoExport.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace
{
// ---- 配色の基準(namigroove.com/tools/piano と同じ: ブルー #396cdf、押さえた鍵盤はオレンジ) ----
const juce::Colour kBlue   { 0xff396cdf };
const juce::Colour kOrange { 0xffff9a2e };

juce::Font uiFont (float height, bool bold = false)
{
#if JUCE_MAJOR_VERSION >= 8
    juce::Font f { juce::FontOptions { height } };
#else
    juce::Font f { height };
#endif
    return bold ? f.boldened() : f;
}

// 見出しや数値用の等幅フォント(英数字だけに使う)
juce::Font monoFont (float height, bool bold = false)
{
    const int style = bold ? juce::Font::bold : juce::Font::plain;
#if JUCE_MAJOR_VERSION >= 8
    return juce::Font { juce::FontOptions { juce::Font::getDefaultMonospacedFontName(), height, style } };
#else
    return juce::Font { juce::Font::getDefaultMonospacedFontName(), height, style };
#endif
}

juce::String u8 (const char* s) { return juce::String::fromUTF8 (s); }
juce::String u8 (const std::string& s) { return juce::String::fromUTF8 (s.c_str()); }

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

const juce::uint32 kRankDark[7]  = { 0xff5aa2ff, 0xff4fd1c5, 0xff7bd88f, 0xffe5d85c,
                                     0xffffb347, 0xffff7a59, 0xfff06ba8 };
const juce::uint32 kRankLight[7] = { 0xff3b63e0, 0xff1c9ec4, 0xff2fa37a, 0xffc79a1e,
                                     0xffe8792f, 0xffe0524d, 0xffc0489b };

double nowSeconds() { return juce::Time::getMillisecondCounterHiRes() * 0.001; }

const float kTwoPi = juce::MathConstants<float>::twoPi;
}  // namespace

// =====================================================================================
//  MidiStackView
// =====================================================================================
MidiStackView::MidiStackView (MidiStackLensProcessor& p, bool isOffline) : proc (p), offline (isOffline)
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
    pTheme        = s.getRawParameterValue ("theme");
    pScaleHint    = s.getRawParameterValue ("scaleHint");
    pRecFollow    = s.getRawParameterValue ("recFollow");

    applyTheme (choiceOf (pTheme) == 1);

    setOpaque (true);
    if (offline)
        return;  // 書き出し用: タイマーも、ライブのMIDIの引き継ぎも使わない

    // すでに押さえているノートを引き継ぐ(エディタを開いた瞬間)
    const double now = nowSeconds();
    lastTick = now;
    for (int n = 0; n < 128; ++n)
    {
        const auto v = proc.heldVel[(size_t) n].load();
        if (v > 0)
            apply ({ (uint8_t) n, v, 1 }, now);
    }
    updateChord (now + 1.0);

    demo = juce::SystemStats::getEnvironmentVariable ("MSL_DEMO", {}) == "1";
    if (demo && juce::SystemStats::getEnvironmentVariable ("MSL_DEMO_THEME", {}) == "dark")
        if (auto* themeParam = proc.apvts.getParameter ("theme"))
            themeParam->setValueNotifyingHost (1.0f);
    demoStart = now + 0.5;

    startTimerHz (60);
}

// ---- 時計と再生位置(ライブ / 書き出しで切り替え) ----------------------------------------
double MidiStackView::clockNow() const { return offline ? offNow : nowSeconds(); }

double MidiStackView::bpmNow() const { return offline ? offTp.bpm : proc.bpm.load(); }

MidiStackView::Transport MidiStackView::transport() const
{
    Transport t;
    if (offline)
    {
        t.ppq = offTp.ppq;
        t.barStart = offTp.barStartPpq;
        t.bpm = offTp.bpm;
        t.num = offTp.num;
        t.den = offTp.den;
        t.playing = offTp.playing;
    }
    else
    {
        t.ppq = proc.ppq.load();
        t.barStart = proc.barStartPpq.load();
        t.bpm = proc.bpm.load();
        t.num = proc.tsNum.load();
        t.den = proc.tsDen.load();
        t.playing = proc.playing.load();
    }
    return t;
}

void MidiStackView::offlineBegin (const msl::Recording& rec)
{
    offRec = &rec;
    offIdx = 0;
    offNow = 0.0;
    offTp = msl::transportAt (rec, 0.0);

    bars.clear();
    labels.clear();
    progression.clear();
    held.fill (0);
    heldSince.fill (0.0);
    lastNotes.clear();
    lastLabelName = {};
    pcWeight.fill (0.0);
    key = {};
    keyCandidate = {};
    keyCandidateSince = 0.0;
    lastKeyCheck = 0.0;
    snap = {};
    snapLive = false;
    committed = true;
    changedAt = 0.0;
    cur = {};

    applyTheme (choiceOf (pTheme) == 1);
    rebuildKeys();
}

void MidiStackView::offlineFrame (double t, double dt)
{
    if (offRec == nullptr)
        return;

    offNow = t;
    offTp = msl::transportAt (*offRec, t);
    const auto& ev = offRec->events;
    while (offIdx < ev.size() && ev[offIdx].sec <= t)
    {
        const auto& e = ev[offIdx++];
        apply ({ e.note, e.vel, e.kind }, e.sec);
    }
    stepModel (t, dt);
}

void MidiStackView::stepModel (double now, double dt)
{
    updateChord (now);
    updateKey (now, dt);

    const double keep = windowSeconds() * 1.1;
    bars.erase (std::remove_if (bars.begin(), bars.end(),
                                [&] (const Bar& b) { return b.end >= 0.0 && now - b.end > keep; }),
                bars.end());
    labels.erase (std::remove_if (labels.begin(), labels.end(),
                                  [&] (const ChordLabel& l) { return now - l.start > keep; }),
                  labels.end());
}

MidiStackView::~MidiStackView() { stopTimer(); }

void MidiStackView::applyTheme (bool wantDark)
{
    dark = wantDark;
    auto& t = theme;

    if (dark)
    {
        t.bgTop       = juce::Colour (0xff0e1220);
        t.bgBottom    = juce::Colour (0xff141a2e);
        t.grid        = juce::Colours::white.withAlpha (0.025f);
        t.panel       = juce::Colour (0xff151b2e);
        t.panelShadow = kBlue.withAlpha (0.35f);
        t.border      = juce::Colour (0xff4a78e6);
        t.ink         = juce::Colour (0xffe9edf7);
        t.dim         = juce::Colour (0xff8a95b2);
        t.faint       = juce::Colours::white.withAlpha (0.08f);
        t.accent      = juce::Colour (0xff5b8cf5);
        t.accentSoft  = juce::Colour (0xff5b8cf5).withAlpha (0.18f);
        t.warn        = kOrange;
        t.whiteKey    = juce::Colour (0xfff4f6fb);
        t.blackKey    = juce::Colour (0xff1a1f2e);
        t.keyLine     = juce::Colour (0xff0b0e18);
        t.keyInk      = juce::Colour (0xff1b2030);
        t.rollTop     = juce::Colour (0xff0f1424);
        t.rollBottom  = juce::Colour (0xff181f36);
        t.rollLine    = juce::Colours::white.withAlpha (0.06f);
        for (int i = 0; i < 7; ++i)
            t.rank[i] = juce::Colour (kRankDark[i]);
    }
    else
    {
        t.bgTop       = juce::Colour (0xffe6ecf5);
        t.bgBottom    = juce::Colour (0xffdbe3f0);
        t.grid        = kBlue.withAlpha (0.045f);
        t.panel       = juce::Colour (0xfff6f8fc);
        t.panelShadow = kBlue.withAlpha (0.55f);
        t.border      = juce::Colour (0xff3d5fd6);
        t.ink         = juce::Colour (0xff1d2433);
        t.dim         = juce::Colour (0xff76829a);
        t.faint       = juce::Colour (0xff1d2433).withAlpha (0.08f);
        t.accent      = kBlue;
        t.accentSoft  = kBlue.withAlpha (0.14f);
        t.warn        = juce::Colour (0xffe8792f);
        t.whiteKey    = juce::Colours::white;
        t.blackKey    = juce::Colour (0xff232a3b);
        t.keyLine     = juce::Colour (0xff232a3b);
        t.keyInk      = juce::Colour (0xff1b2030);
        t.rollTop     = juce::Colour (0xfffafbfe);
        t.rollBottom  = juce::Colour (0xffeef2f9);
        t.rollLine    = juce::Colour (0xff1d2433).withAlpha (0.07f);
        for (int i = 0; i < 7; ++i)
            t.rank[i] = juce::Colour (kRankLight[i]);
    }
}

void MidiStackView::resized()
{
    auto b = getLocalBounds().toFloat().reduced (12.0f, 8.0f);
    const float gap = 10.0f;
    const float totalH = b.getHeight();

    titleRect = b.removeFromTop (52.0f);
    b.removeFromTop (6.0f);

    // タイトルの右に 録音 / 連動 / MIDI / MP4 のチップ
    {
        const float h = 24.0f;
        const float y = titleRect.getCentreY() - 4.0f - h * 0.5f;
        float x = titleRect.getX() + 268.0f;
        recChip    = { x, y, 96.0f, h };  x += 102.0f;
        followChip = { x, y, 74.0f, h };  x += 80.0f;
        midChip    = { x, y, 50.0f, h };  x += 56.0f;
        mp4Chip    = { x, y, 54.0f, h };
    }

    const float rowD = juce::jlimit (104.0f, 150.0f, totalH * 0.165f);
    const float rowB = juce::jlimit (168.0f, 250.0f, totalH * 0.27f);

    auto bottom = b.removeFromBottom (rowD);
    b.removeFromBottom (gap);
    auto middle = b.removeFromBottom (rowB);
    b.removeFromBottom (gap);
    panelA = b;

    panelB = middle.removeFromLeft (std::floor (middle.getWidth() * 0.6f));
    middle.removeFromLeft (gap);
    panelC = middle;

    panelD = bottom.removeFromLeft (std::floor (bottom.getWidth() * 0.6f));
    bottom.removeFromLeft (gap);
    panelE = bottom;

    // パネルAの中身: 見出し(34) + ロール + 鍵盤
    auto inner = panelA.reduced (12.0f, 0.0f).withTrimmedTop (38.0f).withTrimmedBottom (12.0f);
    const float keyH = juce::jlimit (52.0f, 130.0f, inner.getHeight() * 0.3f);
    keyRect = inner.removeFromBottom (keyH);
    rollRect = inner;

    rebuildKeys();
}

void MidiStackView::rebuildKeys()
{
    static const int lows[]  = { 21, 36, 36, 48 };
    static const int highs[] = { 108, 96, 84, 84 };

    builtRange = juce::jlimit (0, 3, choiceOf (pRange));
    builtWidth = keyRect.getWidth();
    builtX = keyRect.getX();
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

double MidiStackView::secondsPerBeat() const
{
    return 60.0 / juce::jlimit (20.0, 400.0, bpmNow());
}

double MidiStackView::windowSeconds() const
{
    static const double beats[] = { 2.0, 4.0, 8.0, 16.0 };
    return beats[juce::jlimit (0, 3, choiceOf (pWindow))] * secondsPerBeat();
}

float MidiStackView::yOf (double t, double now) const
{
    return rollRect.getBottom() - (float) ((now - t) / windowSeconds()) * rollRect.getHeight();
}

// ---- 表示名 --------------------------------------------------------------------------
juce::String MidiStackView::noteName (int concertPc) const
{
    const int k = juce::jlimit (0, 11, choiceOf (pKey));
    const int dpc = ((concertPc - k) % 12 + 12) % 12;  // 移調楽器の楽譜どおりに
    return u8 (kNames[juce::jlimit (0, 1, choiceOf (pNotation))][juce::jlimit (0, 1, choiceOf (pAccidental))][dpc]);
}

juce::String MidiStackView::noteNameWithOctave (int midiNote) const
{
    const int k = juce::jlimit (0, 11, choiceOf (pKey));
    const int shifted = midiNote + ((12 - k) % 12);
    return noteName (midiNote % 12) + juce::String (shifted / 12 - 2);  // C3 = 60
}

juce::String MidiStackView::chordText (const msl::ChordResult& r) const
{
    auto s = noteName (r.rootPc) + u8 (r.suffix);
    if (r.isSlash())
        s << "/" << noteName (r.bassPc);
    return s;
}

juce::String MidiStackView::romanText (const msl::ChordResult& r) const
{
    if (! key.valid || ! r.named)
        return {};
    return u8 (msl::romanNumeral (r, key.tonicPc));
}

juce::String MidiStackView::keyText() const
{
    if (! key.valid)
        return u8 ("—");
    return noteName (key.tonicPc) + (key.minor ? " min" : " maj");
}

juce::Colour MidiStackView::colourFor (int note, int rank, int vel) const
{
    switch (juce::jlimit (0, 3, choiceOf (pColor)))
    {
        case 0:  return theme.rank[(size_t) rank % 7];
        case 1:
        {
            const int k = juce::jlimit (0, 11, choiceOf (pKey));
            const int dpc = ((note % 12 - k) % 12 + 12) % 12;
            return juce::Colour::fromHSV ((float) dpc / 12.0f, dark ? 0.55f : 0.7f, dark ? 0.97f : 0.82f, 1.0f);
        }
        case 2:  return kBlue.interpolatedWith (kOrange, juce::jlimit (0.0f, 1.0f, (float) vel / 127.0f));
        default:
            if (key.valid && ! msl::inScale (note % 12, key.tonicPc, key.minor))
                return theme.warn;
            return theme.accent;
    }
}

juce::Colour MidiStackView::functionColour (const juce::String& fn) const
{
    if (fn == "T")
        return theme.accent;
    if (fn == "SD")
        return juce::Colour (dark ? 0xff4fd1c5 : 0xff1c9ec4);
    if (fn == "D")
        return theme.warn;
    return theme.dim.withAlpha (0.5f);
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
        heldSince[(size_t) n] = now;
        bars.push_back ({ n, (int) e.vel, rank, now, -1.0 });
        pcWeight[(size_t) (n % 12)] += 1.0;
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

        snapLive = ! notes.empty();
        if (snapLive)
        {
            snap.notes = notes;
            snap.vels.clear();
            for (int n : notes)
                snap.vels.push_back ((int) held[(size_t) n]);
            snap.chord = cur;
            snap.voicing = msl::analyzeVoicing (notes, cur);
        }
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

            // 進行: 同じコードを2拍以内に弾き直したときは1つにまとめる
            const bool sameAsLast = ! progression.empty() && progression.back().name == name
                                    && (progression.back().end < 0.0
                                        || changedAt - progression.back().end < secondsPerBeat() * 2.0);
            if (sameAsLast)
            {
                progression.back().end = -1.0;
            }
            else
            {
                if (! progression.empty() && progression.back().end < 0.0)
                    progression.back().end = changedAt;
                progression.push_back ({ cur, name, changedAt, -1.0 });
                if (progression.size() > 64)
                    progression.erase (progression.begin());
            }
        }
        else
        {
            if (notes.size() < 2)
                lastLabelName = {};
            if (! progression.empty() && progression.back().end < 0.0)
                progression.back().end = changedAt;
        }
    }
}

void MidiStackView::updateKey (double now, double dt)
{
    // 押さえ始めの1.5秒だけ少しずつ重みを足す(長い音は少し効くが、伸ばしっぱなしの和音にキーが引っ張られない)。
    // 全体は約30秒で薄れていく
    const double decay = std::exp (-dt / 30.0);
    for (int pc = 0; pc < 12; ++pc)
        pcWeight[(size_t) pc] *= decay;
    for (int n = 0; n < 128; ++n)
        if (held[(size_t) n] && now - heldSince[(size_t) n] < 1.5)
            pcWeight[(size_t) (n % 12)] += dt * 0.5;

    if (now - lastKeyCheck < 0.25)
        return;
    lastKeyCheck = now;

    double total = 0.0;
    for (double w : pcWeight)
        total += w;

    msl::KeyEstimate est;
    if (total >= 5.0)
        est = msl::estimateKey (pcWeight);

    if (! est.valid)
    {
        if (total < 0.5)
            key = {};  // しばらく何も弾いていない
        return;
    }

    const bool sameAsKey = key.valid && est.tonicPc == key.tonicPc && est.minor == key.minor;
    if (sameAsKey)
    {
        key.confidence = est.confidence;
        keyCandidate = {};
        return;
    }

    // 別のキーが1.5秒続けて一番らしいときだけ切り替える(表示がちらつかない)
    const bool sameCandidate = keyCandidate.valid && est.tonicPc == keyCandidate.tonicPc
                               && est.minor == keyCandidate.minor;
    if (! sameCandidate)
    {
        keyCandidate = est;
        keyCandidateSince = now;
    }

    if (! key.valid || now - keyCandidateSince >= 1.5)
    {
        key = est;
        keyCandidate = {};
    }
}

void MidiStackView::clearHistory()
{
    progression.clear();
    labels.clear();
    lastLabelName = {};
    pcWeight.fill (0.0);
    key = {};
    keyCandidate = {};
    bars.erase (std::remove_if (bars.begin(), bars.end(), [] (const Bar& b) { return b.end >= 0.0; }), bars.end());
    if (! snapLive)
        snap = {};
}

void MidiStackView::copyProgression()
{
    juce::StringArray names;
    for (const auto& p : progression)
        names.add (p.name);
    if (names.isEmpty())
        return;

    juce::SystemClipboard::copyTextToClipboard (names.joinIntoString (" | "));
    copiedAt = nowSeconds();
}

void MidiStackView::runDemo (double now)
{
    struct Step { std::vector<int> notes; std::vector<int> vels; };
    static const std::vector<Step> steps = {
        { { 48, 55, 64, 71 },     { 84, 70, 76, 98 } },        // CM7 オープン
        { { 45, 55, 60, 64 },     { 80, 66, 72, 94 } },        // Am7
        { { 50, 60, 64, 65, 69 }, { 86, 64, 70, 72, 100 } },   // Dm9
        { { 47, 50, 55, 65 },     { 82, 68, 70, 96 } },        // G7/B
        { { 55, 60, 64, 71 },     { 78, 70, 74, 104 } },       // CM7 ドロップ2(押さえたまま)
    };

    const int target = juce::jmin ((int) steps.size() - 1, (int) std::floor ((now - demoStart) / 0.6));
    while (demoStep < target)
    {
        if (demoStep >= 0)
            for (int n : steps[(size_t) demoStep].notes)
                apply ({ (uint8_t) n, 0, 0 }, now);

        ++demoStep;
        const auto& st = steps[(size_t) demoStep];
        for (size_t i = 0; i < st.notes.size(); ++i)
            apply ({ (uint8_t) st.notes[i], (uint8_t) st.vels[i], 1 }, now);
    }
}

void MidiStackView::mouseDown (const juce::MouseEvent& e)
{
    if (offline)
        return;

    if (recChip.contains (e.position))
    {
        if (proc.recState.load() == MidiStackLensProcessor::RecIdle)
            proc.armRecording();
        else
            proc.stopRecording();
    }
    else if (followChip.contains (e.position))
    {
        if (auto* p = proc.apvts.getParameter ("recFollow"))
            p->setValueNotifyingHost (pRecFollow->load() > 0.5f ? 0.0f : 1.0f);
    }
    else if (midChip.contains (e.position))
    {
        if (proc.hasRecording() && onMidiClick)
            onMidiClick();
    }
    else if (mp4Chip.contains (e.position))
    {
        if (proc.hasRecording() && onVideoClick)
            onVideoClick();
    }
    else if (copyChip.contains (e.position))
        copyProgression();
    else if (clearChip.contains (e.position))
        clearHistory();
}

void MidiStackView::timerCallback()
{
    const double now = nowSeconds();
    const double dt = juce::jlimit (0.0, 0.25, now - lastTick);
    lastTick = now;

    if (demo)
        runDemo (now);

    MidiStackLensProcessor::MidiEvt e;
    while (proc.popEvent (e))
        apply (e, now);

    stepModel (now, dt);

    if (juce::jlimit (0, 3, choiceOf (pRange)) != builtRange || keyRect.getWidth() != builtWidth
        || keyRect.getX() != builtX)
        rebuildKeys();

    const bool wantDark = choiceOf (pTheme) == 1;
    if (wantDark != dark)
    {
        applyTheme (wantDark);
        if (onThemeChanged)
            onThemeChanged (dark);
    }

    repaint();
}

// ---- 描画の部品 ----------------------------------------------------------------------
juce::Rectangle<float> MidiStackView::paintPanel (juce::Graphics& g, juce::Rectangle<float> r, char letter,
                                                  const juce::String& title, const juce::String& meta)
{
    const float radius = 10.0f;

    // 右下にずらした影の線 → 本体 → 枠
    g.setColour (theme.panelShadow);
    g.fillRoundedRectangle (r.translated (2.5f, 2.5f), radius);
    g.setColour (theme.panel);
    g.fillRoundedRectangle (r, radius);
    g.setColour (theme.border);
    g.drawRoundedRectangle (r.reduced (0.5f), radius, 1.4f);

    // 見出し
    auto head = r.withHeight (32.0f).reduced (12.0f, 0.0f);
    auto badge = juce::Rectangle<float> (head.getX(), head.getCentreY() - 9.0f, 18.0f, 18.0f);
    g.setColour (theme.accent);
    g.fillEllipse (badge);
    g.setColour (juce::Colours::white);
    g.setFont (monoFont (11.0f, true));
    g.drawText (juce::String::charToString ((juce::juce_wchar) letter), badge, juce::Justification::centred, false);

    g.setColour (theme.ink);
    g.setFont (monoFont (11.5f));
    g.drawText (title, head.withTrimmedLeft (28.0f), juce::Justification::centredLeft, false);

    if (meta.isNotEmpty())
    {
        g.setColour (theme.dim);
        g.setFont (uiFont (10.5f));
        g.drawText (meta, head.withTrimmedLeft (head.getWidth() * 0.35f), juce::Justification::centredRight, true);
    }

    // 見出しの下の点線
    const float dashes[] = { 2.0f, 3.0f };
    g.setColour (theme.faint.withMultipliedAlpha (1.6f));
    g.drawDashedLine ({ r.getX() + 12.0f, r.getY() + 32.0f, r.getRight() - 12.0f, r.getY() + 32.0f }, dashes, 2, 1.0f);

    return r.reduced (14.0f, 0.0f).withTrimmedTop (38.0f).withTrimmedBottom (10.0f);
}

void MidiStackView::paintChip (juce::Graphics& g, juce::Rectangle<float> r, const juce::String& text,
                               juce::Colour fill, juce::Colour textColour, bool outlineOnly)
{
    const float radius = r.getHeight() * 0.5f;
    if (outlineOnly)
    {
        g.setColour (fill);
        g.drawRoundedRectangle (r.reduced (0.5f), radius, 1.0f);
    }
    else
    {
        g.setColour (fill);
        g.fillRoundedRectangle (r, radius);
    }
    g.setColour (textColour);
    g.setFont (uiFont (juce::jmin (11.5f, r.getHeight() * 0.68f), true));
    g.drawText (text, r, juce::Justification::centred, false);
}

void MidiStackView::paintRecChips (juce::Graphics& g)
{
    const auto st = proc.recState.load();
    const juce::Colour red (0xffe0524d);

    juce::String recText = u8 ("● 録音");
    if (st == MidiStackLensProcessor::RecRunning)
    {
        const int s = (int) proc.recSeconds.load();
        recText = u8 ("■ 停止 ") + juce::String (s / 60) + ":" + juce::String (s % 60).paddedLeft ('0', 2);
        paintChip (g, recChip, recText, red, juce::Colours::white);
    }
    else if (st == MidiStackLensProcessor::RecArmed)
    {
        const bool blink = std::fmod (nowSeconds(), 1.0) < 0.55;
        paintChip (g, recChip, u8 ("● 待機中"), blink ? red : red.withAlpha (0.45f), juce::Colours::white);
    }
    else
    {
        if (proc.recFull.load())
            recText = u8 ("● 録音(上限)");
        paintChip (g, recChip, recText, red, red, true);
    }

    const bool follow = pRecFollow->load() > 0.5f;
    paintChip (g, followChip, follow ? u8 ("連動 ON") : u8 ("連動 OFF"), follow ? theme.accent : theme.dim,
               follow ? theme.accent : theme.dim, true);

    const bool has = proc.hasRecording();
    const auto c = has ? theme.accent : theme.dim.withAlpha (0.45f);
    paintChip (g, midChip, "MIDI", c, c, true);
    paintChip (g, mp4Chip, "MP4", c, c, true);
}

void MidiStackView::paintBackground (juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat();
    g.setGradientFill (juce::ColourGradient (theme.bgTop, 0.0f, 0.0f, theme.bgBottom, 0.0f, b.getBottom(), false));
    g.fillRect (b);

    // 方眼
    g.setColour (theme.grid);
    for (float x = 0.0f; x < b.getRight(); x += 24.0f)
        g.fillRect (x, 0.0f, 1.0f, b.getHeight());
    for (float y = 0.0f; y < b.getBottom(); y += 24.0f)
        g.fillRect (0.0f, y, b.getWidth(), 1.0f);
}

void MidiStackView::paintTitle (juce::Graphics& g)
{
    auto r = titleRect;

    // 右側: BAR / BEAT / METER / BPM / KEY
    const auto tp = transport();
    const double q = tp.ppq;
    const int num = juce::jmax (1, tp.num);
    const int den = juce::jmax (1, tp.den);
    const double beatLen = 4.0 / (double) den;       // 1拍の長さ(4分音符 = 1)
    const double barLen = beatLen * (double) num;

    juce::String barText = u8 ("—"), beatText = u8 ("—");
    float barProgress = 0.0f;
    if (q >= 0.0)
    {
        double barStart = tp.barStart;
        if (barStart < 0.0 || barStart > q)
            barStart = std::floor (q / barLen) * barLen;

        const int bar = (int) std::floor (barStart / barLen + 1.0e-6) + 1;
        const double inBar = juce::jlimit (0.0, barLen, q - barStart);
        const int beat = juce::jlimit (1, num, (int) std::floor (inBar / beatLen + 1.0e-6) + 1);
        barText = juce::String (bar).paddedLeft ('0', 3);
        beatText = juce::String (beat);
        barProgress = (float) (inBar / barLen);
    }

    struct Readout { const char* label; juce::String value; float width; bool mono; };
    const Readout items[] = {
        { "BAR", barText, 62.0f, true },
        { "BEAT", beatText, 48.0f, true },
        { "METER", juce::String (num) + "/" + juce::String (den), 62.0f, true },
        { "BPM", juce::String (juce::roundToInt (tp.bpm)), 56.0f, true },
        { "KEY", keyText(), 96.0f, false },
    };

    float x = r.getRight();
    for (int i = (int) std::size (items) - 1; i >= 0; --i)
    {
        const auto& it = items[i];
        x -= it.width;
        auto cell = juce::Rectangle<float> (x, r.getY(), it.width, r.getHeight());

        g.setColour (theme.accent.withAlpha (0.85f));
        g.setFont (monoFont (9.5f));
        g.drawText (it.label, cell.removeFromTop (16.0f), juce::Justification::bottomLeft, false);

        g.setColour (theme.ink);
        g.setFont (it.mono ? monoFont (23.0f) : uiFont (19.0f, true));
        g.drawText (it.value, cell.withTrimmedBottom (8.0f), juce::Justification::centredLeft, false);
        x -= 10.0f;
    }

    if (! offline)
        paintRecChips (g);

    // 左側: タイトル
    auto titleArea = r.withRight (juce::jmax (r.getX() + 120.0f, x));
    g.setColour (theme.ink);
    g.setFont (uiFont (30.0f));
    g.drawText ("MIDI Stack Lens", titleArea.withTrimmedBottom (8.0f), juce::Justification::centredLeft, true);

    // 下の線: 小節の中の位置
    auto line = juce::Rectangle<float> (r.getX(), r.getBottom() - 4.0f, r.getWidth(), 2.0f);
    g.setColour (theme.faint.withMultipliedAlpha (1.4f));
    g.fillRoundedRectangle (line, 1.0f);
    if (q >= 0.0)
    {
        g.setColour (theme.accent);
        g.fillRoundedRectangle (line.withWidth (juce::jmax (3.0f, line.getWidth() * barProgress)).expanded (0.0f, 0.5f), 1.5f);
    }
}

void MidiStackView::paintRoll (juce::Graphics& g, double now)
{
    juce::Graphics::ScopedSaveState saved (g);
    g.reduceClipRegion (rollRect.toNearestInt());

    g.setGradientFill (juce::ColourGradient (theme.rollTop, 0.0f, rollRect.getY(), theme.rollBottom, 0.0f,
                                             rollRect.getBottom(), false));
    g.fillRect (rollRect);

    // オクターブ(C)の縦ガイド
    g.setColour (theme.rollLine);
    for (int n = loNote; n <= hiNote; ++n)
        if (n % 12 == 0)
            g.fillRect (kx[(size_t) n], rollRect.getY(), 1.0f, rollRect.getHeight());

    const double win = windowSeconds();
    const auto top = rollRect.getY();

    // 再生中は拍の線を引く
    const auto tp = transport();
    const double q = tp.ppq;
    if (tp.playing && q >= 0.0)
    {
        const double spb = secondsPerBeat();
        const double frac = q - std::floor (q);
        g.setColour (theme.rollLine);
        for (int k = 0; k < 40; ++k)
        {
            const double age = (frac + (double) k) * spb;
            if (age > win)
                break;
            const float y = rollRect.getBottom() - (float) (age / win) * rollRect.getHeight();
            g.fillRect (rollRect.getX(), y, rollRect.getWidth(), 1.0f);
        }
    }

    const bool scaleOutline = key.valid && choiceOf (pColor) != 3 && flagOf (pScaleHint);

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
        const float inset = isBlackKey (b.note) ? 1.0f : 1.5f;

        const float age = b.end < 0.0 ? 0.0f : (float) ((now - b.end) / win);
        const float fade = juce::jlimit (0.15f, 1.0f, 1.0f - age * 0.85f);
        const float velA = 0.6f + 0.4f * (float) b.vel / 127.0f;
        const float alpha = fade * (b.end < 0.0 ? 1.0f : velA);

        auto rect = juce::Rectangle<float> (kx[(size_t) b.note] + inset, yBot - h,
                                            kw[(size_t) b.note] - inset * 2.0f, h);
        g.setColour (colourFor (b.note, b.rank, b.vel).withAlpha (alpha));
        g.fillRoundedRectangle (rect, 2.0f);

        // スケール外の音は枠で知らせる
        if (scaleOutline && ! msl::inScale (b.note % 12, key.tonicPc, key.minor))
        {
            g.setColour (theme.warn.withAlpha (alpha));
            g.drawRoundedRectangle (rect.expanded (1.0f), 2.5f, 1.2f);
        }
    }

    // コード履歴(小さく)
    if (flagOf (pChordHistory))
    {
        g.setFont (uiFont (11.0f, true));
        for (const auto& l : labels)
        {
            const float y = yOf (l.start, now);
            if (y < top + 6.0f || y > rollRect.getBottom())
                continue;

            const float fade = juce::jlimit (0.2f, 1.0f, 1.0f - (float) ((now - l.start) / win) * 0.8f);
            auto r = juce::Rectangle<float> (rollRect.getX() + 6.0f, y - 8.0f, 78.0f, 16.0f);
            g.setColour (theme.panel.withAlpha (0.85f * fade));
            g.fillRoundedRectangle (r, 8.0f);
            g.setColour (theme.border.withAlpha (0.7f * fade));
            g.drawRoundedRectangle (r, 8.0f, 1.0f);
            g.setColour (theme.ink.withAlpha (0.95f * fade));
            g.drawText (l.text, r.reduced (7.0f, 0.0f), juce::Justification::centredLeft, true);
        }
    }

    if (bars.empty() && labels.empty())
    {
        g.setColour (theme.dim.withAlpha (0.7f));
        g.setFont (uiFont (12.5f));
        g.drawText (u8 ("MIDI を流すと、押さえた音がここに積み上がります"), rollRect, juce::Justification::centred, true);
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
    const bool showScale = flagOf (pScaleHint) && key.valid;
    const int labelMode = juce::jlimit (0, 2, choiceOf (pKeyLabels));
    const float blackH = keyRect.getHeight() * 0.62f;

    g.setColour (theme.keyLine);
    g.fillRoundedRectangle (keyRect.expanded (1.0f, 0.0f).withTrimmedTop (-1.0f), 3.0f);

    // 白鍵
    for (int n = loNote; n <= hiNote; ++n)
    {
        if (isBlackKey (n))
            continue;

        auto k = juce::Rectangle<float> (kx[(size_t) n], keyRect.getY(), kw[(size_t) n], keyRect.getHeight())
                     .reduced (0.5f, 0.0f)
                     .withTrimmedBottom (1.0f);
        const bool on = held[(size_t) n] != 0;
        const int pc = n % 12;

        g.setColour (on ? colourFor (n, rankOf[(size_t) n], held[(size_t) n]) : theme.whiteKey);
        g.fillRoundedRectangle (k, 2.0f);

        if (showScale && ! on && msl::inScale (pc, key.tonicPc, key.minor))
        {
            const float d = juce::jlimit (2.5f, 5.0f, kw[(size_t) n] * 0.28f);
            g.setColour (theme.accent.withAlpha (pc == key.tonicPc ? 0.95f : 0.4f));
            g.fillEllipse (k.getCentreX() - d * 0.5f, keyRect.getY() + blackH + 5.0f, d, d);
        }

        const bool showName = labelMode == 1 || (labelMode == 0 && pc == 0);
        if (showName && kw[(size_t) n] >= 11.0f)
        {
            g.setColour (theme.keyInk.withAlpha (on ? 0.9f : 0.5f));
            g.setFont (uiFont (juce::jlimit (8.0f, 11.0f, kw[(size_t) n] * 0.6f)));
            const auto txt = pc == 0 ? noteNameWithOctave (n) : noteName (pc);
            g.drawText (txt, k.withTrimmedBottom (3.0f), juce::Justification::centredBottom, false);
        }

        if (showDeg && on && ! cur.degree[(size_t) pc].empty())
        {
            g.setColour (dark ? theme.keyInk : juce::Colours::white);
            g.setFont (uiFont (juce::jlimit (9.0f, 13.0f, kw[(size_t) n] * 0.75f), true));
            g.drawText (u8 (cur.degree[(size_t) pc]), k.withTop (keyRect.getY() + blackH + 2.0f).withHeight (16.0f),
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
        const int pc = n % 12;

        g.setColour (on ? colourFor (n, rankOf[(size_t) n], held[(size_t) n]) : theme.blackKey);
        g.fillRoundedRectangle (k, 2.0f);
        g.setColour (theme.keyLine.withAlpha (0.7f));
        g.drawRoundedRectangle (k, 2.0f, 0.8f);

        if (showScale && ! on && msl::inScale (pc, key.tonicPc, key.minor))
        {
            const float d = juce::jlimit (2.0f, 4.0f, kw[(size_t) n] * 0.4f);
            g.setColour (juce::Colours::white.withAlpha (pc == key.tonicPc ? 0.95f : 0.5f));
            g.fillEllipse (k.getCentreX() - d * 0.5f, k.getBottom() - d - 4.0f, d, d);
        }

        if (labelMode == 1 && ! on && kw[(size_t) n] >= 15.0f)
        {
            g.setColour (juce::Colours::white.withAlpha (0.55f));
            g.setFont (uiFont (8.0f));
            g.drawText (noteName (pc), k.withTrimmedBottom (10.0f), juce::Justification::centredBottom, false);
        }

        if (showDeg && on && ! cur.degree[(size_t) pc].empty())
        {
            g.setColour (dark ? theme.keyInk : juce::Colours::white);
            g.setFont (uiFont (juce::jlimit (8.0f, 12.0f, kw[(size_t) n] * 0.8f), true));
            g.drawText (u8 (cur.degree[(size_t) pc]), k.withTrimmedBottom (4.0f), juce::Justification::centredBottom,
                        false);
        }
    }
}

// ---- B: CHORD / VOICING -------------------------------------------------------------
void MidiStackView::paintChordPanel (juce::Graphics& g)
{
    const bool has = ! snap.notes.empty();
    juce::String meta;
    if (has)
        meta = snapLive ? juce::String (snap.voicing.count) + u8 (" 音") : u8 ("直前の形");

    auto c = paintPanel (g, panelB, 'B', "CHORD  /  VOICING", meta);

    if (! has)
    {
        g.setColour (theme.dim.withAlpha (0.7f));
        g.setFont (uiFont (12.5f));
        g.drawText (u8 ("和音を押さえると、コード名と積み方が出ます"), c, juce::Justification::centred, true);
        return;
    }

    const float alpha = snapLive ? 1.0f : 0.5f;
    const auto& v = snap.voicing;
    const auto& ch = snap.chord;

    // 下段: 数値の行
    auto stats = c.removeFromBottom (38.0f);
    c.removeFromBottom (6.0f);

    // 左: コード名とディグリー
    auto left = c.removeFromLeft (juce::jlimit (130.0f, 300.0f, c.getWidth() * 0.34f));
    c.removeFromLeft (12.0f);

    juce::String name;
    if (ch.named)
        name = chordText (ch);
    else if (snap.notes.size() == 1)
        name = noteName (snap.notes[0] % 12);
    else
        name = u8 ("—");

    auto chipRow = left.removeFromBottom (22.0f);
    g.setColour (theme.ink.withAlpha (alpha));
    g.setFont (uiFont (juce::jlimit (26.0f, 54.0f, left.getHeight() * 0.8f)));
    g.drawFittedText (name, left.toNearestInt(), juce::Justification::centredLeft, 1, 0.45f);

    {
        float x = chipRow.getX();
        const auto roman = romanText (ch);
        if (roman.isNotEmpty())
        {
            const float w = juce::jmax (40.0f, 16.0f + (float) roman.length() * 8.0f);
            paintChip (g, { x, chipRow.getY(), w, 20.0f }, roman, theme.accent.withAlpha (alpha), juce::Colours::white);
            x += w + 6.0f;

            const auto fn = u8 (msl::harmonicFunction (ch, key.tonicPc, key.minor));
            if (fn.isNotEmpty())
            {
                paintChip (g, { x, chipRow.getY(), 34.0f, 20.0f }, fn, functionColour (fn).withAlpha (alpha),
                           functionColour (fn).withAlpha (alpha), true);
                x += 40.0f;
            }
        }
        if (v.lowIntervalWarning && x + 96.0f <= chipRow.getRight() + 60.0f)
            paintChip (g, { x, chipRow.getY(), 96.0f, 20.0f }, u8 ("低音が近い"), theme.warn.withAlpha (alpha),
                       juce::Colours::white);
    }

    // 右: 積み方のはしご(低い音 → 高い音、間に音程)
    {
        const int total = (int) snap.notes.size();
        const int shown = juce::jmin (total, 10);
        const float connector = total > 7 ? 22.0f : 30.0f;
        const float avail = c.getWidth() - (total > shown ? 18.0f : 0.0f);
        const float pillW = juce::jlimit (26.0f, 62.0f, (avail - connector * (float) (shown - 1)) / (float) shown);
        const float pillH = juce::jmin (52.0f, c.getHeight());
        const float y = c.getCentreY() - pillH * 0.5f;
        float x = c.getX();

        for (int i = 0; i < shown; ++i)
        {
            const int n = snap.notes[(size_t) i];
            const int pc = n % 12;
            auto pill = juce::Rectangle<float> (x, y, pillW, pillH);
            const auto col = colourFor (n, i, snap.vels[(size_t) i]);
            const bool warn = v.lowIntervalWarning && (n == v.warnLow);

            g.setColour (col.withAlpha (0.16f * alpha));
            g.fillRoundedRectangle (pill, 8.0f);
            g.setColour ((warn ? theme.warn : col).withAlpha (alpha));
            g.drawRoundedRectangle (pill.reduced (0.5f), 8.0f, warn ? 2.0f : 1.4f);

            g.setColour (theme.ink.withAlpha (alpha));
            g.setFont (uiFont (pillW < 40.0f ? 10.5f : 13.0f, true));
            g.drawText (noteNameWithOctave (n), pill.withHeight (pillH * 0.56f), juce::Justification::centredBottom,
                        false);

            g.setColour (theme.dim.withAlpha (alpha));
            g.setFont (uiFont (10.5f));
            g.drawText (ch.hasDegrees ? u8 (ch.degree[(size_t) pc]) : juce::String(),
                        pill.withTrimmedTop (pillH * 0.58f).withTrimmedBottom (4.0f), juce::Justification::centred,
                        false);

            x += pillW;

            if (i + 1 < shown)
            {
                auto link = juce::Rectangle<float> (x, y, connector, pillH);
                g.setColour (theme.dim.withAlpha (0.5f * alpha));
                g.fillRect (link.getX() + 2.0f, link.getCentreY() + 7.0f, link.getWidth() - 4.0f, 1.0f);
                g.setColour (theme.accent.withAlpha (alpha));
                g.setFont (monoFont (connector < 26.0f ? 8.5f : 10.0f));
                g.drawText (u8 (msl::intervalName (v.gaps[(size_t) i])), link.withTrimmedBottom (pillH * 0.4f),
                            juce::Justification::centredBottom, false);
                x += connector;
            }
        }

        if (total > shown)
        {
            g.setColour (theme.dim.withAlpha (alpha));
            g.setFont (uiFont (13.0f));
            g.drawText (u8 ("…"), juce::Rectangle<float> (x + 2.0f, y, 16.0f, pillH), juce::Justification::centred, false);
        }
    }

    // 数値の行
    {
        const float dashes[] = { 2.0f, 3.0f };
        g.setColour (theme.faint.withMultipliedAlpha (1.6f));
        g.drawDashedLine ({ stats.getX(), stats.getY(), stats.getRight(), stats.getY() }, dashes, 2, 1.0f);

        juce::String inv = u8 ("—");
        if (v.onChord)
            inv = u8 ("オンコード");
        else if (v.inversion == 0)
            inv = u8 ("基本形");
        else if (v.inversion > 0)
            inv = u8 ("第") + juce::String (v.inversion) + u8 ("転回");

        const int topNote = snap.notes.back();
        juce::String topText = noteNameWithOctave (topNote);
        if (ch.hasDegrees && ! ch.degree[(size_t) (topNote % 12)].empty())
            topText << " (" << u8 (ch.degree[(size_t) (topNote % 12)]) << ")";

        juce::String spanText = u8 ("—");
        if (v.count >= 2)
            spanText = u8 (msl::intervalName (v.span)) + u8 (" · ") + juce::String (v.span) + u8 (" 半音");

        struct Cell { const char* label; juce::String value; };
        const Cell cells[] = { { "TYPE", u8 (msl::voicingTypeName (v.type)) },
                               { "INVERSION", inv },
                               { "SPAN", spanText },
                               { "TOP", topText } };

        const float cw = stats.getWidth() / (float) std::size (cells);
        for (size_t i = 0; i < std::size (cells); ++i)
        {
            auto cell = juce::Rectangle<float> (stats.getX() + cw * (float) i, stats.getY() + 4.0f, cw - 6.0f,
                                                stats.getHeight() - 4.0f);
            g.setColour (theme.accent.withAlpha (0.85f * alpha));
            g.setFont (monoFont (9.0f));
            g.drawText (cells[i].label, cell.removeFromTop (12.0f), juce::Justification::centredLeft, false);
            g.setColour (theme.ink.withAlpha (alpha));
            g.setFont (uiFont (13.5f, true));
            g.drawText (cells[i].value, cell, juce::Justification::centredLeft, true);
        }
    }
}

// ---- C: HARMONY / CIRCLE OF FIFTHS --------------------------------------------------
void MidiStackView::paintHarmonyPanel (juce::Graphics& g)
{
    juce::String meta;
    if (key.valid)
        meta = u8 ("推定キー ") + keyText() + "  " + juce::String (juce::roundToInt (key.confidence * 100.0)) + "%";

    auto c = paintPanel (g, panelC, 'C', "HARMONY  /  CIRCLE OF 5THS", meta);

    // 右: 五度圏
    const float side = juce::jmin (c.getHeight(), c.getWidth() * 0.62f);
    auto circleArea = c.removeFromRight (side);
    const float cx = circleArea.getCentreX();
    const float cy = circleArea.getCentreY();
    const float nodeR = juce::jlimit (8.0f, 13.0f, side * 0.075f);
    const float R = side * 0.5f - nodeR - 3.0f;

    auto indexOf = [] (int pc) { return (pc * 7) % 12; };  // 五度圏での位置(0 = 上)
    auto posOf = [&] (int index)
    {
        const float a = kTwoPi * (float) index / 12.0f;
        return juce::Point<float> (cx + R * std::sin (a), cy - R * std::cos (a));
    };

    g.setColour (theme.faint.withMultipliedAlpha (1.5f));
    g.drawEllipse (cx - R, cy - R, R * 2.0f, R * 2.0f, 1.0f);

    // 推定キーのダイアトニック7音は五度圏で隣り合う(IV 〜 VII)
    if (key.valid)
    {
        const int majorTonic = key.minor ? (key.tonicPc + 3) % 12 : key.tonicPc;
        const int start = indexOf (majorTonic) - 1;
        const float a0 = kTwoPi * ((float) start - 0.35f) / 12.0f;
        const float a1 = kTwoPi * ((float) start + 6.35f) / 12.0f;
        juce::Path arc;
        arc.addCentredArc (cx, cy, R, R, 0.0f, a0, a1, true);
        g.setColour (theme.accentSoft);
        g.strokePath (arc, juce::PathStrokeType (nodeR * 2.0f + 8.0f, juce::PathStrokeType::curved,
                                                 juce::PathStrokeType::rounded));
    }

    // 押さえている音(離したあとは直前の形を薄く)
    std::array<bool, 12> on {};
    std::vector<int> order;  // 低い順に出てきた音名
    for (int n : snap.notes)
    {
        const int pc = n % 12;
        if (! on[(size_t) pc])
        {
            on[(size_t) pc] = true;
            order.push_back (pc);
        }
    }
    const float alpha = snapLive ? 1.0f : 0.45f;

    if (order.size() >= 2)
    {
        juce::Path poly;
        for (size_t i = 0; i < order.size(); ++i)
        {
            const auto p = posOf (indexOf (order[i]));
            if (i == 0)
                poly.startNewSubPath (p);
            else
                poly.lineTo (p);
        }
        if (order.size() >= 3)
            poly.closeSubPath();

        g.setColour (theme.accent.withAlpha (0.10f * alpha));
        g.fillPath (poly);
        g.setColour (theme.accent.withAlpha (0.7f * alpha));
        g.strokePath (poly, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    const bool hasChord = ! snap.notes.empty() && snap.chord.named;
    const int bassPc = snap.notes.empty() ? -1 : snap.notes.front() % 12;

    for (int pc = 0; pc < 12; ++pc)
    {
        const auto p = posOf (indexOf (pc));
        auto node = juce::Rectangle<float> (p.x - nodeR, p.y - nodeR, nodeR * 2.0f, nodeR * 2.0f);
        const bool isOn = on[(size_t) pc];
        const bool isRoot = hasChord && pc == snap.chord.rootPc;

        g.setColour (isOn ? theme.accent.interpolatedWith (theme.panel, 1.0f - alpha) : theme.panel);
        g.fillEllipse (node);
        g.setColour (isOn ? theme.accent : theme.dim.withAlpha (0.55f));
        g.drawEllipse (node, 1.2f);

        if (isRoot)
        {
            g.setColour (theme.accent.withAlpha (alpha));
            g.drawEllipse (node.expanded (3.0f), 1.4f);
        }
        if (isOn && pc == bassPc && ! isRoot)
        {
            g.setColour (theme.warn.withAlpha (alpha));
            g.drawEllipse (node.expanded (3.0f), 1.4f);
        }

        g.setColour (isOn ? juce::Colours::white : theme.ink.withAlpha (key.valid && key.tonicPc == pc ? 1.0f : 0.7f));
        g.setFont (uiFont (juce::jlimit (8.0f, 11.0f, nodeR * 0.85f), isOn || (key.valid && key.tonicPc == pc)));
        g.drawText (noteName (pc), node.expanded (4.0f, 0.0f), juce::Justification::centred, false);
    }

    // 左: 音名ごとの出現量(キー推定の材料)
    c.removeFromRight (10.0f);
    if (c.getWidth() >= 110.0f)
    {
        g.setColour (theme.accent.withAlpha (0.85f));
        g.setFont (monoFont (9.0f));
        g.drawText ("PITCH CLASS", c.removeFromTop (12.0f), juce::Justification::centredLeft, false);

        auto labelsRow = c.removeFromBottom (13.0f);
        const int offset = juce::jlimit (0, 11, choiceOf (pKey));  // 表示上の C から並べる
        double maxW = 1.0;
        for (double w : pcWeight)
            maxW = juce::jmax (maxW, w);

        const float bw = c.getWidth() / 12.0f;
        const bool narrow = bw < 15.0f;
        for (int i = 0; i < 12; ++i)
        {
            const int pc = (i + offset) % 12;
            const float h = (float) (pcWeight[(size_t) pc] / maxW) * (c.getHeight() - 4.0f);
            auto slot = juce::Rectangle<float> (c.getX() + bw * (float) i, c.getY(), bw, c.getHeight());
            const bool scaleNote = key.valid && msl::inScale (pc, key.tonicPc, key.minor);

            g.setColour (theme.faint);
            g.fillRoundedRectangle (slot.reduced (2.0f, 0.0f).withTrimmedTop (4.0f), 2.0f);
            if (h > 0.5f)
            {
                g.setColour (! key.valid || scaleNote ? theme.accent.withAlpha (on[(size_t) pc] ? 1.0f : 0.6f)
                                                      : theme.warn.withAlpha (0.8f));
                g.fillRoundedRectangle (slot.reduced (2.0f, 0.0f).withTop (slot.getBottom() - juce::jmax (2.0f, h)), 2.0f);
            }

            const bool blackPc = isBlackKey (i);
            if (! (narrow && blackPc))
            {
                g.setColour (theme.dim);
                g.setFont (uiFont (narrow ? 8.0f : 9.0f));
                g.drawText (noteName (pc), juce::Rectangle<float> (slot.getX() - 4.0f, labelsRow.getY(), bw + 8.0f, 13.0f),
                            juce::Justification::centred, false);
            }
        }
    }
}

// ---- D: PROGRESSION -----------------------------------------------------------------
void MidiStackView::paintProgressionPanel (juce::Graphics& g, double now)
{
    auto c = paintPanel (g, panelD, 'D', "PROGRESSION", {});

    // 見出しの右のボタン
    auto head = panelD.withHeight (32.0f).reduced (12.0f, 6.0f);
    clearChip = head.removeFromRight (64.0f);
    head.removeFromRight (6.0f);
    copyChip = head.removeFromRight (72.0f);

    if (! offline)
    {
        const bool justCopied = nowSeconds() - copiedAt < 1.2;
        paintChip (g, copyChip, justCopied ? u8 ("コピー済み") : u8 ("コピー"),
                   justCopied ? theme.accent : theme.accent.withAlpha (0.9f),
                   justCopied ? juce::Colours::white : theme.accent, ! justCopied);
        paintChip (g, clearChip, u8 ("クリア"), theme.dim, theme.dim, true);
    }

    if (progression.empty())
    {
        g.setColour (theme.dim.withAlpha (0.7f));
        g.setFont (uiFont (12.5f));
        g.drawText (u8 ("弾いたコードが順に並びます(コピーで C | Am7 | F の形に)"), c, juce::Justification::centred, true);
        return;
    }

    const int maxCells = juce::jmax (1, (int) (c.getWidth() / 86.0f));
    const int count = juce::jmin (maxCells, (int) progression.size());
    const float gap = 6.0f;
    const float cw = (c.getWidth() - gap * (float) (maxCells - 1)) / (float) maxCells;
    const double spb = secondsPerBeat();

    for (int i = 0; i < count; ++i)
    {
        const auto& p = progression[progression.size() - (size_t) count + (size_t) i];
        const bool live = p.end < 0.0;
        auto cell = juce::Rectangle<float> (c.getX() + (cw + gap) * (float) i, c.getY(), cw, c.getHeight());

        const auto fn = key.valid ? u8 (msl::harmonicFunction (p.chord, key.tonicPc, key.minor)) : juce::String();
        const auto fnCol = functionColour (fn);

        g.setColour (live ? theme.accentSoft : theme.faint.withMultipliedAlpha (0.6f));
        g.fillRoundedRectangle (cell, 7.0f);
        g.setColour (live ? theme.accent : theme.dim.withAlpha (0.45f));
        g.drawRoundedRectangle (cell.reduced (0.5f), 7.0f, live ? 1.5f : 1.0f);

        // 下の帯: 機能(T / SD / D)
        g.setColour (fnCol);
        g.fillRoundedRectangle (cell.reduced (8.0f, 0.0f).withTop (cell.getBottom() - 6.0f).withHeight (2.5f), 1.2f);

        auto inner = cell.reduced (7.0f, 4.0f).withTrimmedBottom (5.0f);
        const float nameH = inner.getHeight() * 0.5f;

        g.setColour (theme.ink);
        g.setFont (uiFont (juce::jlimit (13.0f, 19.0f, nameH * 0.8f), true));
        g.drawFittedText (p.name, inner.removeFromTop (nameH).toNearestInt(), juce::Justification::centredLeft, 1, 0.6f);

        const auto roman = romanText (p.chord);
        g.setColour (theme.accent);
        g.setFont (uiFont (11.0f, true));
        g.drawText (roman, inner.removeFromTop (inner.getHeight() * 0.5f), juce::Justification::centredLeft, true);

        const double beats = ((live ? now : p.end) - p.start) / spb;
        g.setColour (theme.dim);
        g.setFont (monoFont (9.0f));
        g.drawText (juce::String (beats, 1) + " beats" + (fn.isNotEmpty() ? "  " + fn : juce::String()), inner,
                    juce::Justification::centredLeft, true);
    }
}

// ---- E: VELOCITY --------------------------------------------------------------------
void MidiStackView::paintVelocityPanel (juce::Graphics& g)
{
    juce::String meta;
    const int count = (int) snap.notes.size();
    if (count > 0)
    {
        int sum = 0;
        for (int v : snap.vels)
            sum += v;
        meta << "AVG " << juce::roundToInt ((double) sum / (double) count);

        if (count >= 2)
        {
            const int top = snap.vels.back();
            const double rest = (double) (sum - top) / (double) (count - 1);
            const int diff = juce::roundToInt ((double) top - rest);
            meta << u8 ("  ·  TOP ") << (diff >= 0 ? "+" : "") << diff;
        }
    }

    auto c = paintPanel (g, panelE, 'E', "VELOCITY  /  BALANCE", meta);

    if (count == 0)
    {
        g.setColour (theme.dim.withAlpha (0.7f));
        g.setFont (uiFont (12.5f));
        g.drawText (u8 ("音ごとの強さのバランス"), c, juce::Justification::centred, true);
        return;
    }

    const float alpha = snapLive ? 1.0f : 0.5f;
    auto labelsRow = c.removeFromBottom (13.0f);
    auto valuesRow = c.removeFromTop (12.0f);

    // 目盛り(64 と 127)
    g.setColour (theme.faint);
    g.fillRect (c.getX(), c.getY(), c.getWidth(), 1.0f);
    g.fillRect (c.getX(), c.getCentreY(), c.getWidth(), 1.0f);

    const int shown = juce::jmin (count, 16);
    const float slotW = juce::jmin (46.0f, c.getWidth() / (float) shown);
    const float startX = c.getX();

    for (int i = 0; i < shown; ++i)
    {
        const int n = snap.notes[(size_t) i];
        const int v = snap.vels[(size_t) i];
        auto slot = juce::Rectangle<float> (startX + slotW * (float) i, c.getY(), slotW, c.getHeight());
        const float h = juce::jmax (2.0f, (float) v / 127.0f * slot.getHeight());
        auto bar = slot.reduced (juce::jmax (2.0f, slotW * 0.2f), 0.0f).withTop (slot.getBottom() - h);

        g.setColour (colourFor (n, i, v).withAlpha (alpha));
        g.fillRoundedRectangle (bar, 3.0f);

        if (slotW >= 20.0f)
        {
            g.setColour (theme.ink.withAlpha (alpha));
            g.setFont (monoFont (9.0f));
            g.drawText (juce::String (v), juce::Rectangle<float> (slot.getX(), valuesRow.getY(), slotW, 12.0f),
                        juce::Justification::centred, false);

            g.setColour (theme.dim.withAlpha (alpha));
            g.setFont (uiFont (9.0f));
            g.drawText (noteNameWithOctave (n), juce::Rectangle<float> (slot.getX() - 4.0f, labelsRow.getY(), slotW + 8.0f, 13.0f),
                        juce::Justification::centred, false);
        }
    }
}

// ---- 全体 ----------------------------------------------------------------------------
void MidiStackView::paint (juce::Graphics& g)
{
    const double now = clockNow();

    paintBackground (g);
    paintTitle (g);

    static const int keyCounts[] = { 88, 61, 49, 37 };
    static const int beatCounts[] = { 2, 4, 8, 16 };
    const juce::String metaA = juce::String (keyCounts[juce::jlimit (0, 3, choiceOf (pRange))]) + u8 (" 鍵  ·  履歴 ")
                               + juce::String (beatCounts[juce::jlimit (0, 3, choiceOf (pWindow))]) + u8 (" 拍");
    paintPanel (g, panelA, 'A', "STACK  /  ROLL", metaA);
    paintRoll (g, now);
    paintKeyboard (g);

    paintChordPanel (g);
    paintHarmonyPanel (g);
    paintProgressionPanel (g, now);
    paintVelocityPanel (g);
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
    setLookAndFeel (&lnf);

    addCombo (cbNotation, "notation");
    addCombo (cbAccidental, "accidental");
    addCombo (cbKey, "key");
    addCombo (cbKeyLabels, "keyLabels");
    addCombo (cbRange, "range");
    addCombo (cbWindow, "window");
    addCombo (cbColor, "colorMode");
    addCombo (cbTheme, "theme");

    tbDegrees.setButtonText (juce::String::fromUTF8 ("度数"));
    tbHistory.setButtonText (juce::String::fromUTF8 ("コード履歴"));
    tbScale.setButtonText (juce::String::fromUTF8 ("スケール"));
    addAndMakeVisible (tbDegrees);
    addAndMakeVisible (tbHistory);
    addAndMakeVisible (tbScale);
    degreesAttachment = std::make_unique<ButtonAttach> (proc.apvts, "degrees", tbDegrees);
    historyAttachment = std::make_unique<ButtonAttach> (proc.apvts, "chordHistory", tbHistory);
    scaleAttachment = std::make_unique<ButtonAttach> (proc.apvts, "scaleHint", tbScale);

    addAndMakeVisible (view);
    view.onThemeChanged = [this] (bool dark) { applyTheme (dark); };
    view.onMidiClick = [this] { exportMidi(); };
    view.onVideoClick = [this] { exportVideo(); };
    applyTheme (view.isDark());

    setResizable (true, true);
    setResizeLimits (960, 620, 2600, 1600);
    setSize (1040, 780);
}

MidiStackLensEditor::~MidiStackLensEditor()
{
    view.onThemeChanged = nullptr;
    view.onMidiClick = nullptr;
    view.onVideoClick = nullptr;

    if (progressWindow != nullptr)
        progressWindow->exitModalState (0);
    progressWindow.reset();
    exporter.reset();
    chooser.reset();
    setLookAndFeel (nullptr);
}

// =====================================================================================
//  書き出し(.mid / mp4)
// =====================================================================================
struct MidiStackLensEditor::VideoRequest
{
    msl::Recording rec;
    int width = 1920, height = 1080, fps = 30;
    double wavOffset = 0.0;
    juce::File wav, out, ffmpeg;
};

void MidiStackLensEditor::exportMidi()
{
    auto rec = std::make_shared<msl::Recording> (proc.copyRecording());
    if (rec->empty())
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, u8 ("録音がありません"),
                                                u8 ("演奏中に「● 録音」を押して、MIDIを記録してください。"));
        return;
    }

    const auto def = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("MIDI Stack Lens.mid");
    chooser = std::make_unique<juce::FileChooser> (u8 (".mid の保存先"), def, "*.mid");
    juce::Component::SafePointer<MidiStackLensEditor> safe (this);
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [safe, rec] (const juce::FileChooser& fc)
                          {
                              auto f = fc.getResult();
                              if (safe == nullptr || f == juce::File())
                                  return;
                              f = f.withFileExtension ("mid");
                              const auto bytes = msl::toMidiFile (*rec);
                              if (f.replaceWithData (bytes.data(), bytes.size()))
                                  juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon,
                                                                          u8 ("保存しました"), f.getFullPathName());
                              else
                                  juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                                          u8 ("保存できませんでした"), f.getFullPathName());
                          });
}

void MidiStackLensEditor::withFfmpeg (std::function<void (juce::File)> then)
{
    const auto saved = proc.apvts.state.getProperty ("ffmpegPath").toString();
    const auto found = VideoExporter::findFfmpeg (saved);
    if (found.existsAsFile())
    {
        then (found);
        return;
    }

    juce::Component::SafePointer<MidiStackLensEditor> safe (this);
    auto* w = new juce::AlertWindow (u8 ("ffmpeg が必要です"),
                                     u8 ("mp4 にまとめるのに、無料の ffmpeg を使います(このプラグインには入っていません)。\n"
                                         "入れたあと、ffmpeg の場所を教えてください。\n"
                                         "Windows: 「winget install ffmpeg」\nMac: 「brew install ffmpeg」"),
                                     juce::MessageBoxIconType::InfoIcon, this);
    w->addButton (u8 ("ffmpegの場所を選ぶ"), 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton (u8 ("やめる"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
    w->enterModalState (true,
                        juce::ModalCallbackFunction::create (
                            [safe, then] (int r)
                            {
                                if (r != 1 || safe == nullptr)
                                    return;
                                safe->chooser = std::make_unique<juce::FileChooser> (u8 ("ffmpeg を選ぶ"), juce::File(), "*");
                                safe->chooser->launchAsync (juce::FileBrowserComponent::openMode
                                                                | juce::FileBrowserComponent::canSelectFiles,
                                                            [safe, then] (const juce::FileChooser& fc)
                                                            {
                                                                const auto f = fc.getResult();
                                                                if (safe == nullptr || ! f.existsAsFile())
                                                                    return;
                                                                safe->proc.apvts.state.setProperty ("ffmpegPath",
                                                                                                    f.getFullPathName(), nullptr);
                                                                then (f);
                                                            });
                            }),
                        true);
}

void MidiStackLensEditor::exportVideo()
{
    auto req = std::make_shared<VideoRequest>();
    req->rec = proc.copyRecording();
    if (req->rec.empty())
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, u8 ("録音がありません"),
                                                u8 ("演奏中に「● 録音」を押して、MIDIを記録してください。"));
        return;
    }

    juce::Component::SafePointer<MidiStackLensEditor> safe (this);
    withFfmpeg (
        [safe, req] (juce::File ffmpeg)
        {
            if (safe == nullptr)
                return;
            req->ffmpeg = ffmpeg;

            auto* w = new juce::AlertWindow (u8 ("動画(MP4)に書き出す"),
                                             u8 ("録音した演奏を、一定のコマ数で動画にします。\n"
                                                 "音は、DAWで書き出したWAVを選ぶと合わせます。"),
                                             juce::MessageBoxIconType::NoIcon, safe.getComponent());
            w->addComboBox ("fmt", { u8 ("横  1920x1080 (16:9)"), u8 ("縦  1080x1920 (9:16)  SNS向け"),
                                     u8 ("正方形  1080x1080"), u8 ("横  1280x720") },
                            u8 ("サイズ"));
            w->addComboBox ("fps", { "30 fps", "60 fps" }, u8 ("コマ数"));
            w->addTextEditor ("offset", juce::String (msl::defaultWavOffset (req->rec), 3),
                              u8 ("WAVの頭出し(秒)  WAVの先頭が、録音の頭より何秒前か"));
            w->getComboBoxComponent ("fmt")->setSelectedItemIndex (0, juce::dontSendNotification);
            w->getComboBoxComponent ("fps")->setSelectedItemIndex (0, juce::dontSendNotification);
            w->addButton (u8 ("WAVを選んで書き出す"), 1, juce::KeyPress (juce::KeyPress::returnKey));
            w->addButton (u8 ("WAVなしで書き出す"), 2);
            w->addButton (u8 ("やめる"), 0, juce::KeyPress (juce::KeyPress::escapeKey));

            w->enterModalState (
                true,
                juce::ModalCallbackFunction::create (
                    [safe, req, w] (int r)
                    {
                        if (r == 0 || safe == nullptr)
                            return;

                        static const int sizes[4][2] = { { 1920, 1080 }, { 1080, 1920 }, { 1080, 1080 }, { 1280, 720 } };
                        const int fi = juce::jlimit (0, 3, w->getComboBoxComponent ("fmt")->getSelectedItemIndex());
                        req->width = sizes[fi][0];
                        req->height = sizes[fi][1];
                        req->fps = w->getComboBoxComponent ("fps")->getSelectedItemIndex() == 1 ? 60 : 30;
                        req->wavOffset = w->getTextEditorContents ("offset").getDoubleValue();

                        if (r == 1)
                            safe->askWavThenOutput (req);
                        else
                            safe->askOutput (req);
                    }),
                true);
        });
}

void MidiStackLensEditor::askWavThenOutput (std::shared_ptr<VideoRequest> req)
{
    juce::Component::SafePointer<MidiStackLensEditor> safe (this);
    chooser = std::make_unique<juce::FileChooser> (u8 ("DAWで書き出したWAVを選ぶ"),
                                                   juce::File::getSpecialLocation (juce::File::userDocumentsDirectory),
                                                   "*.wav;*.aif;*.aiff;*.flac");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [safe, req] (const juce::FileChooser& fc)
                          {
                              const auto f = fc.getResult();
                              if (safe == nullptr || ! f.existsAsFile())
                                  return;  // 選ばなかったら、そこで中止
                              req->wav = f;
                              safe->askOutput (req);
                          });
}

void MidiStackLensEditor::askOutput (std::shared_ptr<VideoRequest> req)
{
    juce::Component::SafePointer<MidiStackLensEditor> safe (this);
    const auto def = (req->wav.existsAsFile() ? req->wav.getParentDirectory()
                                              : juce::File::getSpecialLocation (juce::File::userDocumentsDirectory))
                         .getChildFile ("MIDI Stack Lens.mp4");
    chooser = std::make_unique<juce::FileChooser> (u8 ("mp4 の保存先"), def, "*.mp4");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [safe, req] (const juce::FileChooser& fc)
                          {
                              auto f = fc.getResult();
                              if (safe == nullptr || f == juce::File())
                                  return;
                              req->out = f.withFileExtension ("mp4");
                              safe->runExport (req);
                          });
}

void MidiStackLensEditor::runExport (std::shared_ptr<VideoRequest> req)
{
    if (exporter != nullptr)
        return;  // 書き出し中

    VideoExporter::Options o;
    o.rec = req->rec;
    o.width = req->width;
    o.height = req->height;
    o.fps = req->fps;
    o.wav = req->wav;
    o.wavOffset = req->wavOffset;
    o.out = req->out;
    o.ffmpeg = req->ffmpeg;

    exporter = std::make_unique<VideoExporter> (proc, std::move (o));

    juce::String error;
    if (! exporter->start (error))
    {
        exporter.reset();
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, u8 ("書き出せません"), error);
        return;
    }

    juce::Component::SafePointer<MidiStackLensEditor> safe (this);
    exporter->onFinished = [safe] (bool ok, const juce::String& message)
    {
        // このコールバックの中で exporter を消さないよう、あとで処理する
        juce::MessageManager::callAsync ([safe, ok, message]
                                         {
                                             if (safe == nullptr)
                                                 return;
                                             if (safe->progressWindow != nullptr)
                                                 safe->progressWindow->exitModalState (-1);
                                             safe->progressWindow.reset();
                                             safe->exporter.reset();
                                             safe->showResult (ok, message);
                                         });
    };

    progressWindow = std::make_unique<juce::AlertWindow> (u8 ("書き出し中…"),
                                                          u8 ("画面を1コマずつ描いて、mp4 にしています。"),
                                                          juce::MessageBoxIconType::NoIcon, this);
    progressWindow->addProgressBarComponent (exporter->progress);
    progressWindow->addButton (u8 ("キャンセル"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
    progressWindow->enterModalState (true,
                                     juce::ModalCallbackFunction::create (
                                         [safe] (int r)
                                         {
                                             if (r == 0 && safe != nullptr && safe->exporter != nullptr)
                                                 safe->exporter->cancel();
                                         }),
                                     false);
}

void MidiStackLensEditor::showResult (bool ok, const juce::String& message)
{
    if (! ok)
    {
        if (message != u8 ("キャンセルしました"))
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, u8 ("書き出せませんでした"),
                                                    message);
        return;
    }

    const juce::File f (message);
    auto* w = new juce::AlertWindow (u8 ("書き出しました"), f.getFullPathName(), juce::MessageBoxIconType::InfoIcon, this);
    w->addButton (u8 ("フォルダで見る"), 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton (u8 ("閉じる"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
    w->enterModalState (true, juce::ModalCallbackFunction::create ([f] (int r) { if (r == 1) f.revealToUser(); }), true);
}

void MidiStackLensEditor::applyTheme (bool dark)
{
    lnf.setColourScheme (dark ? juce::LookAndFeel_V4::getMidnightColourScheme()
                              : juce::LookAndFeel_V4::getLightColourScheme());

    if (! dark)
    {
        const juce::Colour ink (0xff1d2433);
        lnf.setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xfff6f8fc));
        lnf.setColour (juce::ComboBox::outlineColourId, juce::Colour (0xff3d5fd6).withAlpha (0.55f));
        lnf.setColour (juce::ComboBox::textColourId, ink);
        lnf.setColour (juce::ComboBox::arrowColourId, juce::Colour (0xff396cdf));
        lnf.setColour (juce::PopupMenu::backgroundColourId, juce::Colour (0xfff6f8fc));
        lnf.setColour (juce::PopupMenu::textColourId, ink);
        lnf.setColour (juce::PopupMenu::highlightedBackgroundColourId, juce::Colour (0xff396cdf));
        lnf.setColour (juce::PopupMenu::highlightedTextColourId, juce::Colours::white);
        lnf.setColour (juce::ToggleButton::textColourId, ink);
        lnf.setColour (juce::ToggleButton::tickColourId, juce::Colour (0xff396cdf));
        lnf.setColour (juce::ToggleButton::tickDisabledColourId, ink.withAlpha (0.4f));
    }

    sendLookAndFeelChange();
    repaint();
}

void MidiStackLensEditor::paint (juce::Graphics& g) { g.fillAll (view.backgroundColour()); }

void MidiStackLensEditor::resized()
{
    auto area = getLocalBounds();
    auto bar = area.removeFromTop (34).reduced (12, 4);
    view.setBounds (area);

    struct Item { juce::Component* c; float weight; };
    const Item items[] = { { &cbNotation, 86 }, { &cbAccidental, 82 }, { &cbKey, 72 },   { &cbKeyLabels, 100 },
                           { &cbRange, 76 },    { &cbWindow, 84 },     { &cbColor, 96 }, { &cbTheme, 104 },
                           { &tbDegrees, 56 },  { &tbHistory, 92 },    { &tbScale, 80 } };

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
