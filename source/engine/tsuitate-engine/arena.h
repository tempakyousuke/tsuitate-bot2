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
	int      maxPlies    = 400;    // 引き分け打ち切り
	uint64_t seed        = 20260827;
	// プレイヤー種別: "belief"(本体) / "heuristic"(前進ヒューリスティック)
	std::string p1 = "belief";
	std::string p2 = "heuristic";
	// belief側の設定。p1/p2で別々に持てるので、同一バイナリ内でA/B比較ができる
	// (`p1cfg <key> <val>` / `p2cfg <key> <val>` で個別に上書きする)。
	// 1手の思考予算も cfg.budgetMs / cfg2.budgetMs から取るので、
	// 予算そのものをA/Bの対象にできる。
	Config   cfg;
	Config   cfg2;
	bool     verbose = false;
};

// 対戦を実行し、結果をstdoutへ出力する
void run_arena(const ArenaOptions& opt);

// 有効なプレイヤー種別か。make_player の分岐と同じ集合をここ1か所で持つ。
// (パーサ側にリストを複製すると、種別を増やしたときに片方だけ直して
//  「beliefと対戦したつもりが内蔵bot相当だった」という取り違えに戻る)
bool valid_player_kind(const std::string& kind);

// 種別の一覧(エラーメッセージ用)
const char* player_kind_list();

} // namespace Tsuitate
} // namespace YaneuraOu

#endif
#endif
