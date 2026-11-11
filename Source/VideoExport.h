#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include <atomic>
#include <functional>
#include <memory>

#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "Recording.h"

// 記録したMIDIから、一定のコマ数で画面を描いて mp4 にする。
//   1. 仮想の時計で画面(MidiStackView)を1コマずつ描き、PNG の連番にする(DAWの再生とは無関係なのでコマ落ちしない)
//   2. 自分のPCに入っている ffmpeg で mp4 にまとめる(WAV を指定すれば音も合体)
// ffmpeg は同梱しない。見つからないときは onFinished で知らせる。
class VideoExporter : private juce::Timer
{
public:
    struct Options
    {
        msl::Recording rec;
        int width = 1920;
        int height = 1080;
        int fps = 30;
        juce::File wav;           // 空なら音なし
        double wavOffset = 0.0;   // WAV の頭が、録音の先頭より何秒前か(負なら後)
        juce::File out;
        juce::File ffmpeg;
    };

    VideoExporter (MidiStackLensProcessor&, Options);
    ~VideoExporter() override;

    // 失敗したら false と理由
    bool start (juce::String& error);
    void cancel();

    std::function<void (bool ok, const juce::String& message)> onFinished;

    double progress = 0.0;  // 0..1(プログレスバーに結びつける)

    static juce::File findFfmpeg (const juce::String& savedPath);
    static void logicalSizeFor (int width, int height, int& logicalW, int& logicalH);

private:
    class EncodeThread;

    void timerCallback() override;
    void renderFrame (int index);
    void finish (bool ok, const juce::String& message);

    MidiStackLensProcessor& proc;
    Options opt;
    std::unique_ptr<MidiStackView> view;
    juce::File frameDir;
    juce::ThreadPool pool { 2 };
    std::unique_ptr<EncodeThread> encoder;

    int frames = 0;
    int nextFrame = 0;
    int logicalW = 0, logicalH = 0;
    double duration = 0.0;
    std::atomic<int> writeFailures { 0 };
    bool done = false;
    bool encoding = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VideoExporter)
};
