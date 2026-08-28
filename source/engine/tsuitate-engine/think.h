// ついたて将棋エンジン: 思考部(候補手の期待値評価)とボット中核
//
// 候補手の評価:
//   combined(m) = p_legal(m) × E[探索値 | mが合法な粒子] + (1 - p_legal(m)) × 反則コスト
//
//   - p_legal: 全粒子での合法率(反則確率の推定)
//   - E[探索値]: mが合法な粒子をサンプルし、確定化した完全局面としてαβ探索した値の平均
//   - 反則コスト: 反則累計に応じて増加(累計10回で反則負け)
//
// 2段階: stage1は全候補を静止探索で粗く序列化し、上位だけをstage2で深く読む。
#ifndef TSUITATE_THINK_H_INCLUDED
#define TSUITATE_THINK_H_INCLUDED

#include "tsuitate_common.h"
#include "belief.h"

#if defined(TSUITATE_ENGINE)

namespace YaneuraOu {
namespace Tsuitate {

struct ThinkResult {
	Move        best = Move::none();  // none = 投了(指せる手がない)
	double      pLegal = 0;
	double      expectedCp = 0;
	size_t      nParticles = 0;
	int         relaxLevel = 0;   // 診断表示用(0..3)
	double      relaxMean = 0;    // 反則コスト割増に使う連続値
	int         depthReached = 0;
	TimePoint   elapsedMs = 0;
};

class Thinker {
public:
	ThinkResult think(const OwnView& view, Belief& belief, const GameHistory& hist,
	                  const std::vector<Move>& foulTried, int budgetMs,
	                  const Config& cfg, PRNG& rng);
};

// ---------------------------------------------------------------------------
// BotCore: 観測イベントの受付と思考をまとめた中核。
// 実対局(プロトコルループ)とローカルアリーナの両方から使う。
// ---------------------------------------------------------------------------
class BotCore {
public:
	void new_game(Color us, const Config& cfg);

	// --- 観測イベント(到着順に呼ぶこと) ---
	void on_our_move_accepted(Move m, PieceType capRole);
	void on_our_foul(Move m);
	void on_opp_move(Square capSq);  // capSq==SQ_NB なら取られていない
	void on_opp_foul();
	// 直前の着手イベントへの王手宣言(サーバーは着手通知の直後に送る)
	void on_check_declared(bool onUs);

	ThinkResult think(int budgetMs);

	const OwnView&     view() const { return view_; }
	const GameHistory& history() const { return hist_; }
	Belief&            belief() { return belief_; }
	const std::vector<Move>& foul_tried() const { return foulTried_; }

private:
	// 新イベントが来た時点で、直前の着手の「王手なし」が確定する
	void finalize_no_check();

	Config            cfg_;
	OwnView           view_;
	GameHistory       hist_;
	Belief            belief_;
	Thinker           thinker_;
	std::vector<Move> foulTried_;  // この手番中に反則になった手
	PRNG              rng_{20260827};
};

} // namespace Tsuitate
} // namespace YaneuraOu

#endif
#endif
