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
