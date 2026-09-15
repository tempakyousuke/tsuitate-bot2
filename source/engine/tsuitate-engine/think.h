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
#include "dsearch.h"

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
	double      ess = 0;          // §2 SIR: 実効サンプル数(再生成前に計測。sir=0では再生成前の粒子数)
	int         depthReached = 0;
	uint64_t    nodes = 0;        // 確定化探索の総ノード数(診断。nodes/sの分子)
	TimePoint   elapsedMs = 0;

	// §9 stage2 スケジューリングの診断。
	//   jobs2 / trunc2: stage2 で完走したパスのジョブ数と、そのうちノード上限
	//     (nodesLimit2)に当たって値が汚れたジョブ数
	//   gateSkipped: まだ深いパスが残っていたのに開始ゲートで止めた(1/0)
	//   passes: 完走したパスの (深さ, 所要ms)。予測ゲートの較正(passGrowth)用
	uint64_t    jobs2 = 0, trunc2 = 0;
	int         gateSkipped = 0;
	std::vector<std::pair<int, int>> passes;
	// 予算の内訳(ms): 信念の同期 / stage1 / stage2(残りは選択などの端数)
	TimePoint   syncMs = 0, stage1Ms = 0, stage2Ms = 0;
	// 候補手の数と stage1 のジョブ数(候補×粒子)。stage1 の重さの診断
	size_t      cands = 0;
	uint64_t    jobs1 = 0;

	// スループット(kilo nodes / 秒 = nodes / 経過ms)。分母は信念同期込みの
	// 思考時間全体。knps の**定義はここ1つ**: 実対局の info 行も、アリーナ診断
	// (合計値のプール)も同じ「nodes/経過ms」で、配備とアリーナのゲート数値を
	// そのまま突き合わせられるようにする(式が2か所に分かれて整数除算などで
	// ずれると、スループット退行の監視という目的自体が壊れる)。
	double knps() const { return elapsedMs > 0 ? double(nodes) / double(elapsedMs) : 0.0; }
};

class Thinker {
public:
	ThinkResult think(const OwnView& view, Belief& belief, const GameHistory& hist,
	                  const std::vector<Move>& foulTried, int budgetMs,
	                  const Config& cfg, PRNG& rng);

	// 対局開始時に呼ぶ。探索コンテキスト(置換表・history)を破棄して、
	// 前の対局のエントリが次の対局に持ち越されないようにする
	// (world(手番色・反則経済)が違う対局の値でカットオフさせない)。
	void new_game() {
		ctx_.clear();
		thinkStamp_ = 0;
		jobMs1_ = 0.0;
		for (auto& v : jobMs2_)
			v = 0.0;
	}

private:
	// §3.2 ワーカースレッドごとの探索コンテキスト(置換表 + history)。
	// cfg.tt != 0 のときだけ確保する。手番をまたいで保持し、世代で無効化する
	// (詳細は dsearch.h の SearchContext)。
	std::vector<std::unique_ptr<SearchContext>> ctx_;
	// think() の通し番号。SearchContext::stamp と突き合わせて
	// 「この think でもう begin_think したか」をワーカー自身が判定する。
	uint32_t thinkStamp_ = 0;

	// §9 予算スケジューラのコストモデル: 1ジョブ((候補,粒子)の探索)あたりの
	// 実測 wall 時間(ms)の指数移動平均。stage1(qsearch)と stage2 の深さ別。
	// 対局内で学習し、new_game で捨てる(スレッド数・予算・局面の複雑さで変わる
	// 量なので、対局をまたいで持ち越さない)。0 = 未観測。
	static constexpr int JOB_MS_SLOTS = 65;  // searchDepth の上限(64)+1
	double   jobMs1_ = 0.0;
	double   jobMs2_[JOB_MS_SLOTS] = {};
	static double ema_update(double prev, double x) { return prev > 0.0 ? 0.7 * prev + 0.3 * x : x; }
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
