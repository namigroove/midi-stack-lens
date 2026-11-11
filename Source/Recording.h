#pragma once

// 録音(MIDIの記録)と、そこから作る .mid / 動画のためのデータ。JUCE に依存しない(単体テストできる)。

#include <cstdint>
#include <string>
#include <vector>

namespace msl
{
struct RecEvent
{
    double sec = 0.0;  // 録音開始からの秒(サンプル数で数えるので、DAWのオフライン書き出し中でもずれない)
    double ppq = -1.0; // DAW上の位置(4分音符=1)。取れなければ -1
    uint8_t note = 0;
    uint8_t vel = 0;
    uint8_t kind = 0;  // 0 = ノートオフ, 1 = ノートオン, 2 = 全ノートオフ
};

// テンポ・拍子・再生位置の記録(画面の BAR / BEAT / BPM を動画に再現するため)
struct TransportKey
{
    double sec = 0.0;
    double ppq = -1.0;
    double barStartPpq = -1.0;
    double bpm = 120.0;
    int num = 4;
    int den = 4;
    bool playing = false;
};

struct Recording
{
    std::vector<RecEvent> events;
    std::vector<TransportKey> keys;
    double duration = 0.0;   // 録音の長さ(秒)
    double startPpq = -1.0;  // 録音を始めたときのDAW上の位置。取れなければ -1
    double bpm = 120.0;      // 録音開始時のテンポ
    int num = 4;
    int den = 4;

    bool empty() const { return events.empty(); }
};

// 標準MIDIファイル(SMF フォーマット0)のバイト列。tempoの変化も入れる。鳴りっぱなしの音は最後に止める。
std::vector<uint8_t> toMidiFile (const Recording&, int ticksPerQuarter = 480);

// 録音の t 秒時点のテンポ・位置(次のキーからの外挿つき)
TransportKey transportAt (const Recording&, double sec);

// 録音の先頭がDAWの何秒目にあたるか(DAWの書き出しが1小節目から始まるとき、WAVの頭出しに使う)。不明なら 0
double defaultWavOffset (const Recording&);

// 動画の長さ(秒)。WAV があれば WAV が終わるまで、なければ録音の終わり + 余韻
double exportDuration (const Recording&, double wavSeconds, double wavOffset, double tail = 1.0);

// 動画のフレーム数(fps 一定)
int frameCountFor (double durationSec, int fps);

// ffmpeg に渡す引数(コマ画像の連番 + 任意のWAV → mp4)。frameDir はコマ画像のフォルダ
std::vector<std::string> ffmpegArgs (const std::string& frameDir, int fps, int width, int height,
                                     const std::string& wavPath, double wavOffset, double durationSec,
                                     const std::string& outPath);
}  // namespace msl
