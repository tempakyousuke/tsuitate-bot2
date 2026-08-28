// ついたて将棋エンジン: ローカル審判とアリーナの実装
#include "arena.h"
#include "think.h"

#if defined(TSUITATE_ENGINE)

#include <algorithm>
#include <iostream>

#include "../../movegen.h"

namespace YaneuraOu {
namespace Tsuitate {

namespace {

constexpr int MAX_FOULS = 10;  // サーバー(constants.ts)と同じ

// プレイヤーの共通インターフェース。観測はサイトのイベントと同じ意味論。
struct IPlayer {
	virtual ~IPlayer() = default;
	virtual void new_game(Color us) = 0;
	virtual Move choose() = 0;  // none = 投了
	virtual void on_our_move_accepted(Move m, PieceType capRole, bool gaveCheck) = 0;
	virtual void on_our_foul(Move m) = 0;
	virtual void on_opp_move(Square capSq, bool gaveCheck) = 0;
	virtual void on_opp_foul() = 0;
	// 診断用フック(審判だけが持つ完全情報を渡す。思考には使わない)。
	// どちらも1手番につき1回だけ呼ぶこと(反則のやり直しでは呼ばない)。
	virtual void observe_turn(const Position&) {}
	virtual void observe_verdict(bool /*wasLegal*/) {}
};

// 診断用の集計(プレイヤーごと)
//
// 反則が勝敗を決める競技なので、「信念がどれだけ真の局面に近いか」を
// 審判の完全情報と突き合わせて直接測る。変更はこの数字とセットで評価すること。
//   king_acc … 相手玉のマスを当てている粒子の割合(信念の芯の鋭さ)
//   occ_rec  … 真の相手駒のうち、粒子が正しいマスに置けている割合(再現率)
//   brier    … p_legalの較正誤差(選んだ手の合法性の予測 vs 実際)。小さいほど良い
//   foul_rate… 1回の着手決定が反則になった割合(fouls / decisions)
//
// 手番ごとの指標(avg_particles / avg_p_legal / relax / king_acc / occ_rec / brier)と
// 決定ごとの指標(foul_rate / after_zero / after_relax)は分母が違う。
// 出力にはどちらの分母も出す(turns= と decisions=)。
struct ArenaStats {
	// --- 決定ごと(反則のやり直しも1回と数える) ---
	// 反則率 fouls/decisions は「1回の着手決定が反則になる割合」で、これは
	// やり直しも込みで数えるのが正しい(偏りもない)。
	// 反則の帰属(after_zero / after_relax)も決定ごとなので、比較できるように
	// 「そのとき粒子ゼロだった決定数 / 緩和状態だった決定数」を分母として持つ。
	long long decisions = 0;
	long long decisionsAtZero = 0, decisionsAtRelax = 0;
	long long fouls = 0, foulsAfterZero = 0, foulsAfterRelax = 0;

	// --- 手番ごと(1手番につき1回だけ) ---
	// 反則すると手番は変わらず同じ局面で指し直しになる。指し直しまで数えると
	// 「反則が多い設定ほど、信念が痛んだ直後の思考を何度も数える」ことになり、
	// 平均が反則率で重み付いてしまう。設定間で比べる指標はすべて手番単位で取る。
	long long turns = 0;
	long long particleSum = 0, zeroParticle = 0;
	double    pLegalSum = 0;     // 整数%で持つと二重に切り捨てて0.5pp沈むのでdoubleで持つ
	long long relaxHist[4] = {};  // 緩和レベル別の手番数(3=合成粒子)
	double    kingAccSum = 0, occAccSum = 0;
	long long truthSamples = 0;
	double    brierSum = 0;
	long long brierSamples = 0;

	void add_decision(const ThinkResult& r) {
		decisions++;
		if (r.nParticles == 0)
			decisionsAtZero++;
		else if (r.relaxLevel > 0)
			decisionsAtRelax++;
	}
	void add_turn(const ThinkResult& r) {
		turns++;
		particleSum += r.nParticles;
		pLegalSum += r.pLegal;
		if (r.nParticles == 0)
			zeroParticle++;
		if (r.relaxLevel >= 0 && r.relaxLevel < 4)
			relaxHist[r.relaxLevel]++;
	}
};
ArenaStats g_stats[2];

// 本体(信念+確定化探索)
struct BeliefPlayer : IPlayer {
	BotCore core;
	Config  cfg;
	int     budgetMs;
	int     slot;  // g_stats のインデックス(p1=0 / p2=1)

	BeliefPlayer(const Config& c, int budget, int slot_)
	    : cfg(c), budgetMs(budget), slot(slot_) {}

