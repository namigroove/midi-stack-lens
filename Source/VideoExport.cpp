#include "VideoExport.h"

#include <cmath>

// ffmpeg を別スレッドで実行して待つ
class VideoExporter::EncodeThread : public juce::Thread
{
public:
    EncodeThread (juce::StringArray a) : juce::Thread ("MSL ffmpeg"), args (std::move (a)) {}

    ~EncodeThread() override { stopThread (4000); }

    void run() override
    {
        juce::ChildProcess proc;
        if (! proc.start (args, juce::ChildProcess::wantStdErr))
        {
            output = "ffmpeg を起動できませんでした";
            ok = false;
            finished = true;
            return;
        }

        juce::MemoryOutputStream collected;
        char buf[4096];
        while (proc.isRunning())
        {
            if (threadShouldExit())
            {
                proc.kill();
                output = "キャンセルしました";
                ok = false;
                finished = true;
                return;
            }
            const int n = proc.readProcessOutput (buf, (int) sizeof buf);
            if (n > 0)
                collected.write (buf, (size_t) n);
            else
                wait (20);
        }
        for (int n; (n = proc.readProcessOutput (buf, (int) sizeof buf)) > 0;)
            collected.write (buf, (size_t) n);

        output = collected.toString().trim();
        ok = proc.getExitCode() == 0;
        finished = true;
    }

    juce::StringArray args;
    juce::String output;
    std::atomic<bool> finished { false };
    bool ok = false;
};

VideoExporter::VideoExporter (MidiStackLensProcessor& p, Options o) : proc (p), opt (std::move (o)) {}

VideoExporter::~VideoExporter()
{
    stopTimer();
    pool.removeAllJobs (true, 3000);
    encoder.reset();
    view.reset();
    if (frameDir != juce::File() && frameDir.isDirectory())
        frameDir.deleteRecursively();
}

juce::File VideoExporter::findFfmpeg (const juce::String& savedPath)
{
#if JUCE_WINDOWS
    const juce::String exe = "ffmpeg.exe";
#else
    const juce::String exe = "ffmpeg";
#endif

    if (savedPath.isNotEmpty())
    {
        juce::File f (savedPath);
        if (f.existsAsFile())
            return f;
    }

    // プラグインやDAWの隣に置いてあるとき
    for (auto loc : { juce::File::currentApplicationFile, juce::File::currentExecutableFile })
    {
        const auto dir = juce::File::getSpecialLocation (loc).getParentDirectory();
        if (dir.getChildFile (exe).existsAsFile())
            return dir.getChildFile (exe);
    }

    // PATH
    const auto path = juce::SystemStats::getEnvironmentVariable ("PATH", {});
#if JUCE_WINDOWS
    const juce::String sep = ";";
#else
    const juce::String sep = ":";
#endif
    for (auto dir : juce::StringArray::fromTokens (path, sep, "\""))
    {
        if (dir.isEmpty())
            continue;
        const auto f = juce::File (dir).getChildFile (exe);
        if (f.existsAsFile())
            return f;
    }

    // DAW から起動すると PATH が短いことがあるので、よくある場所も見る
    juce::StringArray common;
#if JUCE_MAC
    common = { "/opt/homebrew/bin/ffmpeg", "/usr/local/bin/ffmpeg", "/opt/local/bin/ffmpeg", "/usr/bin/ffmpeg" };
#elif JUCE_WINDOWS
    common = { "C:\\ffmpeg\\bin\\ffmpeg.exe", "C:\\Program Files\\ffmpeg\\bin\\ffmpeg.exe",
               juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile ("ffmpeg\\bin\\ffmpeg.exe").getFullPathName(),
               juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getParentDirectory().getChildFile ("Local\\Microsoft\\WinGet\\Links\\ffmpeg.exe").getFullPathName(),
               "C:\\ProgramData\\chocolatey\\bin\\ffmpeg.exe" };
#else
    common = { "/usr/bin/ffmpeg", "/usr/local/bin/ffmpeg", "/snap/bin/ffmpeg" };
#endif
    for (auto& c : common)
        if (juce::File (c).existsAsFile())
            return juce::File (c);

    return {};
}

void VideoExporter::logicalSizeFor (int width, int height, int& lw, int& lh)
{
    // 画面は文字の大きさが固定なので、小さめの論理サイズで配置して拡大して描く
    const double scale = width >= height ? (double) width / 1280.0 : (double) width / 800.0;
    lw = (int) std::lround ((double) width / scale);
    lh = (int) std::lround ((double) height / scale);
}

