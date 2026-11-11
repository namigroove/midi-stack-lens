// 記録・.mid・動画の計算の単体テスト(JUCE なし)
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "Recording.h"

static int failures = 0;

#define CHECK(cond)                                                                     \
    do                                                                                  \
    {                                                                                   \
        if (! (cond))                                                                   \
        {                                                                               \
            std::printf ("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                \
            ++failures;                                                                 \
        }                                                                               \
    } while (0)

struct Ev
{
    unsigned tick;
    int status;  // 0x90 / 0x80 / 0xff
    int a, b;
    unsigned tempoUs = 0;
};

// 最小限の SMF パーサ
static bool parse (const std::vector<uint8_t>& d, int& tpq, std::vector<Ev>& out)
{
    if (d.size() < 22 || std::string (d.begin(), d.begin() + 4) != "MThd")
        return false;
    tpq = (d[12] << 8) | d[13];
    size_t p = 14;
    if (std::string (d.begin() + (long) p, d.begin() + (long) p + 4) != "MTrk")
        return false;
    const size_t len = ((size_t) d[p + 4] << 24) | ((size_t) d[p + 5] << 16) | ((size_t) d[p + 6] << 8) | d[p + 7];
    p += 8;
    const size_t end = p + len;
    if (end != d.size())
        return false;

    unsigned tick = 0;
    while (p < end)
    {
        unsigned delta = 0;
        while (true)
        {
            const uint8_t c = d[p++];
            delta = (delta << 7) | (c & 0x7f);
            if (! (c & 0x80))
                break;
        }
        tick += delta;
        const int st = d[p++];
        if (st == 0xff)
        {
            const int type = d[p++];
            const int n = d[p++];
            Ev e { tick, 0xff, type, n };
            if (type == 0x51)
                e.tempoUs = ((unsigned) d[p] << 16) | ((unsigned) d[p + 1] << 8) | d[p + 2];
            p += (size_t) n;
            out.push_back (e);
        }
        else
        {
            out.push_back ({ tick, st & 0xf0, d[p], d[p + 1] });
            p += 2;
        }
    }
    return true;
}

static int count (const std::vector<Ev>& v, int status)
{
    return (int) std::count_if (v.begin(), v.end(), [&] (const Ev& e) { return e.status == status; });
}

static void testMidiBasic()
{
    msl::Recording r;
    r.bpm = 120.0;
    r.duration = 3.0;
    r.events = { { 0.5, -1, 60, 90, 1 }, { 0.5, -1, 64, 80, 1 }, { 1.0, -1, 60, 0, 0 }, { 1.0, -1, 64, 0, 0 },
                 { 1.5, -1, 67, 70, 1 } };  // 67 は鳴りっぱなし → 最後に止まる

    int tpq = 0;
    std::vector<Ev> ev;
    CHECK (parse (msl::toMidiFile (r), tpq, ev));
    CHECK (tpq == 480);
    CHECK (count (ev, 0x90) == 3);
    CHECK (count (ev, 0x80) == 3);

    // 120 BPM: 0.5 秒 = 1 拍 = 480 tick
    bool tempoOk = false, onOk = false, offOk = false, endOk = false;
    for (const auto& e : ev)
    {
        if (e.status == 0xff && e.a == 0x51 && e.tempoUs == 500000)
            tempoOk = true;
        if (e.status == 0x90 && e.a == 60 && e.tick == 480)
            onOk = true;
        if (e.status == 0x80 && e.a == 60 && e.tick == 960)
            offOk = true;
        if (e.status == 0x80 && e.a == 67 && e.tick == 2880)  // 3.0 秒 = 6 拍
            endOk = true;
    }
    CHECK (tempoOk);
    CHECK (onOk);
    CHECK (offOk);
    CHECK (endOk);

    // 最後は End of Track
    CHECK (ev.back().status == 0xff && ev.back().a == 0x2f);
}

static void testMidiRetriggerAndAllOff()
{
    msl::Recording r;
    r.duration = 2.0;
    r.events = { { 0.0, -1, 60, 90, 1 }, { 0.5, -1, 60, 80, 1 },  // 離さずに再度ノートオン → 間にオフが入る
                 { 1.0, -1, 62, 70, 1 }, { 1.5, -1, 0, 0, 2 } };   // 全ノートオフ

    int tpq = 0;
    std::vector<Ev> ev;
    CHECK (parse (msl::toMidiFile (r), tpq, ev));
    CHECK (count (ev, 0x90) == 3);
    CHECK (count (ev, 0x80) == 3);  // 60(再押し) + 60(全オフ) + 62(全オフ)

    // 同じ時刻ではノートオフがノートオンより前
    for (size_t i = 0; i + 1 < ev.size(); ++i)
        if (ev[i].tick == ev[i + 1].tick && ev[i].status == 0x90 && ev[i + 1].status == 0x80)
            CHECK (false);
}

static void testMidiTempoChange()
{
    msl::Recording r;
    r.bpm = 120.0;
    r.duration = 4.0;
    r.keys = { { 0.0, 0.0, 0.0, 120.0, 4, 4, true }, { 1.0, 2.0, 0.0, 60.0, 4, 4, true } };
    // 1 秒まで 120 BPM(2 拍 = 960 tick)、そのあと 60 BPM(1 秒 = 1 拍 = 480 tick)
    r.events = { { 2.0, -1, 60, 90, 1 }, { 3.0, -1, 60, 0, 0 } };

    int tpq = 0;
    std::vector<Ev> ev;
    CHECK (parse (msl::toMidiFile (r), tpq, ev));
    int tempos = 0;
    for (const auto& e : ev)
    {
        if (e.status == 0xff && e.a == 0x51)
            ++tempos;
        if (e.status == 0x90)
            CHECK (e.tick == 960 + 480);
        if (e.status == 0x80)
            CHECK (e.tick == 960 + 960);
    }
    CHECK (tempos == 2);
}

static void testTransport()
{
    msl::Recording r;
    r.bpm = 120.0;
    r.keys = { { 0.0, 4.0, 4.0, 120.0, 4, 4, true }, { 1.0, 6.0, 4.0, 120.0, 4, 4, true } };

    auto t0 = msl::transportAt (r, 0.5);  // 0.5 秒 = 1 拍進む
    CHECK (std::abs (t0.ppq - 5.0) < 1e-9);
    CHECK (std::abs (t0.barStartPpq - 4.0) < 1e-9);

    auto t1 = msl::transportAt (r, 1.75);  // 1.0 秒のキー(ppq 6)から 0.75 秒 = 1.5 拍 → 7.5
    CHECK (std::abs (t1.ppq - 7.5) < 1e-9);

    auto t2 = msl::transportAt (r, 2.5);  // 6 + 3 = 9 → 次の小節(8〜)に入っている
    CHECK (std::abs (t2.barStartPpq - 8.0) < 1e-9);

    msl::Recording none;
    CHECK (msl::transportAt (none, 1.0).ppq < 0.0);

    // 止まっているときは進めない
    msl::Recording stopped;
    stopped.keys = { { 0.0, 4.0, 4.0, 120.0, 4, 4, false } };
    CHECK (std::abs (msl::transportAt (stopped, 3.0).ppq - 4.0) < 1e-9);
}

static void testExportMath()
{
    msl::Recording r;
    r.duration = 10.0;
    r.bpm = 120.0;
    r.startPpq = 8.0;  // 3 小節目あたりから録音
    CHECK (std::abs (msl::defaultWavOffset (r) - 4.0) < 1e-9);
    r.startPpq = -1.0;
    CHECK (msl::defaultWavOffset (r) == 0.0);

    CHECK (std::abs (msl::exportDuration (r, 0.0, 0.0) - 11.0) < 1e-9);
    CHECK (std::abs (msl::exportDuration (r, 30.0, 4.0) - 26.0) < 1e-9);  // WAV がずっと長い
    CHECK (std::abs (msl::exportDuration (r, 5.0, 0.0) - 11.0) < 1e-9);   // WAV のほうが短い

    CHECK (msl::frameCountFor (1.0, 30) == 30);
    CHECK (msl::frameCountFor (1.01, 30) == 31);
    CHECK (msl::frameCountFor (0.0, 30) == 1);
}

static void testFfmpegArgs()
{
    auto has = [] (const std::vector<std::string>& a, const std::string& s)
    { return std::find (a.begin(), a.end(), s) != a.end(); };

    auto a = msl::ffmpegArgs ("/tmp/fr", 30, 1080, 1920, "", 0.0, 12.0, "/tmp/out.mp4");
    CHECK (has (a, "libx264"));
    CHECK (has (a, "1080x1920"));
    CHECK (! has (a, "-c:a"));
    CHECK (a.back() == "/tmp/out.mp4");

    auto b = msl::ffmpegArgs ("/tmp/fr", 60, 1920, 1080, "/tmp/a.wav", 4.0, 12.0, "/tmp/out.mp4");
    CHECK (has (b, "-ss") && has (b, "aac") && has (b, "/tmp/a.wav"));
    CHECK (! has (b, "-itsoffset"));

    auto c = msl::ffmpegArgs ("/tmp/fr", 60, 1920, 1080, "/tmp/a.wav", -2.0, 12.0, "/tmp/out.mp4");
    CHECK (has (c, "-itsoffset") && ! has (c, "-ss"));
}

int main()
{
    testMidiBasic();
    testMidiRetriggerAndAllOff();
    testMidiTempoChange();
    testTransport();
    testExportMath();
    testFfmpegArgs();

    if (failures == 0)
        std::printf ("recording_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
