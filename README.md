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

`TSUITATE_ENGINE_OPTS`(とエンジンの `set <key> <val>`)で変えられる主な設定:

| キー | 既定 | 説明 |
| --- | --- | --- |
| `particles` | 256 | 粒子数の目標 |
| `depth` | 6 | stage2の探索深さ上限 |
| `threads` | 1 | 思考のワーカースレッド数(粒子並列)。ブリッジは未指定ならCPUコア数を自動設定し、あわせて `syncpct 55` も設定する(下記) |
| `tt` | 0 | 確定化探索の置換表+killer/historyオーダリング。スレッドごとに16MB使う。200ms予算では中立だったため既定オフ(docs/strengthening.md 3.4章) |
| `foulbase` / `foulstep` | 350 / 60 | 反則の期待コストと、反則累計1回ごとの追加ぶん |
| `opppolicy` | 1 | 相手手の方策。1=非千里眼prior / 0=旧(着手後評価のsoftmax) |
| `deduce` | 1 | 演繹層(確実な空きマス・反則からの割り出し)の有効化 |
| `synthprior` | 1 | 合成粒子の配置に駒種ごとの事前分布を使う(0で一様配置) |
| `syncpct` | 40 | 1手の思考予算のうち信念の同期・再生成に回す割合(%)。`threads` 有効時はブリッジが既定55を設定(探索だけ速くすると反則経済が崩れるため。docs/strengthening.md 3.4章) |
| `blockcp` | 0 | 相手の反則を誘う配置への加点(0で無効。未較正) |
| `oppmodel` | 0 | 探索の相手ノードを非千里眼モデルにする(1=直後の応手だけ / 2=全相手ノード)。**既定オフ**: 210局で勝率55.2%(有意でない)、採用ゲート未達(下記) |
| `opplambda` | 0 | 相手ノードに千里眼の最善応手を混ぜる割合。実測では0(完全に盲目)が最良 |
| `foulgain` | 0 | 相手の期待反則をこちらの利得として数える倍率。**有害と実測されたので既定0** |
| `oppreplyk` | 6 | 期待値化する相手応手の数(prior上位k手)。`oppmodel` 有効時のみ |
| `oppreplykdeep` | 3 | 同上、2手目以降の相手ノード(`oppmodel 2` のときだけ使う) |
| `oppreplykstage1` | 2 | stage1(全候補の粗い序列化)で展開する相手応手の数。0でstage1は従来の千里眼qsearchのまま |
| `oppfoulcap` | 2.0 | 1手番あたりの相手の期待反則回数の上限 |
| `oppfoulmax` | 800 | 相手の反則項が1つの相手ノードで動かせる評価の上限(cp)。局面評価の飽和(±2500)を超えさせない |
| `oppcheckprior` | 0 | 王手されている相手が「玉を逃がしたがる」ことをpriorに反映する。信念側の方策とも共有しているので、`oppmodel` とは独立にA/Bすること |

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

`p1cfg <key> <val>` / `p2cfg <key> <val>` で**片側だけ**設定を変えられるので、
同一バイナリ内でA/B対戦ができる:

```sh
printf "arena games 12 budget 200 particles 128 p2 belief p1cfg foulbase 350\nquit\n" | ./YaneuraOu-by-gcc
```

診断行には、審判の完全情報と信念を突き合わせた指標が出る
(`king_acc`=相手玉のマスを当てている粒子の割合、`occ_rec`=相手駒の再現率、
`brier`=p_legalの較正誤差)。勝敗は反則負けの分散が大きいので、
**変更はこの数字とセットで評価すること**。

現状の実測: 前進ヒューリスティック(サイト内蔵bot相当)に **20戦全勝**(反則0.20回/局。初版は2.2回/局)。

初版と同じ設定(`opppolicy 0 deduce 0 synthprior 0 foulbase 100`)との直接対決は
60局で **38勝22敗**。1手あたりの反則は **8.0% → 6.4%** と明確に減っている(z=3.2)。
ただし勝ち星38のうち32は反則の消耗戦由来で、盤上で決着する局は60局中12局・6勝6敗の互角。
信念を良くしても「勝ちにいく」力は動いていないので、次の律速は探索側の千里眼バイアス。
詳細は docs/design.md の 4.5 / 5 章。

### ブリッジ込みのE2Eテスト

```sh
cd bridge
npx tsx test/mock-server.ts &        # サイトと同じ裁定のモックサーバー(:5199)
TSUITATE_URL=http://localhost:5199 TSUITATE_BOT_TOKEN=tsb_a npm start &
TSUITATE_URL=http://localhost:5199 TSUITATE_BOT_TOKEN=tsb_b npm start &
```

## 設計の要点

詳細は [docs/design.md](docs/design.md)。次版に向けた強化設計と実測は
[docs/strengthening.md](docs/strengthening.md)。

直近の実測でわかったこと(現行バイナリ210局、docs/strengthening.md 1.11章):
**探索の相手ノードから千里眼バイアスを取り除くと打ち方は大きく変わる** ——
盤上で決着する局が 18% → 28% に増え、1局あたりの反則も減る
(局ごとの対応のあるt検定で t=−2.78)。**しかし勝率は55.2%(有意でない)、
盤上の勝敗は31勝28敗で互角**で、採用ゲートの60%には遠い。
設計時に「次の律速」と見込んだ場所は、方向は正しかったが効果量が足りなかった。
残る候補は素の棋力(計算資源・評価関数)のほう。

- **観測**: 自分の反則(理由なし)・取った/取られた駒・王手宣言だけが入力。
  相手の指し手は一切見えない(サーバー側で構造的に保証)
- **信念状態**: 粒子=観測と矛盾しない完全局面。観測でフィルタし、
  枯渇したら死んだ粒子の相手手列の直近だけ再サンプリングする「部分若返り」で再生成、
  それも尽きたら駒勘定と王手状態だけ合う配置を直接作る「合成粒子」に落とす
- **相手モデル**: 相手は**こちらの駒が見えない**ので、相手の手は
  「自分の駒だけで決まる素朴なprior × 合法性フィルタ」で生成する。
  評価関数を呼ばないので、粒子の再生成リプレイでも同じ方策が使える
- **演繹**: 自分の手が通った経路は空きだった / 相手が最後に取ったマスには相手駒がいる /
  反則になった打ちの着地マスは埋まっている、といった論理的に確定する情報を
  合成粒子の生成と棄却に使う
- **手の評価**: `combined = p_legal × E[確定化探索値] + (1−p_legal) × 反則コスト`。
  反則コストは累計と信念品質に応じて急騰し、反則負けのスパイラルを防ぐ。
  この競技は実質「反則予算10回の消耗戦」なので、この値付けが勝敗を大きく動かす
- **ライセンス**: やねうら王がGPLv3のため、本リポジトリ全体もGPLv3

## 由来

- やねうら王: https://github.com/yaneurao/YaneuraOu (fork元: commit 33ccf1f, 2026-08-05)
- ついたて将棋サイト: https://beta.tsuitate.info (bot API仕様は tsuitate リポジトリ docs/bot-api.md)
