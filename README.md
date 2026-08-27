# tsuitate-bot2 — やねうら王フォークのついたて将棋エンジン

[やねうら王 (YaneuraOu)](https://github.com/yaneurao/YaneuraOu) をフォークして作った
ついたて将棋(王様のかくれんぼ)用の思考エンジンと、
[beta.tsuitate.info](https://beta.tsuitate.info) のbot APIに接続するブリッジ。

前作 [tsuitate-bot](https://github.com/tempakyousuke/tsuitate-bot) (Rust) は
探索木を張れず2手読みが上限だった。本作はやねうら王の局面表現・合法手生成・
評価関数(MaterialLv9)・詰み判定を土台に、**確定化サンプリング**
(粒子=相手配置の仮説ごとに完全情報のαβ探索を回して集計する)で深い読みを実現する。

## 構成

```
source/                     やねうら王のフォーク(GPLv3)
  engine/tsuitate-engine/   ついたて将棋エンジン(このリポジトリの本体)
    tsuitate_common.*       観測イベント・自分視点の盤面・候補手生成
    belief.*                信念状態 = 粒子フィルタ(整合フィルタ/部分若返り/合成粒子)
    dsearch.*               確定化局面のαβ+静止探索(+1手詰め)
    think.*                 候補手の期待値評価(p_legal×探索値+反則コスト)と時間管理
    arena.*                 完全情報の審判つきローカル自己対戦
    tsuitate-search.cpp     行プロトコルとエントリポイント
bridge/                     サイト⇔エンジンのSocket.IOブリッジ(TypeScript)
  src/bridge.ts             本体
  test/mock-server.ts       サイトと同じ裁定のモックサーバー(E2Eテスト用)
docs/design.md              設計ノート
```

## ビルド

```sh
cd source
make -j normal YANEURAOU_EDITION=TSUITATE_ENGINE TARGET_CPU=AVX2 COMPILER=clang++
# → source/YaneuraOu-by-gcc
```

評価関数はMaterialLv9(利き・紐・玉周辺の穴まで入った手作り評価)で、
**外部の評価ファイルは不要**。AVX2のないCPUでは `TARGET_CPU=SSE42` などを指定する。

## サイトへの接続

1. サイトにログインし、マイページの「bot管理」でbotを作成してAPIトークン(`tsb_...`)を取得
2. ブリッジを起動:

```sh
cd bridge
npm install
TSUITATE_URL=https://beta.tsuitate.info \
TSUITATE_BOT_TOKEN=tsb_... \
npm start
```

キューに自動で並び、マッチしたら対局し、終局したらまた並ぶ。Ctrl-Cで終了。

| 環境変数 | 既定値 | 説明 |
| --- | --- | --- |
| `TSUITATE_URL` | `https://beta.tsuitate.info` | 接続先 |
| `TSUITATE_BOT_TOKEN` | (必須) | APIトークン |
| `TSUITATE_ENGINE_PATH` | `../source/YaneuraOu-by-gcc` | エンジンのパス |
| `TSUITATE_ENGINE_OPTS` | (空) | エンジン設定。例 `particles 256,depth 4` |
| `TSUITATE_QUEUE_RETRY_MS` | `60000` | キュー参加拒否後の再試行間隔 |

時計はフィッシャー300秒+3秒。思考予算は残り時間とincrementから毎手計算する。

## ローカルでの検証

### 審判つき自己対戦(アリーナ)

エンジン単体にサイトと同じ裁定(反則=通常将棋ルールで不正、累計10回で反則負け)の
審判が内蔵されている:

```sh
cd source
printf "arena games 20 budget 300 particles 128\nquit\n" | ./YaneuraOu-by-gcc
# belief(本体) vs heuristic(前進ヒューリスティック=サイト内蔵bot相当)
# p2 belief を付けると本体同士の対戦
```

現状の実測: 前進ヒューリスティックに **20戦全勝**(反則2.2回/局)。
前作 tsuitate-bot (Rust) との直接対決はまだ 0-6(主因は信念の質=反則経済。
分析と次の一手は docs/design.md の 4.5 / 5 章)。

### ブリッジ込みのE2Eテスト

```sh
cd bridge
npx tsx test/mock-server.ts &        # サイトと同じ裁定のモックサーバー(:5199)
TSUITATE_URL=http://localhost:5199 TSUITATE_BOT_TOKEN=tsb_a npm start &
TSUITATE_URL=http://localhost:5199 TSUITATE_BOT_TOKEN=tsb_b npm start &
```

## 設計の要点

詳細は [docs/design.md](docs/design.md)。

- **観測**: 自分の反則(理由なし)・取った/取られた駒・王手宣言だけが入力。
  相手の指し手は一切見えない(サーバー側で構造的に保証)
- **信念状態**: 粒子=観測と矛盾しない完全局面。観測でフィルタし、
  枯渇したら死んだ粒子の相手手列の直近だけ再サンプリングする「部分若返り」で再生成、
  それも尽きたら駒勘定と王手状態だけ合う配置を直接作る「合成粒子」に落とす
- **手の評価**: `combined = p_legal × E[確定化探索値] + (1−p_legal) × 反則コスト`。
  反則コストは累計と信念品質に応じて急騰し、反則負けのスパイラルを防ぐ
- **ライセンス**: やねうら王がGPLv3のため、本リポジトリ全体もGPLv3

## 由来

- やねうら王: https://github.com/yaneurao/YaneuraOu (fork元: commit 33ccf1f, 2026-08-05)
- ついたて将棋サイト: https://beta.tsuitate.info (bot API仕様は tsuitate リポジトリ docs/bot-api.md)
