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

時計はフィッシャー300秒+3秒。思考予算は残り時間とincrementから毎手計算する:
`budget = inc×0.8 + 残り時間/tmhorizon`(上限 `tmmax`、残り時間の1割まで、かつ `残り時間 − tmreserve` まで)。
序盤は12秒、以降は銀行の残額に比例して減る(docs/strengthening.md 10章)。
以前は3秒で頭打ちだったため持ち時間の300秒がほぼ使われずに終わっていた。

`TSUITATE_ENGINE_OPTS`(とエンジンの `set <key> <val>`)で変えられる主な設定:

| キー | 既定 | 説明 |
| --- | --- | --- |
| `particles` | 256 | 粒子数の目標 |
| `depth` | 8 | stage2の探索深さ上限。実対局の予算(3秒以上)では深さ6のパスが1秒前後で終わって残りを捨てていたので8に(docs/strengthening.md 10章)。200msでは深さ6にも届かないので低予算の挙動は変わらない |
| `threads` | 1 | 思考のワーカースレッド数(粒子並列)。0=auto(使えるCPU数=ハードウェア並列度とcgroupクォータの小さいほう、上限16)。実効値は常に同じ基準でクランプされ、対局開始時のinfo行で報告される。ブリッジは未指定なら `threads 0` を送る |
| `tt` | auto | 確定化探索の置換表(+killer/historyオーダリング)。auto(-1)=1手の予算が `ttautoms`(既定1500ms)以上のときだけ有効。固定深さのベンチで深さ6のノード数を69%減らす(値は同一)が、200ms予算(深さ2が主)では定数コストが勝って中立(docs/strengthening.md 3.4章・10章)。メモリはスレッドごとに16MB、全スレッド合計128MBを上限に縮める(16スレッドなら8MBずつ)。一度確保した表は予算が下がっても対局中は保持する。**`tt 0` は表なしであってオーダリングは `hist` に従う**。§10以前の素の探索に戻すには `hist 0` も要る |
| `hist` | 1 | killer/historyオーダリングだけ(置換表なし、スレッドごとに256KB)。固定深さのベンチで深さ4のノード数 −17%、深さ6 −49%(値は同一)。0で従来(MVV-LVA+成りのみ)に戻る(docs/strengthening.md 10章)。注意: history はワーカーごとで粒子の割当が動的なので、`threads` が2以上だと同一seedでも実行ごとに手が変わりうる。seedからの再現(9.5章のようなクラッシュハント)には `hist 0` を付けること |
| `pvs` | 1 | 主変化探索(null窓)。最初の手だけ全窓、以降は [α, α+1] の null 窓で検査して超えたときだけ読み直す。**値は同一**で、固定深さのベンチで深さ4のノード数 −61〜68%、深さ6 −37〜46%(docs/strengthening.md 11章)。0 で従来の全窓αβ。以下の3つは pvs 0 では働かない(全ノードが PV になる) |
| `nmp` / `nmpr` | 1 / 2 | null move pruning(非PVノード・王手されていないとき・静的評価 ≥ β)。短縮量 R = `nmpr` + depth/4。値は変わる(深さ6の192局面で value_sum −0.5%) |
| `lmr` / `lmrstart` / `lmrdepth` | 1 / 3 / 3 | late move reductions。深さ `lmrdepth` 以上の非PVノードで `lmrstart` 手目以降の quiet 手(TT手・捕獲・成り・killer・王手を除く)を1〜2浅く読み、α を超えたときだけ読み直す |
| `futility` | 150 | フロンティア(depth 1)の futility 枝刈りの余白(cp)。静的評価 + 余白 ≤ α なら quiet 手を読まない。0 で無効 |
| `nlpct` | 2.0 | stage2 の1ジョブが使ってよい予算の割合(%)。実効のノード上限は max(`nodeslimit2`, 予算ms × これ/100 × 5000)。200msでは従来どおり60000、3秒では30万。0で固定上限のまま |
| `tmhorizon` / `tmmax` / `tmreserve` | 30 / 12000 / 500 | 実対局の時間管理(上記)。持ち時間の銀行を何手で配るか、1手の予算の上限(ms、下限300)、残り時間から必ず残す余白(ms)。予算は常に `残り時間 − tmreserve` を超えない |
| `passgate` | 0 | §9 探索予算のスケジューリング。1で stage2 の深いパスの開始を固定の「予算の60%」ゲートではなく、深さ別の1ジョブあたり実測時間(対局内で学習)× 実ジョブ数の予測で決める。最初のパスが収まらないときは粒子数を減らして読む(docs/strengthening.md 9章) |
| `halving` | 0 | 1で stage2 のパスごとに候補を直前の序列で半分に絞る(12→6→3、下限2)。深いパスが安くなるぶん同じ予算で1段深く読める。絞られた候補は最終選択からも外れる |
| `stage1pct` | 0 | stage1(全候補の粗い序列化)に使う予算の上限(%)。0=従来(固定24粒子)。指定すると1ジョブあたりの実測時間から収まる粒子数を選ぶ(下限2)。候補が100手を超える中盤で stage1 が予算の半分を食っていたのを抑える |
| `passgrowth` | 3.0 | `passgate` の未観測の深さの外挿係数(直前のパスの単価 × これ) |
| `s2margin` | 50 | stage2 の締め切り余白(ms)。`passgate` とは独立。200ms のアリーナ計測では 20 を併用(docs/strengthening.md 9.2章) |
| `depthstep` | 2 | stage2 の反復深化の刻み(1 or 2) |
| `nodeslimit2` | 60000 | stage2 の1ジョブのノード上限の下限値(超えると静的評価で打ち切り、値が汚れる。診断 `trunc2`)。予算に応じた引き上げは `nlpct` |
| `sir` | 0 | 重み付き粒子フィルタ(SIR)。相手の反則を粒子の尤度(1−p_ok)として使い、相手の着手も整合手の方策質量で重み付ける。ESSが粒子数の半分を切ったら系統的リサンプリング(docs/strengthening.md 2章) |
| `foulbase` / `foulstep` | 350 / 60 | 反則の期待コストと、反則累計1回ごとの追加ぶん |
| `opppolicy` | 1 | 相手手の方策。1=非千里眼prior / 0=旧(着手後評価のsoftmax) |
| `deduce` | 1 | 演繹層(確実な空きマス・反則からの割り出し)の有効化 |
| `synthprior` | 1 | 合成粒子の配置に駒種ごとの事前分布を使う(0で一様配置) |
| `syncpct` | auto | 1手の思考予算のうち信念の同期・再生成に回す割合(%)。既定はauto=実効スレッド数が2以上なら55、それ以外は40(探索だけ速くすると反則経済が崩れるため。docs/strengthening.md 3.4章)。0〜100の明示指定が優先 |
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
| `priorfit` | 0 | 相手priorの重み表。0=手書き / 1=アリーナの完全情報に適合した重み(王手の反映込み。docs/strengthening.md 4章)。適合は `arena ... dump <path>` で教師データを取り `tools/fit_policy.py` で行う |

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

