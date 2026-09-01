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
// 世代(gen)は think() の呼び出しごとに進める。値による枝刈り(カットオフ)は
// **同世代のエントリだけ**に許す: 局面評価は手番内では純粋に (局面, 深さ) の
// 関数だが、手番をまたぐと oppFouls に依存する項(foulGain)が変わりうるため。
// 旧世代のエントリは指し手のオーダリングにだけ使う(こちらは値が古くても無害)。
//
// gen は uint16: uint8 だと think() 256回(反則のやり直しも1回と数えるので
// 1局内でも到達しうる)で一巡し、256世代前のエントリが「同世代」に化けて
// 別の foulGain / 別の対局で計算した値がカットオフに使われる。uint16 の一巡には
// 1局で65536回の決定が要り、さらに対局開始(new_game)でコンテキストごと
// 破棄されるので、実質到達不能。
struct TTEntry {
	uint64_t key   = 0;  // pos.key()(0 = 空きスロット)
	int16_t  value = 0;  // value_to_tt 済み(詰みはply補正済み)
	uint16_t move16 = 0; // 最善手(Move::raw()。オーダリング用)
	int8_t   depth = -1;
	Bound    bound = BOUND_NONE;  // types.h の共通enum(独自enumで数値をずらさない)
	uint16_t gen   = 0;
};
static_assert(sizeof(TTEntry) == 16, "TTEntry should stay 16 bytes");

struct SearchContext {
	static constexpr size_t TT_BITS = 20;                  // 2^20 = 1M エントリ(16MB)
	static constexpr int    HIST_MAX = 16384;              // history の飽和値
	std::vector<TTEntry> tt;
	// history[手番(0=BLACK)][Move::raw()]。quietの beta カットで depth^2 を加点
	std::vector<int16_t> hist;
	// killer もここに置く(ワーカーごと・think ごとにクリア)。DSearch のメンバに
	// すると (候補,粒子) ジョブごとの構築で毎回 ~2KB のゼロ初期化が走り、
	// killer を一度も読まない既定(tt 0)経路まで恒常コストを払うことになる。
	Move     killer[MAX_PLY][2];
	uint16_t gen   = 0;
	// この think() でもう begin_think 済みかの判定(Thinker が通し番号を発行)。
	// コンテキストは1リージョン内では担当ワーカーだけが触り、リージョン間は
	// run_workers の join が順序づけるので、単純な比較で足りる。
	uint32_t stamp = 0;

	// think() ごと・ワーカーごとに1回。
	//   - 初回は割り当て(16MB)をここで行う: 呼び出しスレッドで全ワーカー分を
	//     まとめて確保すると、初手の予算内で workers×16MB のゼロ初期化と
	//     first-touch が直列に走ってしまう。ワーカー自身にやらせて分散する
	//   - 2回目以降は世代を進めて前手番の値カットオフを無効化し、history は
	//     半減させる(減衰なしだと長い対局で飽和して序列の分解能が落ちる)
	void begin_think() {
		if (tt.empty()) {
			tt.resize(size_t(1) << TT_BITS);
			hist.assign(2 * 65536, 0);
		} else {
			++gen;
			for (auto& h : hist)
				h = int16_t(h / 2);
		}
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

	// --- §3.2 置換表 + オーダリング(cfg->tt != 0 のときだけ think() が設定する) ---
	// nullptr なら従来の探索(MVV-LVAのみ)と完全に同一の経路。
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
