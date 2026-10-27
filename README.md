# MIDI Stack Lens

MIDIの積み方(ボイシング)を鍵盤で見るためのVST3。音は出さず、届いたMIDIをそのまま素通しします。

## できること

- 押さえている音を鍵盤で光らせる(色は「積み順 / 音名 / 強さ」から選べる)
- 鍵盤の上に履歴ロール。押した音が積み上がって、あとから和音の組み方を見返せる
- 押さえた鍵盤の上に度数(R, 3, 5, ♭7, 9 …)を小さく表示
- コード名を小さく表示(転回形は `G7/B` のように表示)。履歴ロールにもコード名が残る
- [namigroove.com/tools/piano](https://www.namigroove.com/tools/piano/) から引き継いだ設定
  - 表記 CDE / ドレミ、♭ / ♯
  - キー(アルトサックスは E♭、テナーは B♭ など。鍵盤の音名とコード名がその楽器の楽譜どおりになる)
  - 鍵盤の音名(Cだけ / 全部 / なし)
  - 配色は #396cdf のブルーと、押さえた鍵盤のオレンジ

履歴の長さは「拍」で決まります。DAW のテンポに合わせて流れる速さが変わります(テンポが取れないときは 120 BPM)。
オクターブ表記は C3 = MIDIノート60 です。

## ビルド

CMake と C++17 のコンパイラがあれば、JUCE は自動で取得されます。

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --target MidiStackLens_VST3
ctest --test-dir build -C Release     # コード判定の単体テスト
```

- Windows: `build/MidiStackLens_artefacts/Release/VST3/MIDI Stack Lens.vst3` を `C:\Program Files\Common Files\VST3\` へ
- macOS: 同じ場所にできる `.vst3` を `~/Library/Audio/Plug-Ins/VST3/` へ(AU も欲しいときは `-DMSL_BUILD_AU=ON`)
- GitHub に置けば `.github/workflows/build.yml` が Windows と Mac のVST3を自動でビルドします

## 使い方のメモ

- DAW ではインストゥルメントとして立ち上げて、MIDIトラックの出力先にするか、見たいトラックのMIDIを送り込みます。
  MIDIはそのまま出力に素通しされます。
- Standalone 版も一緒に作られるので、DAW なしでMIDIキーボードをつないで試せます。

## コード判定の仕組み

`Source/ChordDetector.cpp` に、コードの型の表(約40種)があります。構成音を12の音名に畳み込み、
全ての音名をルート候補として表と照合します。最低音がルートならコスト0、そうでなければ加点して、
一番コストの小さい名前を採用します(`A C E G` を最低音Cで押さえると `Am7/C` ではなく `C6`、最低音Aなら `Am7`)。
5度の省略は許し、ルートの省略(ルートレス)は判定しません。
名前が付かない押さえ方でも、最低音を基準に度数は出ます。