	ThinkResult lastResult;

	void new_game(Color us) override { core.new_game(us, cfg); }
	Move choose() override {
		ThinkResult r = core.think(budgetMs);
		g_stats[slot].add_decision(r);
		lastResult = r;
		return r.best;
	}
	// 1手番につき1回だけ呼ばれる診断フック(思考には一切使わない)。
	// 直前の choose() の結果と、審判だけが持つ完全情報を突き合わせる。
	void observe_turn(const Position& truth) override {
		g_stats[slot].add_turn(lastResult);
		const auto& parts = core.belief().particles();
		if (parts.empty()) {
			// 粒子が1つもない手番を数えないと、信念が壊れやすい設定ほど
			// 都合の悪い手番だけが標本から抜けて king_acc / occ_rec が良く見える。
			// 「1つも当てられていない」= 0 として数える。
			g_stats[slot].truthSamples++;
			return;
		}
		const Color opp     = ~core.view().us;
		const Square trueK  = truth.square<KING>(opp);
		uint64_t     trueOcc[2] = {};  // 相手駒の占有(81bitを2ワードに)
		for (auto sq : SQ) {
			Piece pc = truth.piece_on(sq);
			if (pc != NO_PIECE && color_of(pc) == opp)
				trueOcc[sq >= 64] |= 1ull << (sq & 63);
		}
		long long kingHit = 0, occHit = 0;
		for (const auto& p : parts) {
			if (p->pos.square<KING>(opp) == trueK)
				++kingHit;
			uint64_t o0 = 0, o1 = 0;
			for (auto sq : SQ) {
				Piece pc = p->pos.piece_on(sq);
				if (pc != NO_PIECE && color_of(pc) == opp)
					(sq >= 64 ? o1 : o0) |= 1ull << (sq & 63);
			}
			// 真の相手駒のマスを言い当てた数
			occHit += POPCNT64(o0 & trueOcc[0]) + POPCNT64(o1 & trueOcc[1]);
		}
		int trueN = POPCNT64(trueOcc[0]) + POPCNT64(trueOcc[1]);
		ArenaStats& st = g_stats[slot];
		st.kingAccSum += double(kingHit) / double(parts.size());
		if (trueN > 0)
			st.occAccSum += double(occHit) / double(parts.size()) / double(trueN);
		st.truthSamples++;
	}
	// 選んだ手が実際に合法だったか(p_legalの較正=Brierスコア)。
	// king_acc / occ_rec と同じく1手番につき1回だけ数える(呼び出し側で制御)。
	// 反則のやり直しまで数えると、反則が多い設定ほど標本が増えて
	// 指標が反則率で重み付いてしまい、反則率とは別の証拠として使えなくなる。
	void observe_verdict(bool wasLegal) override {
		// 粒子ゼロの手番は「p_legal の予測そのものが存在しない」ので数えられない
		//(think() はヒューリスティックに落ちて pLegal を既定の0のまま返す)。
		// ただし除外したぶん母集団が設定によって変わるので、brier_n を出力して
		// どれだけ落としたかが分かるようにする(turns と突き合わせれば分かる)。
		if (lastResult.best == Move::none() || lastResult.nParticles == 0)
			return;
		double d = lastResult.pLegal - (wasLegal ? 1.0 : 0.0);
		g_stats[slot].brierSum += d * d;
		g_stats[slot].brierSamples++;
	}
	void on_our_move_accepted(Move m, PieceType capRole, bool gaveCheck) override {
		core.on_our_move_accepted(m, capRole);
		if (gaveCheck)
			core.on_check_declared(false);
	}
	void on_our_foul(Move m) override {
		core.on_our_foul(m);
		g_stats[slot].fouls++;
		if (lastResult.nParticles == 0)
			g_stats[slot].foulsAfterZero++;
		else if (lastResult.relaxLevel > 0)
			g_stats[slot].foulsAfterRelax++;
	}
	void on_opp_move(Square capSq, bool gaveCheck) override {
		core.on_opp_move(capSq);
		if (gaveCheck)
			core.on_check_declared(true);
	}
	void on_opp_foul() override { core.on_opp_foul(); }
};

// サイト内蔵bot相当のヒューリスティック(前進+成り+乱数)。基準棋力。
struct HeuristicPlayer : IPlayer {
	OwnView           view;
	std::vector<Move> foulTried;
	PRNG              rng;

	explicit HeuristicPlayer(uint64_t seed) : rng(seed) {}