### 探索のベンチ(木の同一性とnps)

```sh
printf "bench 12 120 4 0 0\nquit\n" | ./YaneuraOu-by-gcc   # [games] [maxplies] [depth] [oppmodel] [mode: 0=素/1=表+hist/2=histのみ]
# → info string bench positions=192 depth=4 ... nodes=... value_sum=... hash=... knps=...
```

ランダム局面で固定深さの確定化探索を回し、総ノード数・値の和・ハッシュ・npsを出す。
**木を変えないはずの変更**(割り当ての削減・オーダリングの実装差し替え)は `hash` が
一致することで検証し、**オーダリングの改善**は `value_sum` が同一のまま `nodes` が
減ることで測る(全窓のαβは並び順によらず同じ値を返す)。アリーナの `knps` は
時間門と粒子数に依存するので、npsの退行監視にはこちらを使う。

### ブリッジ込みのE2Eテスト

```sh
cd bridge
npx tsx test/mock-server.ts &        # サイトと同じ裁定のモックサーバー(:5199)
TSUITATE_URL=http://localhost:5199 TSUITATE_BOT_TOKEN=tsb_a npm start &
TSUITATE_URL=http://localhost:5199 TSUITATE_BOT_TOKEN=tsb_b npm start &
```

※ 1台で2ボットを走らせると、既定ではそれぞれがCPU数ぶんのスレッドを立てて
合計2倍のオーバーサブスクリプションになる。挙動確認には十分だが、
思考時間を測る用途では `TSUITATE_ENGINE_OPTS="threads <コア数/2>"` を
両方に指定して割ること。

