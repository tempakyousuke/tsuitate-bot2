// ついたて将棋エンジン: 確定化局面(粒子)に対する探索
//
// 粒子は完全情報の通常の将棋局面なので、普通のαβ探索がそのまま使える。
// 前作(Rust bot)は探索木を張れず2手読みが上限だったが、
// 確定化サンプリングでは粒子ごとに任意の深さまで読める。ここがフォークの主眼。
//
// v1は軽量な独自negamax(αβ+静止探索+1手詰め)。評価はMaterialLv9
// (利き・紐・玉周辺の穴込みの手作り評価。外部評価ファイル不要)。
#ifndef TSUITATE_DSEARCH_H_INCLUDED
#define TSUITATE_DSEARCH_H_INCLUDED

#include "tsuitate_common.h"

#if defined(TSUITATE_ENGINE)

#include <algorithm>

namespace YaneuraOu {
namespace Tsuitate {

// ---------------------------------------------------------------------------
// 探索コンテキスト(置換表 + history)。§3.2 探索の底上げ。
// ---------------------------------------------------------------------------
// DSearch は (候補手, 粒子) のジョブごとに使い捨てなので、ジョブをまたいで
// 生かしたい状態はここに置く。**ワーカースレッドごとに1つ**持つ(ロック不要)。
// 反復深化(stage2 の d=2,4,6…)は同じ部分木を深さを変えて読み直すので、
// 前のパスの結果が置換表に残っていると枝刈りとオーダリングが大きく効く。
//
// 値カットオフのゲートは**保存時の oppFouls と現在の oppFouls の一致**で行う。
// 当初は「同世代(=同じthink)のみ」で代理していたが、それは真の不変条件
// (foulGain = oppFouls の関数、が変わっていないこと)より遥かに強すぎる:
// 自分の反則のやり直し(同一局面・oppFouls不変 = TT再利用の理想形)でも
// 手番が変わるたびでも、まだ有効な値を全部捨てていた。oppFouls を
// エントリに保存して一致を要求すれば、必要十分のゲートになる
// (us と cfg は対局内で不変、コンテキストは対局開始で破棄、
//  相手ノードは TT を通らないので ply 依存の反則項は混入しない)。
// gen は置換の老化(同一thinkのエントリを深さ優先で守る)にだけ使う。
//
// 既知のトレードオフ(コンテキストあり = hist(既定オン)または tt、かつ threads > 1
// のとき): TT/history はワーカーローカルで、粒子グループ→ワーカーの割当は atomic
// カウンタの動的スケジュールなので、**ジョブの評価値自体が「どのワーカーが先に
// どのグループを取ったか」に依存する**。think() の固定順還元は加算順しか固定しない
// ため、同一 seed でも実行ごとに選ぶ手が変わりうる(seed からの再現デバッグが不能)。
// §10 で hist を既定にしたので、これは既定の挙動でもある(時間門があるので
// 完全な再現性はもともと無い)。再現性が要るなら `hist 0` にするか、
// 割当を静的(g % nw)にすること。
struct TTEntry {
	uint64_t key   = 0;  // pos.key()(0 = 空きスロット)
	int16_t  value = 0;  // value_to_tt 済み(詰みはply補正済み)
	uint16_t move16 = 0; // 最善手(Move::raw()。オーダリング用)
	int8_t   depth = -1;
	Bound    bound = BOUND_NONE;   // types.h の共通enum(独自enumで数値をずらさない)
	uint8_t  oppFouls = 0;         // 保存時の相手反則累計(値カットオフのゲート。0..10)
	uint8_t  gen   = 0;            // 置換の老化用(一巡しても値の正しさには関わらない)
};
static_assert(sizeof(TTEntry) == 16, "TTEntry should stay 16 bytes");

struct SearchContext {
	static constexpr size_t TT_BITS = 20;                  // 2^20 = 1M エントリ(16MB)
	static constexpr int    HIST_MAX = 16384;              // history の飽和値
	std::vector<TTEntry> tt;
	// false なら表を確保せず、killer/history のオーダリングだけを使う(Config::hist)
	bool useTT = true;
	// history[手番(0=BLACK)][Move::raw()]。quietの beta カットで depth^2 を加点
	std::vector<int16_t> hist;
	// killer もここに置く(ワーカーごと・think ごとにクリア)。DSearch のメンバに
	// すると (候補,粒子) ジョブごとの構築で毎回 ~2KB のゼロ初期化が走り、
	// killer を一度も読まない既定(tt 0)経路まで恒常コストを払うことになる。
	Move     killer[MAX_PLY][2];
	uint8_t  gen   = 0;
	// 現在の相手反則累計(think() 開始時に begin_think へ渡される)。
	// 値カットオフは tte->oppFouls == curOppFouls のエントリにだけ許す。
	uint8_t  curOppFouls = 0;
	// この think() でもう begin_think 済みかの判定(Thinker が通し番号を発行)。
	// コンテキストは1リージョン内では担当ワーカーだけが触り、リージョン間は
	// run_workers の join が順序づけるので、単純な比較で足りる。
	uint32_t stamp = 0;

