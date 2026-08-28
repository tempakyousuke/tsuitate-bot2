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
	// 診断用フック(審判だけが持つ完全情報を渡す。思考には使わない)
	virtual void observe_truth(const Position&) {}
	virtual void observe_verdict(bool /*wasLegal*/) {}
};

// 診断用の集計(プレイヤーごと)
//
// 反則が勝敗を決める競技なので、「信念がどれだけ真の局面に近いか」を
// 審判の完全情報と突き合わせて直接測る。変更はこの数字とセットで評価すること。
//   king_acc … 相手玉のマスを当てている粒子の割合(信念の芯の鋭さ)
//   occ_rec  … 真の相手駒のうち、粒子が正しいマスに置けている割合(再現率)
//   brier    … p_legalの較正誤差(選んだ手の合法性の予測 vs 実際)。小さいほど良い
struct ArenaStats {
	long long thinks = 0, particleSum = 0, relaxSum = 0, zeroParticle = 0;
	long long pLegalPctSum = 0;
	long long fouls = 0, foulsAfterZero = 0, foulsAfterRelax = 0;
	double    kingAccSum = 0, occAccSum = 0;
	long long truthSamples = 0;
	double    brierSum = 0;
	long long brierSamples = 0;
	long long relaxHist[4] = {};  // 緩和レベル別の思考回数(3=合成粒子)
	void add(const ThinkResult& r) {
		thinks++;
		particleSum += r.nParticles;
		relaxSum += r.relaxLevel;
		pLegalPctSum += (long long)(r.pLegal * 100);
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
		g_stats[slot].add(r);
		lastResult = r;
		return r.best;
	}
	// 審判の完全情報と信念を突き合わせる(診断専用。思考には一切使わない)
	void observe_truth(const Position& truth) override {
		const auto& parts = core.belief().particles();
		if (parts.empty())
			return;
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
	// 選んだ手が実際に合法だったか(p_legalの較正=Brierスコア)
	void observe_verdict(bool wasLegal) override {
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
		return std::make_unique<BeliefPlayer>(c, opt.budgetMs, slot);
	}
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
		mover->observe_truth(pos);
		if (m == Move::none()) {
			stat.winner = 1 - side;
			stat.reason = "resign";
			break;
		}

		// 審判: 通常将棋ルールでの合法性
		const bool wasLegal = pos.pseudo_legal_s<true>(m) && pos.legal(m);
		mover->observe_verdict(wasLegal);
		if (!wasLegal) {
			fouls[side]++;
			mover->on_our_foul(m);
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

void run_arena(const ArenaOptions& opt) {
	int p1Wins = 0, p2Wins = 0, draws = 0;
	long long p1Fouls = 0, p2Fouls = 0, plies = 0;
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
		if (st.winner == -1)
			draws++;
		else if (st.winner == p1Side)
			p1Wins++;
		else
			p2Wins++;
		p1Fouls += st.fouls[p1Side];
		p2Fouls += st.fouls[1 - p1Side];
		plies += st.plies;
		std::cout << "info arena game " << (g + 1) << "/" << opt.games
		          << " winner=" << (st.winner == -1 ? "draw" : st.winner == p1Side ? opt.p1 : opt.p2)
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
	for (int k = 0; k < 2; ++k) {
		const ArenaStats& g = g_stats[k];
		if (g.thinks == 0)
			continue;
		std::cout << "  belief diag[" << (k == 0 ? opt.p1 : opt.p2) << "#" << (k + 1) << "]:"
		          << " thinks=" << g.thinks
		          << " avg_particles=" << (g.particleSum / g.thinks)
		          << " relax(0/1/2/synth)=" << g.relaxHist[0] << "/" << g.relaxHist[1]
		          << "/" << g.relaxHist[2] << "/" << g.relaxHist[3]
		          << " zero_particle=" << g.zeroParticle
		          << " avg_p_legal=" << (g.pLegalPctSum / g.thinks) << "%"
		          << " fouls=" << g.fouls
		          << " (after_zero=" << g.foulsAfterZero
		          << " after_relax=" << g.foulsAfterRelax << ")";
		if (g.truthSamples)
			std::cout << " king_acc=" << (100.0 * g.kingAccSum / double(g.truthSamples)) << "%"
			          << " occ_rec=" << (100.0 * g.occAccSum / double(g.truthSamples)) << "%";
		if (g.brierSamples)
			std::cout << " brier=" << (g.brierSum / double(g.brierSamples));
		std::cout << std::endl;
	}
	g_stats[0] = ArenaStats();
	g_stats[1] = ArenaStats();
}

} // namespace Tsuitate
} // namespace YaneuraOu

#endif // TSUITATE_ENGINE
