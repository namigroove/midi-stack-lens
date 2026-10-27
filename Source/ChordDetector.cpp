#include "ChordDetector.h"

#include <algorithm>
#include <climits>
#include <initializer_list>

namespace msl
{
namespace
{
unsigned mk (std::initializer_list<int> v)
{
    unsigned r = 0;
    for (int i : v)
        r |= 1u << i;
    return r;
}

int popcount (unsigned v)
{
    int n = 0;
    for (; v; v &= v - 1)
        ++n;
    return n;
}

struct Template
{
    const char* suffix;
    unsigned mask;      // ルートを0とした半音の集合
    unsigned optional;  // 省略してよい音(多くは5度)
    int cost;           // 複雑さ。小さいほど優先
};

const std::vector<Template>& templates()
{
    const unsigned p5 = 1u << 7;
    static const std::vector<Template> t = {
        // 3和音など
        { "",       mk ({ 0, 4, 7 }), 0, 0 },
        { "m",      mk ({ 0, 3, 7 }), 0, 0 },
        { "dim",    mk ({ 0, 3, 6 }), 0, 1 },
        { "aug",    mk ({ 0, 4, 8 }), 0, 1 },
        { "sus4",   mk ({ 0, 5, 7 }), 0, 1 },
        { "sus2",   mk ({ 0, 2, 7 }), 0, 1 },
        { "5",      mk ({ 0, 7 }),    0, 2 },
        // 4和音
        { "6",      mk ({ 0, 4, 7, 9 }),  p5, 1 },
        { "m6",     mk ({ 0, 3, 7, 9 }),  p5, 1 },
        { "7",      mk ({ 0, 4, 7, 10 }), p5, 1 },
        { "M7",     mk ({ 0, 4, 7, 11 }), p5, 1 },
        { "m7",     mk ({ 0, 3, 7, 10 }), p5, 1 },
        { "mM7",    mk ({ 0, 3, 7, 11 }), p5, 2 },
        { "m7♭5",   mk ({ 0, 3, 6, 10 }), 0,  1 },
        { "dim7",   mk ({ 0, 3, 6, 9 }),  0,  1 },
        { "7sus4",  mk ({ 0, 5, 7, 10 }), p5, 2 },
        { "7♭5",    mk ({ 0, 4, 6, 10 }), 0,  2 },
        { "7♯5",    mk ({ 0, 4, 8, 10 }), 0,  2 },
        { "M7♯5",   mk ({ 0, 4, 8, 11 }), 0,  2 },
        { "add9",   mk ({ 0, 2, 4, 7 }),  p5, 2 },
        { "madd9",  mk ({ 0, 2, 3, 7 }),  p5, 2 },
        // テンション入り
        { "6/9",    mk ({ 0, 2, 4, 7, 9 }),  p5, 3 },
        { "m6/9",   mk ({ 0, 2, 3, 7, 9 }),  p5, 3 },
        { "9",      mk ({ 0, 2, 4, 7, 10 }), p5, 2 },
        { "M9",     mk ({ 0, 2, 4, 7, 11 }), p5, 2 },
        { "m9",     mk ({ 0, 2, 3, 7, 10 }), p5, 2 },
        { "9sus4",  mk ({ 0, 2, 5, 7, 10 }), p5, 3 },
        { "7♭9",    mk ({ 0, 1, 4, 7, 10 }), p5, 2 },
        { "7♯9",    mk ({ 0, 3, 4, 7, 10 }), p5, 2 },
        { "7♯11",   mk ({ 0, 4, 6, 7, 10 }), 0,  3 },
        { "M7(♯11)", mk ({ 0, 4, 6, 7, 11 }), 0, 3 },
        { "7♭13",   mk ({ 0, 4, 7, 8, 10 }), 0,  3 },
        { "m7(11)", mk ({ 0, 3, 5, 7, 10 }), p5, 3 },
        // 6音までの拡張
        { "m11",    mk ({ 0, 2, 3, 5, 7, 10 }), p5, 3 },
        { "13",     mk ({ 0, 2, 4, 7, 9, 10 }), p5 | (1u << 2), 3 },
        { "M13",    mk ({ 0, 2, 4, 7, 9, 11 }), p5 | (1u << 2), 3 },
        { "m13",    mk ({ 0, 2, 3, 7, 9, 10 }), p5 | (1u << 2), 3 },
    };
    return t;
}

// ルートからの半音数 → 度数ラベル(文脈で書き分ける)
std::string degreeLabel (int i, unsigned S)
{
    const bool maj3 = S & (1u << 4);
    const bool min3 = S & (1u << 3);
    const bool noThird = !maj3 && !min3;
    const bool p5 = S & (1u << 7);
    const bool seventh = S & ((1u << 10) | (1u << 11));
    const bool dim7 = min3 && (S & (1u << 6)) && (S & (1u << 9)) && !p5;

    switch (i)
    {
        case 0:  return "R";
        case 1:  return "♭9";
        case 2:  return noThird ? "2" : "9";
        case 3:  return maj3 ? "♯9" : "♭3";
        case 4:  return "3";
        case 5:  return noThird ? "4" : "11";
        case 6:  return p5 ? "♯11" : "♭5";
        case 7:  return "5";
        case 8:  return p5 ? "♭13" : (maj3 ? "♯5" : "♭6");
        case 9:  return dim7 ? "♭♭7" : (seventh ? "13" : "6");
        case 10: return "♭7";
        default: return "7";
    }
}

void fillDegrees (ChordResult& r, int root, unsigned pcMask)
{
    unsigned S = 0;
    for (int pc = 0; pc < 12; ++pc)
        if (pcMask & (1u << pc))
            S |= 1u << ((pc - root + 12) % 12);

    for (int i = 0; i < 12; ++i)
        if (S & (1u << i))
            r.degree[(size_t) ((root + i) % 12)] = degreeLabel (i, S);

    r.hasDegrees = true;
}
}  // namespace

ChordResult detectChord (const std::vector<int>& notes)
{
    ChordResult r;
    if (notes.size() < 2)
        return r;

    const int lowest = *std::min_element (notes.begin(), notes.end());
    const int bass = ((lowest % 12) + 12) % 12;

    unsigned pcMask = 0;
    for (int n : notes)
        pcMask |= 1u << (((n % 12) + 12) % 12);

    r.bassPc = bass;
    r.rootPc = bass;

    if (popcount (pcMask) < 2)
        return r;  // 同じ音名だけ(オクターブ重ね)

    int bestCost = INT_MAX;
    int bestRoot = -1;
    const Template* best = nullptr;

    for (int root = 0; root < 12; ++root)
    {
        if (! (pcMask & (1u << root)))
            continue;

        unsigned S = 0;
        for (int pc = 0; pc < 12; ++pc)
            if (pcMask & (1u << pc))
                S |= 1u << ((pc - root + 12) % 12);

        for (const auto& t : templates())
        {
            if (S & ~t.mask)
                continue;  // テンプレートに無い音がある

            const unsigned required = t.mask & ~t.optional;
            if (required & ~S)
                continue;  // 必須の音が足りない

            const int cost = t.cost + popcount (t.mask & ~S) + (root == bass ? 0 : 3);
            if (cost < bestCost)
            {
                bestCost = cost;
                bestRoot = root;
                best = &t;
            }
        }
    }

    if (best != nullptr)
    {
        r.named = true;
        r.rootPc = bestRoot;
        r.suffix = best->suffix;
    }

    fillDegrees (r, r.rootPc, pcMask);  // 名前が付かないときは最低音を基準にする
    return r;
}
}  // namespace msl
