// ついたて将棋エンジン: ローカル審判とアリーナ(自己対戦)
//
// 完全情報を持つ審判がサイトと同じ裁定(反則=通常将棋ルールで不正、
// 累計10回で反則負け、詰み/ステイルメイトで終局)を行い、
// 各プレイヤーには「そのプレイヤーから見える観測」だけを通知する。
// 実サーバー(judge.ts)の移植であり、エンジンの正当性検証と強さ測定に使う。
#ifndef TSUITATE_ARENA_H_INCLUDED
#define TSUITATE_ARENA_H_INCLUDED

#include "tsuitate_common.h"

#if defined(TSUITATE_ENGINE)

namespace YaneuraOu {
namespace Tsuitate {

struct ArenaOptions {
	int      games       = 20;
	int      budgetMs    = 300;    // belief側の1手予算
	int      maxPlies    = 400;    // 引き分け打ち切り
	uint64_t seed        = 20260827;
	// プレイヤー種別: "belief"(本体) / "heuristic"(前進ヒューリスティック)
	std::string p1 = "belief";
	std::string p2 = "heuristic";
	Config   cfg;                  // belief側の設定
	bool     verbose = false;
};

// 対戦を実行し、結果をstdoutへ出力する
void run_arena(const ArenaOptions& opt);

} // namespace Tsuitate
} // namespace YaneuraOu

#endif
#endif