	void new_game(Color us) override {
		view.reset(us);
		foulTried.clear();
	}
	Move choose() override {
		Move best = Move::none();
		double bestScore = -1e18;
		for (Move m : generate_candidates(view)) {
			if (std::find(foulTried.begin(), foulTried.end(), m) != foulTried.end())
				continue;
			double s = double(rng.rand<uint64_t>() % 1000) / 250.0;
			if (!m.is_drop()) {
				int adv = view.us == BLACK
				              ? int(rank_of(m.from_sq())) - int(rank_of(m.to_sq()))
				              : int(rank_of(m.to_sq())) - int(rank_of(m.from_sq()));
				s += adv;
				if (m.is_promote())
					s += 3;
				if (type_of(view.board[m.from_sq()]) == KING)
					s -= 2;
			}
			if (s > bestScore) { bestScore = s; best = m; }
		}
		return best;
	}
	void on_our_move_accepted(Move m, PieceType capRole, bool) override {
		view.apply_our_move(m, capRole);
		foulTried.clear();
	}
	void on_our_foul(Move m) override {
		view.ourFouls++;
		foulTried.push_back(m);
	}
	void on_opp_move(Square capSq, bool) override {
		view.apply_opp_capture(capSq);
		foulTried.clear();
	}
	void on_opp_foul() override { view.oppFouls++; }
};

std::unique_ptr<IPlayer> make_player(const std::string& kind, const ArenaOptions& opt,
                                     uint64_t seed, int slot) {
	if (kind == "belief") {
		Config c = slot == 0 ? opt.cfg : opt.cfg2;
		c.seed   = seed;
		return std::make_unique<BeliefPlayer>(c, c.budgetMs, slot);
	}
	// ここに来るのは "heuristic" だけ(valid_player_kind が入口で弾いている)
	return std::make_unique<HeuristicPlayer>(seed);
}

struct GameStat {
	int winner = -1;  // 0=先手,1=後手,-1=引き分け
	std::string reason;
	int fouls[2] = {0, 0};
	int plies = 0;
};

GameStat play_one(IPlayer& sente, IPlayer& gote, const ArenaOptions& opt, bool verbose) {
	GameStat stat;
	Position pos;
	std::deque<StateInfo> sts;
	sts.emplace_back();
	pos.set_hirate(&sts.back());

	IPlayer* players[2] = {&sente, &gote};
	sente.new_game(BLACK);
	gote.new_game(WHITE);
	int fouls[2] = {0, 0};
	// 反則しても手番は変わらず同じ局面で指し直しになる。信念の診断を毎回取ると
	// 「反則が多い設定ほど同じ局面を何度も数える」ことになり、
	// king_acc / occ_rec が反則率で重み付いてしまう。1手番につき1回だけ取る。
	bool retryOfSameTurn = false;

	while (true) {
		if (stat.plies >= opt.maxPlies) {
			stat.reason = "max_plies";
			break;
		}
		int      side  = pos.side_to_move() == BLACK ? 0 : 1;
		IPlayer* mover = players[side];
		IPlayer* other = players[1 - side];

		Move m = mover->choose();
		// 診断は choose() のあとに取る。信念の同期・再生成は think() の中で走るので、
		// 先に取ると「相手の直前の手をまだ反映していない粒子」を今の真の盤と
		// 比べることになり、この変更が効かせたい経路そのものを測り損ねる。
		// pos は自分の着手前なので、真の盤としてはここでも同じもの。
		if (!retryOfSameTurn)
			mover->observe_turn(pos);
		if (m == Move::none()) {
			stat.winner = 1 - side;
			stat.reason = "resign";
			break;
		}

		// 審判: 通常将棋ルールでの合法性
		const bool wasLegal = pos.pseudo_legal_s<true>(m) && pos.legal(m);
		if (!retryOfSameTurn)
			mover->observe_verdict(wasLegal);
		if (!wasLegal) {
			fouls[side]++;
			retryOfSameTurn = true;
			mover->on_our_foul(m);
			// 相手の反則はサイトから両者に通知される(ブリッジは oppfoul を送る)。
			// アリーナだけ通知していなかったため、view.oppFouls が常に0のままで、
			// 相手の反則累計を見る foulOppW が「唯一A/Bできる場所」で無効化されていた。
			other->on_opp_foul();
			if (fouls[side] >= MAX_FOULS) {
				stat.winner = 1 - side;
				stat.reason = "foul_limit";
				break;
			}
			continue;  // 手番は変わらない
		}

		Piece     cap     = m.is_drop() ? NO_PIECE : pos.piece_on(m.to_sq());
		PieceType capRole = cap == NO_PIECE ? NO_PIECE_TYPE : type_of(cap);
		bool      gives   = pos.gives_check(m);

		sts.emplace_back();
		pos.do_move(m, sts.back());
		stat.plies++;
		retryOfSameTurn = false;

		mover->on_our_move_accepted(m, capRole, gives);
		other->on_opp_move(capRole == NO_PIECE_TYPE ? SQ_NB : m.to_sq(), gives);

		if (verbose)
			std::cout << "# " << stat.plies << " " << to_usi_string(m)
			          << (gives ? " (check)" : "") << std::endl;

		// 詰み・ステイルメイト(合法手なし=手番側の負け)
		if (MoveList<LEGAL_ALL>(pos).size() == 0) {
			stat.winner = side;
			stat.reason = pos.in_check() ? "checkmate" : "stalemate";
			break;
		}
	}
	stat.fouls[0] = fouls[0];
	stat.fouls[1] = fouls[1];
	return stat;
}

} // namespace

bool valid_player_kind(const std::string& kind) {
	return kind == "belief" || kind == "heuristic";
}

const char* player_kind_list() { return "belief | heuristic"; }

void run_arena(const ArenaOptions& opt) {
	// regenfloor は hardMin = min(particles, max(8, particles/16)) で床上げされる。
	// 黙って効かない値でスイープすると「このつまみは無反応」と誤読するので、
	// 実効値が設定値と違うときは明示する。
	for (int k = 0; k < 2; ++k) {
		const Config& c = k == 0 ? opt.cfg : opt.cfg2;
		if ((k == 0 ? opt.p1 : opt.p2) != "belief")
			continue;
		size_t want    = size_t(c.particles);
		size_t hardMin = std::min(want, std::max<size_t>(8, want / 16));
		size_t asked   = want * size_t(c.regenFloorPct) / 100;
		if (asked < hardMin)
			std::cout << "info string note: p" << (k + 1) << " regenfloor=" << c.regenFloorPct
			          << " は粒子数 " << want << " では下限 " << hardMin
			          << " 個(=" << (100 * hardMin / (want ? want : 1))
			          << "%)に床上げされます" << std::endl;
		// blockcp が黙って無効化される/二重に効くと「効かないつまみをスイープして
		// いる」ことに気づけない(A/Bが丸ごと無意味になる)。
		// 条件は think.cpp の oppNodeCountsFouls と厳密に揃えること。
		if (c.blockCp > 0.0 && c.oppModel > 0 && c.foulGainScale > 0.0) {
			if (c.oppReplyKStage1 > 0)
				std::cout << "info string note: p" << (k + 1) << " blockcp=" << c.blockCp
				          << " は oppmodel=" << c.oppModel << " / foulgain=" << c.foulGainScale
				          << " / oppreplykstage1=" << c.oppReplyKStage1
				          << " のとき無効化されます(相手ノードが同じ妨害の価値を数えるため)"
				          << std::endl;
			else
				std::cout << "info string note: p" << (k + 1) << " blockcp=" << c.blockCp
				          << " は oppreplykstage1=0 なので有効なままですが、"
				          << "stage2 の相手ノードも foulgain=" << c.foulGainScale
				          << " で同じ妨害の価値を数えるため二重計上になります"
				          << std::endl;
		}
	}

	int p1Wins = 0, p2Wins = 0, draws = 0;
	long long p1Fouls = 0, p2Fouls = 0, plies = 0;
	// 決着理由の内訳。信念を良くしても反則の消耗戦で勝っているだけ、という状態を
	// 総合勝率は隠してしまう(第2版は勝ち星38のうち32が反則負け由来で、
	// 盤上で決着した12局は6勝6敗の互角だった)。
	// 「盤上で勝てるようになったか」は総合勝率とは別に数える。
	int boardGames = 0, boardP1 = 0, boardP2 = 0;  // 詰み/ステイルメイト/投了
	int foulGames  = 0, foulP1  = 0, foulP2  = 0;  // 反則負け
	int otherGames = 0;                            // 手数切れ
	TimePoint t0 = now();

	for (int g = 0; g < opt.games; ++g) {
		uint64_t s1 = opt.seed + g * 2, s2 = opt.seed + g * 2 + 1;
		auto     a  = make_player(opt.p1, opt, s1, 0);
		auto     b  = make_player(opt.p2, opt, s2, 1);
		// 先後を交互に入れ替える
		bool p1Sente = (g % 2 == 0);
		GameStat st  = p1Sente ? play_one(*a, *b, opt, opt.verbose)
		                       : play_one(*b, *a, opt, opt.verbose);
		int p1Side = p1Sente ? 0 : 1;
		const bool p1Won = st.winner == p1Side;
		if (st.winner == -1)
			draws++;
		else if (p1Won)
			p1Wins++;
		else
			p2Wins++;

		// 決着理由の内訳。反則負け以外で終わった局が「盤上で決着した局」。
		if (st.winner == -1) {
			otherGames++;
		} else if (st.reason == "foul_limit") {
			foulGames++;
			(p1Won ? foulP1 : foulP2)++;
		} else {
			boardGames++;
			(p1Won ? boardP1 : boardP2)++;
		}
		p1Fouls += st.fouls[p1Side];
		p2Fouls += st.fouls[1 - p1Side];
		plies += st.plies;
		// A/B(p1cfg/p2cfg で片側だけ設定を変える)では両者とも種別名が "belief" に
		// なるので、種別名だけでは勝者が読めない。p1/p2 を必ず添える。
		std::cout << "info arena game " << (g + 1) << "/" << opt.games
		          << " winner=" << (st.winner == -1 ? std::string("draw")
		                            : p1Won ? "p1(" + opt.p1 + ")" : "p2(" + opt.p2 + ")")
		          << " sente=" << (p1Sente ? "p1" : "p2")
		          << " reason=" << st.reason << " plies=" << st.plies
		          << " fouls=" << st.fouls[p1Side] << "/" << st.fouls[1 - p1Side]
		          << std::endl;
	}

	double n = double(opt.games);
	std::cout << "arena result: " << opt.p1 << " " << p1Wins << " - " << draws
	          << " - " << p2Wins << " " << opt.p2 << std::endl;
	std::cout << "  p1 win rate = " << (100.0 * p1Wins / n) << "%"
	          << ", fouls/game p1=" << (p1Fouls / n) << " p2=" << (p2Fouls / n)
	          << ", avg plies=" << (plies / n)
	          << ", elapsed=" << (now() - t0) / 1000 << "s" << std::endl;
	// 盤上決着の勝敗は「勝ちにいく力」の直接の指標。総合勝率と分けて見ること。
	std::cout << "  decided: board=" << boardGames
	          << " (" << opt.p1 << " " << boardP1 << " - " << boardP2 << " " << opt.p2 << ")"
	          << " foul=" << foulGames
	          << " (" << opt.p1 << " " << foulP1 << " - " << foulP2 << " " << opt.p2 << ")"
	          << " other=" << otherGames
	          << " board_rate=" << (100.0 * boardGames / n) << "%" << std::endl;
	for (int k = 0; k < 2; ++k) {
		const ArenaStats& g = g_stats[k];
		if (g.decisions == 0)
			continue;
		// decisions == 0 の行は上で弾いてあり、decisions >= 1 なら
		// observe_turn も必ず1回は走っているので turns >= 1。ゼロ除算はない。
		const double T = double(g.turns);
		std::cout << "  belief diag[" << (k == 0 ? opt.p1 : opt.p2) << "#" << (k + 1) << "]:"
		          << " turns=" << g.turns << " decisions=" << g.decisions
		          << " avg_particles=" << (double(g.particleSum) / T)
		          << " relax(0/1/2/synth)=" << g.relaxHist[0] << "/" << g.relaxHist[1]
		          << "/" << g.relaxHist[2] << "/" << g.relaxHist[3]
		          << " zero_particle=" << g.zeroParticle
		          << " avg_p_legal=" << (100.0 * g.pLegalSum / T) << "%"
		          << " fouls=" << g.fouls
		          << " foul_rate=" << (100.0 * double(g.fouls) / double(g.decisions)) << "%"
		          // 帰属は決定ごと。分母も決定ごとで出さないと手番単位の
		          // zero_particle / relax(...) と突き合わせられない
		          << " (after_zero=" << g.foulsAfterZero << "/" << g.decisionsAtZero
		          << " after_relax=" << g.foulsAfterRelax << "/" << g.decisionsAtRelax << ")";
		if (g.truthSamples)
			std::cout << " king_acc=" << (100.0 * g.kingAccSum / double(g.truthSamples)) << "%"
			          << " occ_rec=" << (100.0 * g.occAccSum / double(g.truthSamples)) << "%";
		if (g.brierSamples)
			std::cout << " brier=" << (g.brierSum / double(g.brierSamples))
			          << " brier_n=" << g.brierSamples;
		std::cout << std::endl;
	}
	g_stats[0] = ArenaStats();
	g_stats[1] = ArenaStats();
}

} // namespace Tsuitate
} // namespace YaneuraOu

#endif // TSUITATE_ENGINE