bool VideoExporter::start (juce::String& error)
{
    if (opt.rec.empty())
    {
        error = juce::String::fromUTF8 ("録音がありません");
        return false;
    }
    if (! opt.ffmpeg.existsAsFile())
    {
        error = juce::String::fromUTF8 ("ffmpeg が見つかりません");
        return false;
    }
    if (opt.out.getParentDirectory().exists() == false)
    {
        error = juce::String::fromUTF8 ("保存先のフォルダがありません");
        return false;
    }

    double wavSeconds = 0.0;
    if (opt.wav != juce::File())
    {
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (opt.wav));
        if (reader == nullptr || reader->sampleRate <= 0.0)
        {
            error = juce::String::fromUTF8 ("WAV を読めませんでした");
            return false;
        }
        wavSeconds = (double) reader->lengthInSamples / reader->sampleRate;
    }

    duration = msl::exportDuration (opt.rec, wavSeconds, opt.wavOffset);
    frames = msl::frameCountFor (duration, opt.fps);

    frameDir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                   .getChildFile ("msl_frames_" + juce::String::toHexString (juce::Random::getSystemRandom().nextInt64()));
    if (! frameDir.createDirectory())
    {
        error = juce::String::fromUTF8 ("作業用のフォルダを作れませんでした");
        return false;
    }

    logicalSizeFor (opt.width, opt.height, logicalW, logicalH);
    view = std::make_unique<MidiStackView> (proc, true);
    view->setSize (logicalW, logicalH);
    view->offlineBegin (opt.rec);

    nextFrame = 0;
    progress = 0.0;
    startTimer (1);
    return true;
}

void VideoExporter::renderFrame (int index)
{
    const double t = (double) index / (double) opt.fps;
    view->offlineFrame (t, 1.0 / (double) opt.fps);

    juce::Image img (juce::Image::RGB, opt.width, opt.height, false);
    {
        juce::Graphics g (img);
        g.addTransform (juce::AffineTransform::scale ((float) opt.width / (float) logicalW,
                                                      (float) opt.height / (float) logicalH));
        view->paintEntireComponent (g, true);
    }

    const auto file = frameDir.getChildFile ("f_" + juce::String (index).paddedLeft ('0', 6) + ".png");
    pool.addJob ([img, file, this]
                 {
                     juce::FileOutputStream out (file);
                     juce::PNGImageFormat png;
                     if (! out.openedOk() || ! png.writeImageToStream (img, out))
                         ++writeFailures;
                 });
}

void VideoExporter::timerCallback()
{
    if (done)
        return;

    if (! encoding)
    {
        const auto start = juce::Time::getMillisecondCounterHiRes();
        while (nextFrame < frames && juce::Time::getMillisecondCounterHiRes() - start < 25.0)
        {
            if (pool.getNumJobs() > 10)  // 書き込みが追いつくまで待つ
                break;
            renderFrame (nextFrame++);
        }
        progress = 0.9 * (double) nextFrame / (double) frames;

        if (nextFrame >= frames && pool.getNumJobs() == 0)
        {
            if (writeFailures.load() > 0)
            {
                finish (false, juce::String::fromUTF8 ("コマ画像を保存できませんでした(ディスクの空きを確認してください)"));
                return;
            }

            juce::StringArray args;
            args.add (opt.ffmpeg.getFullPathName());
            for (auto& a : msl::ffmpegArgs (frameDir.getFullPathName().replaceCharacter ('\\', '/').toStdString(), opt.fps,
                                            opt.width, opt.height,
                                            opt.wav == juce::File() ? std::string() : opt.wav.getFullPathName().toStdString(),
                                            opt.wavOffset, duration, opt.out.getFullPathName().toStdString()))
                args.add (juce::String::fromUTF8 (a.c_str()));

            encoder = std::make_unique<EncodeThread> (args);
            encoder->startThread();
            encoding = true;
            progress = 0.92;
        }
    }
    else if (encoder->finished.load())
    {
        encoder->stopThread (2000);
        if (encoder->ok && opt.out.existsAsFile() && opt.out.getSize() > 0)
            finish (true, opt.out.getFullPathName());
        else
            finish (false, juce::String::fromUTF8 ("ffmpeg でエラーになりました\n") + encoder->output.substring (0, 600));
    }
}

void VideoExporter::cancel()
{
    if (done)
        return;
    if (encoder != nullptr)
        encoder->signalThreadShouldExit();
    pool.removeAllJobs (true, 3000);
    finish (false, juce::String::fromUTF8 ("キャンセルしました"));
}

void VideoExporter::finish (bool ok, const juce::String& message)
{
    if (done)
        return;
    done = true;
    stopTimer();
    progress = ok ? 1.0 : progress;

    if (encoder != nullptr)
        encoder->stopThread (3000);
    pool.removeAllJobs (true, 3000);
    if (frameDir.isDirectory())
        frameDir.deleteRecursively();
    if (! ok && opt.out.existsAsFile() && opt.out.getSize() == 0)
        opt.out.deleteFile();

    if (onFinished)
    {
        auto cb = onFinished;
        cb (ok, message);
    }
}