## 設計の要点

詳細は [docs/design.md](docs/design.md)。次版に向けた強化設計と実測は
[docs/strengthening.md](docs/strengthening.md)。

最新の変更(docs/strengthening.md 11章): 確定化探索に PVS(null窓)・null move・
LMR・futility の枝刈りを入れた(既定オン、`pvs 0` で従来の探索に戻る)。
固定深さのベンチで PVS は値を変えずに深さ4のノード数 −61〜68%、深さ6 −37〜46%、
4つ合わせて深さ6の木が **1/5〜1/7**。200ms のアリーナ(60局)は中立だが、
1000ms(30局)では 58.3%・反則率と brier が3seedとも改善・avg_depth +0.6 で、
実対局の予算帯(深さ6〜8)ほど効く。

その前の変更(docs/strengthening.md 10章): 実対局では予算式が3秒で頭打ちで、
探索も深さ上限6に1.3秒で当たって止まり、持ち時間300秒がほぼ使われていなかった。
時間管理を銀行の幾何配分(序盤12秒)に変え、深さ上限8とノード上限の予算連動で
使い切るようにした。探索自体もノードごとの割り当て除去で knps +16〜26%、
killer/history オーダリング(`hist`、既定オン)で同じ深さのノード数を
17〜49% 減らし、置換表(`tt`)は予算1.5秒以上で自動的に有効になる。

それ以前の実測(docs/strengthening.md 3.4章): **粒子並列(`threads`)+思考予算の
再配分(`syncpct 55`)が初めて採用ゲートを通過した** —— 対旧既定 120局で
**勝率61.7%(z=+2.74、有意)**、盤上決着も22勝9敗、1局あたりの反則も有意に減少
(対応のあるt=−2.63)。鍵は「増えた計算をどこに使うか」で、探索だけを4倍にすると
攻撃性と反則コストの均衡が壊れて43.3%に沈む(1.6章の「反則の値付けは探索の
攻撃性に依存する」の再現)。増えた計算の一部を信念(粒子フィルタ)に戻すと
探索の深さを保ったまま反則率が3割下がる。

その後の実測(9章): 思考の予算内訳を測ると、信念の同期は平均3msしか使っておらず、
予算は stage1(全候補の粗い序列化)と stage2 が使い切っていた。深いパスの開始を
予測ゲートに変え、候補を半減し、stage1 に予算上限を課すスケジューラ
(`passgate` / `halving` / `stage1pct`)は深さ0の決定を33%→9%に減らし
avg_depth を 1.6→2.3 にしたが、60局で勝率はちょうど50%。**探索を深くしても
勝敗が動かない**ことが分かり、既定はオフのまま。

その前段の実測(1.11章): 探索の相手ノードから千里眼バイアスを取り除く施策
(`oppmodel`)は、打ち方を大きく変えた(盤上決着 18%→28%、反則減)が
勝率55.2%(有意でない)でゲート不達 —— 「次の律速は素の棋力(計算資源)」
という読みに改訂し、それが3.4章で実証された。

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
