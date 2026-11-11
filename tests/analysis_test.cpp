// g++ -std=c++17 -I../Source analysis_test.cpp ../Source/Analysis.cpp ../Source/ChordDetector.cpp -o analysis_test && ./analysis_test
#include "Analysis.h"

#include <cstdio>
#include <string>
#include <vector>

static int bad = 0;

static void check (bool ok, const std::string& what, const std::string& got = {})
{
    if (! ok)
        ++bad;
    std::printf ("%s  %s%s%s\n", ok ? "ok  " : "FAIL", what.c_str(), got.empty() ? "" : "  got=", got.c_str());
}

static msl::VoicingInfo voicing (const std::vector<int>& notes)
{
    return msl::analyzeVoicing (notes, msl::detectChord (notes));
}

int main()
{
    using T = msl::VoicingInfo::Type;

    // 音程名
    check (msl::intervalName (3) == "m3", "interval 3 = m3");
    check (msl::intervalName (7) == "P5", "interval 7 = P5");
    check (msl::intervalName (14) == "M9", "interval 14 = M9");
    check (msl::intervalName (30) == "+30", "interval 30 = +30");

    // ボイシングの型
    check (voicing ({ 60, 64, 67, 71 }).type == T::close, "CM7 close");
    check (voicing ({ 55, 60, 64, 71 }).type == T::drop2, "CM7 drop2 (G C E B)");
    check (voicing ({ 52, 60, 67, 71 }).type == T::drop3, "CM7 drop3 (E C G B)");
    check (voicing ({ 48, 55, 64, 71 }).type == T::open, "CM7 open (C G E B)");
    check (voicing ({ 36, 55, 64, 71, 74 }).type == T::spread, "spread over 2 octaves");
    check (voicing ({ 60 }).type == T::single, "single note");

    // 間隔と幅
    {
        auto v = voicing ({ 48, 55, 64, 71 });
        check (v.span == 23 && v.gaps == std::vector<int> ({ 7, 9, 7 }), "gaps 7,9,7 / span 23");
    }

    // 転回
    check (voicing ({ 60, 64, 67 }).inversion == 0, "C root position");
    check (voicing ({ 64, 67, 72 }).inversion == 1, "C/E first inversion");
    check (voicing ({ 67, 72, 76 }).inversion == 2, "C/G second inversion");
    check (voicing ({ 58, 60, 64, 67 }).inversion == 3, "C7/B♭ third inversion");

    // ローインターバルリミット
    check (voicing ({ 36, 40, 43 }).lowIntervalWarning, "C2 E2 G2 is muddy");
    check (! voicing ({ 36, 43, 52, 55 }).lowIntervalWarning, "C2 G2 E3 G3 is fine");
    check (! voicing ({ 60, 64, 67 }).lowIntervalWarning, "C4 triad is fine");

    // キー推定
    {
        std::array<double, 12> w {};
        for (int pc : { 0, 2, 4, 5, 7, 9, 11 })
            w[(size_t) pc] = 1.0;
        w[0] = 3.0;
        w[7] = 2.0;
        w[4] = 2.0;
        auto k = msl::estimateKey (w);
        check (k.valid && k.tonicPc == 0 && ! k.minor, "C major scale -> C major",
               std::to_string (k.tonicPc) + (k.minor ? "m" : ""));
    }
    {
        std::array<double, 12> w {};
        for (int pc : { 9, 11, 0, 2, 4, 5, 7 })
            w[(size_t) pc] = 1.0;
        w[9] = 3.0;
        w[4] = 2.0;
        w[0] = 2.0;
        auto k = msl::estimateKey (w);
        check (k.valid && k.tonicPc == 9 && k.minor, "A minor scale -> A minor",
               std::to_string (k.tonicPc) + (k.minor ? "m" : ""));
    }
    {
        std::array<double, 12> w {};
        w[0] = 1.0;
        check (! msl::estimateKey (w).valid, "one pitch class is not enough");
    }
    {
        // コード進行だけ(CM7 Am7 Dm9 G7/B CM7)でも C メジャーになる
        std::array<double, 12> w {};
        for (int n : { 48, 55, 64, 71, 45, 55, 60, 64, 50, 60, 64, 65, 69, 47, 50, 55, 65, 55, 60, 64, 71 })
            w[(size_t) (n % 12)] += 1.0;
        auto k = msl::estimateKey (w);
        check (k.valid && k.tonicPc == 0 && ! k.minor, "CM7 Am7 Dm9 G7/B CM7 -> C major",
               std::to_string (k.tonicPc) + (k.minor ? "m" : ""));
    }
    {
        // Am Dm E7 Am は A マイナー
        std::array<double, 12> w {};
        for (int n : { 45, 48, 52, 50, 53, 57, 52, 56, 59, 62, 45, 48, 52 })
            w[(size_t) (n % 12)] += 1.0;
        auto k = msl::estimateKey (w);
        check (k.valid && k.tonicPc == 9 && k.minor, "Am Dm E7 Am -> A minor",
               std::to_string (k.tonicPc) + (k.minor ? "m" : ""));
    }

    // ディグリーと機能
    {
        auto g7 = msl::detectChord ({ 55, 59, 62, 65 });
        check (msl::romanNumeral (g7, 0) == "V7", "G7 in C = V7", msl::romanNumeral (g7, 0));
        check (msl::harmonicFunction (g7, 0, false) == "D", "G7 in C = D");

        auto dm7 = msl::detectChord ({ 62, 65, 69, 72 });
        check (msl::romanNumeral (dm7, 0) == "ii7", "Dm7 in C = ii7", msl::romanNumeral (dm7, 0));
        check (msl::harmonicFunction (dm7, 0, false) == "SD", "Dm7 in C = SD");

        auto bb = msl::detectChord ({ 58, 62, 65 });
        check (msl::romanNumeral (bb, 0) == "♭VII", "B♭ in C = ♭VII", msl::romanNumeral (bb, 0));
        check (msl::harmonicFunction (bb, 0, false).empty(), "B♭ in C has no diatonic function");

        auto bm7b5 = msl::detectChord ({ 59, 62, 65, 69 });
        check (msl::romanNumeral (bm7b5, 0) == "viiø7", "Bm7♭5 in C = viiø7", msl::romanNumeral (bm7b5, 0));

        auto cOverE = msl::detectChord ({ 52, 60, 67 });
        check (msl::romanNumeral (cOverE, 0) == "I/3", "C/E in C = I/3", msl::romanNumeral (cOverE, 0));

        auto am = msl::detectChord ({ 57, 60, 64 });
        check (msl::harmonicFunction (am, 9, true) == "T", "Am in A minor = T");
    }

    // スケール
    check (msl::inScale (6, 7, false), "F♯ is in G major");
    check (! msl::inScale (5, 7, false), "F is not in G major");

    std::printf (bad == 0 ? "\nall passed\n" : "\n%d failed\n", bad);
    return bad == 0 ? 0 : 1;
}
