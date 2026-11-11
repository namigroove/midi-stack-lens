#include "Analysis.h"

#include <algorithm>
#include <cmath>

namespace msl
{
namespace
{
int mod12 (int v) { return ((v % 12) + 12) % 12; }

// 2音の間隔(半音)ごとの、これより低いと濁りやすい下限(低い方のノート番号)。
// 一般的なローインターバルリミットの目安(C4 = 60)。
int lowLimitFor (int gap)
{
    switch (gap)
    {
        case 1:  return 52;  // 短2度: E3 より下
        case 2:  return 51;  // 長2度: E♭3 より下
        case 3:  return 48;  // 短3度: C3 より下
        case 4:  return 46;  // 長3度: B♭2 より下
        case 5:  return 41;  // 完全4度: F2 より下
        case 6:  return 46;  // 増4度
        case 7:  return 34;  // 完全5度: B♭1 より下
        case 8:  return 43;  // 短6度
        case 9:  return 41;  // 長6度
        case 10: return 41;  // 短7度
        case 11: return 41;  // 長7度
        default: return -1;  // オクターブ以上は気にしない
    }
}
}  // namespace

std::string intervalName (int s)
{
    static const char* names[] = { "P1", "m2", "M2", "m3", "M3", "P4", "TT", "P5", "m6", "M6", "m7", "M7",
                                   "P8", "m9", "M9", "m10", "M10", "P11", "A11", "P12", "m13", "M13", "m14", "M14",
                                   "P15" };
    if (s < 0)
        s = -s;
    if (s <= 24)
        return names[s];
    return "+" + std::to_string (s);
}

const char* voicingTypeName (VoicingInfo::Type t)
{
    switch (t)
    {
        case VoicingInfo::Type::single:   return "単音";
        case VoicingInfo::Type::interval: return "2音";
        case VoicingInfo::Type::close:    return "クローズ";
        case VoicingInfo::Type::open:     return "オープン";
        case VoicingInfo::Type::drop2:    return "ドロップ2";
        case VoicingInfo::Type::drop3:    return "ドロップ3";
        case VoicingInfo::Type::spread:   return "スプレッド";
        case VoicingInfo::Type::none:     break;
    }
    return "—";
}

VoicingInfo analyzeVoicing (const std::vector<int>& notes, const ChordResult& chord)
{
    VoicingInfo v;
    v.count = (int) notes.size();
    if (notes.empty())
        return v;

    unsigned pcMask = 0;
    for (int n : notes)
        pcMask |= 1u << mod12 (n);
    for (int pc = 0; pc < 12; ++pc)
        if (pcMask & (1u << pc))
            ++v.distinct;

    v.span = notes.back() - notes.front();
    for (size_t i = 1; i < notes.size(); ++i)
        v.gaps.push_back (notes[i] - notes[i - 1]);

    // ローインターバルリミット
    for (size_t i = 1; i < notes.size(); ++i)
    {
        const int gap = notes[i] - notes[i - 1];
        const int limit = lowLimitFor (gap);
        if (limit >= 0 && notes[i - 1] < limit)
        {
            v.lowIntervalWarning = true;
            v.warnLow = notes[i - 1];
            break;
        }
    }

    // 型
    if (v.count == 1)
        v.type = VoicingInfo::Type::single;
    else if (v.distinct <= 2 && v.count == 2)
        v.type = VoicingInfo::Type::interval;
    else if (v.span >= 24)
        v.type = VoicingInfo::Type::spread;
    else if (v.span <= 12)
        v.type = VoicingInfo::Type::close;
    else
    {
        v.type = VoicingInfo::Type::open;

        // 4声で、最低音を1オクターブ上げるとクローズに収まるならドロップ2/3
        if (v.count == 4 && v.distinct == 4)
        {
            const int raised = notes[0] + 12;
            const int top = std::max (raised, notes[3]);
            const int bottom = notes[1];
            if (top - bottom < 12)
            {
                if (raised > notes[2] && raised < notes[3])
                    v.type = VoicingInfo::Type::drop2;
                else if (raised > notes[1] && raised < notes[2])
                    v.type = VoicingInfo::Type::drop3;
            }
        }
    }

    // 転回
    if (chord.named)
    {
        const int bassIv = mod12 (chord.bassPc - chord.rootPc);
        if (bassIv == 0)
            v.inversion = 0;
        else if (bassIv == 3 || bassIv == 4)
            v.inversion = 1;
        else if (bassIv == 6 || bassIv == 7 || bassIv == 8)
            v.inversion = 2;
        else if (bassIv == 9 || bassIv == 10 || bassIv == 11)
            v.inversion = 3;
        else
            v.onChord = true;  // 9th や 11th が最低音
    }

    return v;
}

// ---- キー推定 ------------------------------------------------------------------------
KeyEstimate estimateKey (const std::array<double, 12>& w)
{
    static const double majorProfile[12] = { 6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88 };
    static const double minorProfile[12] = { 6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17 };

    KeyEstimate best;

    double total = 0.0;
    int used = 0;
    for (double x : w)
    {
        total += x;
        if (x > 0.0)
            ++used;
    }
    if (total <= 0.0 || used < 3)
        return best;

    const double meanW = total / 12.0;

    auto correlate = [&] (const double* profile, int tonic)
    {
        double meanP = 0.0;
        for (int i = 0; i < 12; ++i)
            meanP += profile[i];
        meanP /= 12.0;

        double num = 0.0, dw = 0.0, dp = 0.0;
        for (int i = 0; i < 12; ++i)
        {
            const double a = w[(size_t) mod12 (tonic + i)] - meanW;
            const double b = profile[i] - meanP;
            num += a * b;
            dw += a * a;
            dp += b * b;
        }
        const double den = std::sqrt (dw * dp);
        return den > 0.0 ? num / den : 0.0;
    };

    double bestR = -2.0;
    for (int tonic = 0; tonic < 12; ++tonic)
    {
        const double rMaj = correlate (majorProfile, tonic);
        const double rMin = correlate (minorProfile, tonic);
        if (rMaj > bestR)
        {
            bestR = rMaj;
            best.tonicPc = tonic;
            best.minor = false;
        }
        if (rMin > bestR)
        {
            bestR = rMin;
            best.tonicPc = tonic;
            best.minor = true;
        }
    }

    best.confidence = std::max (0.0, bestR);
    best.valid = bestR >= 0.55;
    return best;
}

bool inScale (int pc, int tonicPc, bool minorKey)
{
    static const unsigned majorMask = (1u << 0) | (1u << 2) | (1u << 4) | (1u << 5) | (1u << 7) | (1u << 9) | (1u << 11);
    static const unsigned minorMask = (1u << 0) | (1u << 2) | (1u << 3) | (1u << 5) | (1u << 7) | (1u << 8) | (1u << 10);
    return ((minorKey ? minorMask : majorMask) >> mod12 (pc - tonicPc)) & 1u;
}

// ---- ディグリー ----------------------------------------------------------------------
std::string romanNumeral (const ChordResult& c, int tonicPc)
{
    if (! c.named)
        return {};

    static const char* upper[12] = { "I", "♭II", "II", "♭III", "III", "IV", "♯IV", "V", "♭VI", "VI", "♭VII", "VII" };
    static const char* lower[12] = { "i", "♭ii", "ii", "♭iii", "iii", "iv", "♯iv", "v", "♭vi", "vi", "♭vii", "vii" };

    const int iv = mod12 (c.rootPc - tonicPc);
    const std::string& s = c.suffix;

    auto startsWith = [&] (const char* p) { return s.rfind (p, 0) == 0; };

    std::string out;
    if (s == "m7♭5")
        out = std::string (lower[iv]) + "ø7";
    else if (s == "dim7")
        out = std::string (lower[iv]) + "°7";
    else if (s == "dim")
        out = std::string (lower[iv]) + "°";
    else if (startsWith ("mM7"))
        out = std::string (lower[iv]) + s.substr (1);
    else if (startsWith ("m") && ! startsWith ("mM"))
        out = std::string (lower[iv]) + s.substr (1);
    else
        out = std::string (upper[iv]) + s;

    if (c.isSlash())
    {
        const int biv = mod12 (c.bassPc - tonicPc);
        static const char* bassDeg[12] = { "1", "♭2", "2", "♭3", "3", "4", "♯4", "5", "♭6", "6", "♭7", "7" };
        out += std::string ("/") + bassDeg[biv];
    }
    return out;
}

std::string harmonicFunction (const ChordResult& c, int tonicPc, bool minorKey)
{
    if (! c.named)
        return {};

    const int iv = mod12 (c.rootPc - tonicPc);
    const std::string& s = c.suffix;
    const bool minorChord = s.rfind ("m", 0) == 0;  // m, m7, mM7, m7♭5 …
    const bool dimChord = s.rfind ("dim", 0) == 0 || s == "m7♭5";
    const bool dominant7 = ! minorChord && ! dimChord
                           && (s == "7" || s == "9" || s == "13" || s.rfind ("7", 0) == 0 || s.rfind ("9", 0) == 0);

    if (! minorKey)
    {
        switch (iv)
        {
            case 0:  return (! minorChord && ! dimChord && ! dominant7) ? "T" : "";
            case 2:  return (minorChord && ! dimChord) ? "SD" : "";
            case 4:  return (minorChord && ! dimChord) ? "T" : "";
            case 5:  return (! minorChord && ! dimChord) ? "SD" : "";
            case 7:  return (! minorChord && ! dimChord) ? "D" : "";
            case 9:  return (minorChord && ! dimChord) ? "T" : "";
            case 11: return dimChord ? "D" : "";
            default: return "";
        }
    }

    switch (iv)
    {
        case 0:  return (minorChord && ! dimChord) ? "T" : "";
        case 2:  return dimChord ? "SD" : "";
        case 3:  return (! minorChord && ! dimChord) ? "T" : "";
        case 5:  return (minorChord && ! dimChord) ? "SD" : "";
        case 7:  return dimChord ? "" : "D";               // v も V7 もドミナント
        case 8:  return (! minorChord && ! dimChord) ? "SD" : "";
        case 10: return (! minorChord && ! dimChord) ? "D" : "";
        case 11: return dimChord ? "D" : "";
        default: return "";
    }
}
}  // namespace msl
