// g++ -std=c++17 -I../Source chord_test.cpp ../Source/ChordDetector.cpp -o chord_test && ./chord_test
#include "ChordDetector.h"

#include <cstdio>
#include <string>
#include <vector>

static const char* kNames[12] = { "C", "D♭", "D", "E♭", "E", "F", "G♭", "G", "A♭", "A", "B♭", "B" };

static std::string nameOf (const std::vector<int>& notes)
{
    auto r = msl::detectChord (notes);
    if (! r.named)
        return "(none)";
    std::string s = std::string (kNames[r.rootPc]) + r.suffix;
    if (r.isSlash())
        s += std::string ("/") + kNames[r.bassPc];
    return s;
}

int main()
{
    struct Case { std::vector<int> notes; const char* expect; };
    const Case cases[] = {
        { { 60, 64, 67 },            "C" },
        { { 60, 63, 67 },            "Cm" },
        { { 60, 64, 67, 71 },        "CM7" },
        { { 60, 64, 67, 70 },        "C7" },
        { { 57, 60, 64, 67 },        "Am7" },       // 最低音 A → Am7
        { { 48, 64, 67, 69 },        "C6" },        // 最低音 C → C6(Am7/C ではなく)
        { { 55, 59, 62, 65 },        "G7" },
        { { 47, 50, 55, 65 },        "G7/B" },      // B D G F → 最低音B
        { { 60, 64, 71, 74 },        "CM9" },       // 5度省略
        { { 60, 63, 67, 70, 74 },    "Cm9" },
        { { 62, 65, 69, 72, 76 },    "Dm9" },
        { { 60, 63, 66, 69 },        "Cdim7" },
        { { 60, 63, 66, 70 },        "Cm7♭5" },
        { { 60, 65, 67 },            "Csus4" },
        { { 60, 64, 67, 69, 74 },    "C6/9" },
        { { 60, 67 },                "C5" },
        { { 60, 64, 70, 75 },        "C7♯9" },      // 5度省略
        { { 60, 64, 67, 70, 73 },    "C7♭9" },
        { { 60, 64, 70, 74, 81 },    "C13" },
        { { 60, 64, 68 },            "Caug" },
        { { 60, 64 },                "(none)" },    // 2音(長3度)は名前なし
        { { 60 },                    "(none)" },
        { { 60, 72 },                "(none)" },
    };

    int bad = 0;
    for (const auto& c : cases)
    {
        const std::string got = nameOf (c.notes);
        const bool ok = got == c.expect;
        if (! ok)
            ++bad;
        std::printf ("%s  expect=%-8s got=%s\n", ok ? "ok  " : "FAIL", c.expect, got.c_str());
    }

    // 度数ラベル
    auto r = msl::detectChord ({ 60, 64, 67, 70 });
    const bool deg = r.degree[0] == "R" && r.degree[4] == "3" && r.degree[7] == "5" && r.degree[10] == "♭7";
    std::printf ("%s  degrees of C7\n", deg ? "ok  " : "FAIL");
    if (! deg)
        ++bad;

    std::printf (bad == 0 ? "\nall passed\n" : "\n%d failed\n", bad);
    return bad == 0 ? 0 : 1;
}
