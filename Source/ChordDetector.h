#pragma once
// JUCE に依存しない、コード判定だけのヘッダ。tests/ から単体で試せます。
#include <array>
#include <string>
#include <vector>

namespace msl
{
struct ChordResult
{
    bool named = false;       // コード名を付けられた
    bool hasDegrees = false;  // 度数ラベルを出せる(2音以上なら true)
    int rootPc = 0;           // ルートの音名(0=C ... 11=B、コンサートピッチ)
    int bassPc = 0;           // 最低音の音名
    std::string suffix;       // "", "m7", "M9" など(UTF-8)
    std::array<std::string, 12> degree;  // 音名(0〜11)ごとの度数ラベル。無い音は空

    bool isSlash() const { return named && rootPc != bassPc; }
};

// midiNotes は今押さえている MIDI ノート番号(順不同、重複可)
ChordResult detectChord (const std::vector<int>& midiNotes);
}  // namespace msl