	// think() ごと・ワーカーごとに1回。
	//   - 初回は割り当て(16MB)をここで行う: 呼び出しスレッドで全ワーカー分を
	//     まとめて確保すると、初手の予算内で workers×16MB のゼロ初期化と
	//     first-touch が直列に走ってしまう。ワーカー自身にやらせて分散する
	//   - 2回目以降は置換老化用の世代を進め、history は半減させる
	//     (減衰なしだと長い対局で飽和して序列の分解能が落ちる)
	// oppFouls: 現在の相手反則累計(値カットオフのゲートに使う)
	void begin_think(int oppFouls) {
		curOppFouls = uint8_t(std::clamp(oppFouls, 0, 255));
		if (hist.empty()) {
			hist.assign(2 * 65536, 0);
		} else {
			++gen;
			for (auto& h : hist)
				h = int16_t(h / 2);
		}
		// 表は「使うと決まった最初の think」で確保する。useTT は手番ごとに変わりうる
		// (tt=auto は予算で決まる)ので、初回に不要でも後で要ることがある
		if (useTT && tt.empty())
			tt.resize(size_t(1) << TT_BITS);
		for (int p = 0; p < MAX_PLY; ++p)
			killer[p][0] = killer[p][1] = Move::none();
	}
	TTEntry& slot(uint64_t key) { return tt[key & ((size_t(1) << TT_BITS) - 1)]; }
	int16_t& hist_of(Color side, uint16_t raw16) {
		return hist[(side == BLACK ? 0 : 65536) + raw16];
	}
};

struct DSearch {
	// nodesLimit: このノード数を超えたら打ち切って静的評価を返す(粒子1つ分の保険)
	uint64_t nodes      = 0;
	uint64_t nodesLimit = 200000;
	// ノード上限に当たって途中から静的評価を返した(値が汚れている)か。
	// 探索の打ち切り・TT書き込みの抑止・think() の trunc 診断が**同じ述語**を使う
	bool truncated() const { return nodes > nodesLimit; }

	// --- 相手モデル(非千里眼化。cfg=nullptr または cfg->oppModel=0 で従来動作) ---
	// cfg : 相手モデルの設定。nullptr なら素の千里眼αβ
	// us  : こちらの手番色。相手ノードの判定に使う。
	//       COLOR_NB のままなら相手モデルは働かない(取り違えの保険)
	// foulGain : 相手の反則1回のこちらから見た価値(cp、正の値)。
	//            think() が foul_value(oppFouls) × foulGainScale で与える
	// oppK1 : ply==1 の相手ノードで展開する応手数。0なら cfg->oppReplyK。
	//         stage1(粗い序列化)は cfg->oppReplyKStage1 を入れて安く回す
	const Config* cfg      = nullptr;
	Color         us       = COLOR_NB;
	double        foulGain = 0.0;
	int           oppK1    = 0;

	// --- §3.2 置換表 + オーダリング(hist / tt が有効なときだけ think() が設定する) ---
	// nullptr なら従来の探索(MVV-LVAのみ)と完全に同一の経路。
	// ctx->useTT が false のときは表を引かず、killer/history のオーダリングだけ使う。
	// killer は ctx 側(ワーカーごと、think ごとにクリア)。
	SearchContext* ctx = nullptr;

	// ply==1 の相手ノードが「確率混合の値」を返したか(呼び出しごとに1回だけ立つ)。
	//
	// 混合値は子を飽和させてから重み付けしたもので、**すでに混合空間**
	// (通常 ±2900 = MIX_MAX / 詰み ±3000 = MATE_MIX)にいる。think() が返り値に
	// もう一度 squash_cp を掛けると、詰み寄りの値(2500超)が通常評価の上限 2500 に
	// 潰れて、**深いところで見つけた詰みが「ふつうの優勢」と区別できなくなる**。
	// このフラグが立っているときは think() 側で二重に squash しないこと。
	//
	// 本物の詰みスコアを返す経路(mated_in / exhaustive な全詰み)ではフラグは
	// 立たない。そちらは squash_cp を通して ±3000 になるのが正しい。
	//
	// ※ DSearch は1回の探索につき1つ作る前提(think() がループ内で毎回作る)。
	bool          rootMixed = false;

	// 手番側から見た評価値(centipawn相当, 詰みは±(32000-ply))を返す。
	Value search(Position& pos, int depth, Value alpha, Value beta, int ply);
	Value qsearch(Position& pos, Value alpha, Value beta, int ply);

private:
	// 相手ノードを非千里眼モデルで評価する。返り値は相手視点(negamaxの規約どおり)。
	//
	//   V_opp = (1−λ)·Σ ŵ(m)·V_child(m)  +  λ·max_m V_child(m)  −  fG·E[反則回数]
	//
	// ŵ は相手の意図prior(こちらの駒が見えない前提)を合法な応手の上で正規化したもの、
	// λ は千里眼の最善応手を混ぜる割合(実際の相手は自分なりの信念でこちらの駒を
	// 推測してくるので、完全な盲目モデルは逆向きに楽観的すぎる)。
	// 期待値ノードなので子はαβ窓を使えない(全窓で探索する)。
	Value opp_node(Position& pos, int depth, int ply);
};

// 集計に使うためのスコアの飽和変換(詰みスコアを有限に丸める)
double squash_cp(Value v);

} // namespace Tsuitate
} // namespace YaneuraOu

#endif
#endif
