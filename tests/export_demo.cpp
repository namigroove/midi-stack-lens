// 画面なしで、デモの演奏を mp4 にする動作確認用ツール。
//   MslExportDemo <out.mp4> [land|portrait|square] [fps] [wav]
#include <juce_audio_utils/juce_audio_utils.h>

#include <cstdio>

#include "PluginProcessor.h"
#include "VideoExport.h"

namespace
{
msl::Recording demoRecording()
{
    struct Step { std::vector<int> notes; std::vector<int> vels; };
    const std::vector<Step> steps = {
        { { 48, 55, 64, 71 }, { 84, 70, 76, 98 } },        // CM7
        { { 45, 55, 60, 64 }, { 80, 66, 72, 94 } },        // Am7
        { { 50, 60, 64, 65, 69 }, { 86, 64, 70, 72, 100 } },  // Dm9
        { { 47, 50, 55, 65 }, { 82, 68, 70, 96 } },        // G7/B
        { { 55, 60, 64, 71 }, { 78, 70, 74, 104 } },       // CM7 drop2
    };

    msl::Recording r;
    r.bpm = 120.0;
    r.startPpq = 0.0;
    const double beat = 0.5;
    double t = 0.5;
    for (int round = 0; round < 2; ++round)
        for (const auto& s : steps)
        {
            for (size_t i = 0; i < s.notes.size(); ++i)
                r.events.push_back ({ t + 0.02 * (double) i, t * 2.0, (uint8_t) s.notes[i], (uint8_t) s.vels[i], 1 });
            for (int n : s.notes)
                r.events.push_back ({ t + 2.0 * beat - 0.05, (t + 2.0 * beat) * 2.0, (uint8_t) n, 0, 0 });
            t += 2.0 * beat;
        }
    r.duration = t + 0.5;
    for (double s = 0.0; s < r.duration; s += 0.05)
        r.keys.push_back ({ s, s * 2.0, std::floor (s * 2.0 / 4.0) * 4.0, 120.0, 4, 4, true });
    return r;
}
}  // namespace

int main (int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf ("usage: MslExportDemo <out.mp4> [land|portrait|square] [fps] [wav]\n");
        return 2;
    }

    juce::ScopedJuceInitialiser_GUI init;
    MidiStackLensProcessor proc;

    VideoExporter::Options o;
    o.rec = demoRecording();
    o.out = juce::File::getCurrentWorkingDirectory().getChildFile (argv[1]);
    const juce::String shape = argc > 2 ? argv[2] : "land";
    o.width = shape == "portrait" ? 1080 : shape == "square" ? 1080 : 1920;
    o.height = shape == "portrait" ? 1920 : shape == "square" ? 1080 : 1080;
    o.fps = argc > 3 ? juce::String (argv[3]).getIntValue() : 30;
    if (argc > 4)
    {
        o.wav = juce::File::getCurrentWorkingDirectory().getChildFile (argv[4]);
        o.wavOffset = 0.0;
    }
    o.ffmpeg = VideoExporter::findFfmpeg ({});

    VideoExporter ex (proc, std::move (o));
    bool finished = false, ok = false;
    juce::String msg;
    ex.onFinished = [&] (bool k, const juce::String& m)
    {
        ok = k;
        msg = m;
        finished = true;
        juce::MessageManager::callAsync ([] { juce::MessageManager::getInstance()->stopDispatchLoop(); });
    };

    juce::String err;
    if (! ex.start (err))
    {
        std::printf ("start failed: %s\n", err.toRawUTF8());
        return 1;
    }

    juce::MessageManager::getInstance()->runDispatchLoop();

    std::printf ("%s: %s\n", ok ? "ok" : "FAILED", msg.toRawUTF8());
    return ok ? 0 : 1;
}
