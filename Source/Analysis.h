#pragma once
// JUCE に依存しない、ボイシング分析・キー推定・ディグリー表記。tests/ から単体で試せます。
#include <array>
#include <string>
#include <vector>

#include "ChordDetector.h"

namespace msl
{
// 半音数 → 音程名("m3", "P5", "M9" …)。2オクターブを超えたら "+25" のように半音数で返す
std::string intervalName (int semitones);

struct VoicingInfo
{
    enum class Type { none, single, interval, close, open, drop2, drop3, spread };

    int count = 0;              // 押さえている音の数
    int distinct = 0;           // 音名の種類
    int span = 0;               // 最低音から最高音までの半音数
    std::vector<int> gaps;      // 隣り合う音の間隔(半音)。低い方から
    Type type = Type::none;
    int inversion = -1;         // 0 = 基本形, 1〜 = 第n転回, -1 = 判定なし(コード名なし・オンコード)
    bool onChord = false;       // 最低音がコードの構成音ではない、またはテンションが最低音
    bool lowIntervalWarning = false;  // ローインターバルリミットより低い所で音が近い
    int warnLow = -1;           // 濁りそうな2音のうち低い方のノート番号
};

// notes は低い順に並んだ MIDI ノート番号
VoicingInfo analyzeVoicing (const std::vector<int>& sortedNotes, const ChordResult&);

// VoicingInfo::Type の表示名(UTF-8)
const char* voicingTypeName (VoicingInfo::Type);

struct KeyEstimate
{
    bool valid = false;
    int tonicPc = 0;       // 0 = C … 11 = B(コンサートピッチ)
    bool minor = false;
    double confidence = 0.0;  // 相関係数(0〜1)
};

// 音名ごとの重み(弾いた回数や長さ)からキーを推定する(キープロファイルとの相関)
KeyEstimate estimateKey (const std::array<double, 12>& pcWeights);

// キーの中でのディグリー表記("V7", "ii7", "♭VII" …)。コード名が無いときは空
std::string romanNumeral (const ChordResult&, int tonicPc);

// 機能("T" / "SD" / "D")。ダイアトニックでなければ空
std::string harmonicFunction (const ChordResult&, int tonicPc, bool minorKey);

// そのキーのダイアトニックスケールに含まれる音名か
bool inScale (int pc, int tonicPc, bool minorKey);
}  // namespace msl
