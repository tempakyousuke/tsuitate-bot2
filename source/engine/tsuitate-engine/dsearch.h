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

	// 手番側から見た評価値(centipawn相当, 詰みは±(32000-ply))を返す。
	Value search(Position& pos, int depth, Value alpha, Value beta, int ply);
	Value qsearch(Position& pos, Value alpha, Value beta, int ply);
};

// 集計に使うためのスコアの飽和変換(詰みスコアを有限に丸める)
double squash_cp(Value v);

} // namespace Tsuitate
} // namespace YaneuraOu

#endif
#endif
