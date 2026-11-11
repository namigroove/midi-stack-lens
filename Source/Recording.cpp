#include "Recording.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace msl
{
namespace
{
void putVlq (std::vector<uint8_t>& out, uint32_t v)
{
    uint8_t buf[5];
    int n = 0;
    buf[n++] = (uint8_t) (v & 0x7f);
    while ((v >>= 7) != 0)
        buf[n++] = (uint8_t) ((v & 0x7f) | 0x80);
    while (n > 0)
        out.push_back (buf[--n]);
}

void put32 (std::vector<uint8_t>& out, uint32_t v)
{
    for (int s = 24; s >= 0; s -= 8)
        out.push_back ((uint8_t) ((v >> s) & 0xff));
}

void put16 (std::vector<uint8_t>& out, uint32_t v)
{
    out.push_back ((uint8_t) ((v >> 8) & 0xff));
    out.push_back ((uint8_t) (v & 0xff));
}

struct TempoSeg
{
    double sec;
    double bpm;
    double tick0;
};

struct Item
{
    uint32_t tick;
    int order;  // 同じ時刻なら テンポ → ノートオフ → ノートオン の順
    std::vector<uint8_t> bytes;
};

int log2Int (int v)
{
    int r = 0;
    while (v > 1)
    {
        v >>= 1;
        ++r;
    }
    return r;
}

std::string num (double v)
{
    char buf[64];
    std::snprintf (buf, sizeof buf, "%.6f", v);
    return buf;
}
}  // namespace

std::vector<uint8_t> toMidiFile (const Recording& rec, int tpq)
{
    const double bpm0 = std::clamp (rec.bpm, 20.0, 400.0);

    std::vector<TempoSeg> segs { { 0.0, bpm0, 0.0 } };
    for (const auto& k : rec.keys)
    {
        const double b = std::clamp (k.bpm, 20.0, 400.0);
        if (std::abs (b - segs.back().bpm) > 0.01 && k.sec > segs.back().sec)
        {
            const auto& s = segs.back();
            segs.push_back ({ k.sec, b, s.tick0 + (k.sec - s.sec) * s.bpm / 60.0 * tpq });
        }
    }

    auto secToTick = [&] (double sec) -> uint32_t
    {
        sec = std::max (0.0, sec);
        const TempoSeg* s = &segs.front();
        for (const auto& c : segs)
            if (c.sec <= sec)
                s = &c;
        return (uint32_t) std::llround (s->tick0 + (sec - s->sec) * s->bpm / 60.0 * tpq);
    };

    std::vector<Item> items;

    auto tempoMeta = [] (double bpm)
    {
        const uint32_t us = (uint32_t) std::llround (60000000.0 / bpm);
        return std::vector<uint8_t> { 0xff, 0x51, 0x03, (uint8_t) ((us >> 16) & 0xff), (uint8_t) ((us >> 8) & 0xff),
                                      (uint8_t) (us & 0xff) };
    };

    for (const auto& s : segs)
        items.push_back ({ secToTick (s.sec), 0, tempoMeta (s.bpm) });

    const int den = std::max (1, rec.den);
    items.push_back ({ 0, 0,
                       { 0xff, 0x58, 0x04, (uint8_t) std::clamp (rec.num, 1, 255), (uint8_t) log2Int (den), 24, 8 } });

    std::array<bool, 128> held {};
    auto noteOff = [&] (uint32_t tick, int n)
    {
        items.push_back ({ tick, 1, { (uint8_t) 0x80, (uint8_t) n, 0x40 } });
        held[(size_t) n] = false;
    };

    for (const auto& e : rec.events)
    {
        const uint32_t tick = secToTick (e.sec);
        const int n = std::min<int> (127, e.note);
        if (e.kind == 1)
        {
            if (held[(size_t) n])
                noteOff (tick, n);
            items.push_back ({ tick, 2, { (uint8_t) 0x90, (uint8_t) n, (uint8_t) std::clamp<int> (e.vel, 1, 127) } });
            held[(size_t) n] = true;
        }
        else if (e.kind == 0)
        {
            if (held[(size_t) n])
                noteOff (tick, n);
        }
        else
        {
            for (int i = 0; i < 128; ++i)
                if (held[(size_t) i])
                    noteOff (tick, i);
        }
    }

    const uint32_t endTick = std::max<uint32_t> (secToTick (rec.duration), items.empty() ? 0 : items.back().tick);
    for (int i = 0; i < 128; ++i)
        if (held[(size_t) i])
            noteOff (endTick, i);

    std::stable_sort (items.begin(), items.end(), [] (const Item& a, const Item& b)
                      { return a.tick != b.tick ? a.tick < b.tick : a.order < b.order; });

    std::vector<uint8_t> track;
    uint32_t last = 0;
    for (const auto& it : items)
    {
        putVlq (track, it.tick - last);
        last = it.tick;
        track.insert (track.end(), it.bytes.begin(), it.bytes.end());
    }
    putVlq (track, 0);
    track.insert (track.end(), { 0xff, 0x2f, 0x00 });

    std::vector<uint8_t> out { 'M', 'T', 'h', 'd' };
    put32 (out, 6);
    put16 (out, 0);
    put16 (out, 1);
    put16 (out, (uint32_t) tpq);
    out.insert (out.end(), { 'M', 'T', 'r', 'k' });
    put32 (out, (uint32_t) track.size());
    out.insert (out.end(), track.begin(), track.end());
    return out;
}

TransportKey transportAt (const Recording& rec, double sec)
{
    TransportKey t;
    t.sec = sec;
    t.bpm = rec.bpm;
    t.num = rec.num;
    t.den = rec.den;

    if (rec.keys.empty())
        return t;

    auto it = std::upper_bound (rec.keys.begin(), rec.keys.end(), sec,
                                [] (double s, const TransportKey& k) { return s < k.sec; });
    const TransportKey& k = it == rec.keys.begin() ? *it : *(it - 1);

    t = k;
    t.sec = sec;
    if (k.ppq >= 0.0)
    {
        if (k.playing && sec > k.sec)
            t.ppq = k.ppq + (sec - k.sec) * std::clamp (k.bpm, 20.0, 400.0) / 60.0;

        const double barLen = 4.0 / (double) std::max (1, k.den) * (double) std::max (1, k.num);
        if (t.barStartPpq < 0.0 || t.ppq < t.barStartPpq || t.ppq >= t.barStartPpq + barLen)
            t.barStartPpq = std::floor (t.ppq / barLen + 1.0e-9) * barLen;
    }
    return t;
}

double defaultWavOffset (const Recording& rec)
{
    if (rec.startPpq < 0.0)
        return 0.0;
    const double sec = rec.startPpq * 60.0 / std::clamp (rec.bpm, 20.0, 400.0);
    return std::round (sec * 1000.0) / 1000.0;
}

double exportDuration (const Recording& rec, double wavSeconds, double wavOffset, double tail)
{
    double d = rec.duration + tail;
    if (wavSeconds > 0.0)
        d = std::max (d, wavSeconds - wavOffset);
    return std::max (d, 0.5);
}

int frameCountFor (double durationSec, int fps)
{
    return std::max (1, (int) std::ceil (durationSec * (double) fps - 1.0e-9));
}

std::vector<std::string> ffmpegArgs (const std::string& frameDir, int fps, int width, int height,
                                     const std::string& wavPath, double wavOffset, double durationSec,
                                     const std::string& outPath)
{
    std::vector<std::string> a { "-y", "-hide_banner", "-loglevel", "error", "-framerate", std::to_string (fps),
                                 "-i", frameDir + "/f_%06d.png" };
    const bool hasAudio = ! wavPath.empty();
    if (hasAudio)
    {
        if (wavOffset > 0.001)
        {
            a.push_back ("-ss");
            a.push_back (num (wavOffset));
        }
        else if (wavOffset < -0.001)
        {
            a.push_back ("-itsoffset");
            a.push_back (num (-wavOffset));
        }
        a.push_back ("-i");
        a.push_back (wavPath);
    }

    a.insert (a.end(), { "-map", "0:v" });
    if (hasAudio)
        a.insert (a.end(), { "-map", "1:a" });

    a.insert (a.end(), { "-t", num (durationSec), "-c:v", "libx264", "-preset", "veryfast", "-crf", "18",
                         "-pix_fmt", "yuv420p", "-r", std::to_string (fps), "-s",
                         std::to_string (width) + "x" + std::to_string (height) });
    if (hasAudio)
        a.insert (a.end(), { "-c:a", "aac", "-b:a", "192k" });

    a.insert (a.end(), { "-movflags", "+faststart", outPath });
    return a;
}
}  // namespace msl
