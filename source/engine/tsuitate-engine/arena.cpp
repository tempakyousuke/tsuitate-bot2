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
};

// 診断用の集計(アリーナ全体)
struct ArenaStats {
	long long thinks = 0, particleSum = 0, relaxSum = 0, zeroParticle = 0;
	long long pLegalPctSum = 0;
	long long fouls = 0, foulsAfterZero = 0, foulsAfterRelax = 0;
	void add(const ThinkResult& r) {
		thinks++;
		particleSum += r.nParticles;
		relaxSum += r.relaxLevel;
		pLegalPctSum += (long long)(r.pLegal * 100);
		if (r.nParticles == 0)
			zeroParticle++;
	}
};
ArenaStats g_stats;

// 本体(信念+確定化探索)
struct BeliefPlayer : IPlayer {
	BotCore core;
	Config  cfg;
	int     budgetMs;

	BeliefPlayer(const Config& c, int budget) : cfg(c), budgetMs(budget) {}

	ThinkResult lastResult;

	void new_game(Color us) override { core.new_game(us, cfg); }
	Move choose() override {
		ThinkResult r = core.think(budgetMs);
		g_stats.add(r);
		lastResult = r;
		return r.best;
	}
	void on_our_move_accepted(Move m, PieceType capRole, bool gaveCheck) override {
		core.on_our_move_accepted(m, capRole);
		if (gaveCheck)
			core.on_check_declared(false);
	}
	void on_our_foul(Move m) override {
		core.on_our_foul(m);
		g_stats.fouls++;
		if (lastResult.nParticles == 0)
			g_stats.foulsAfterZero++;
		else if (lastResult.relaxLevel > 0)
			g_stats.foulsAfterRelax++;
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
                                     uint64_t seed) {
	if (kind == "belief") {
		Config c = opt.cfg;
		c.seed   = seed;
		return std::make_unique<BeliefPlayer>(c, opt.budgetMs);
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
		if (m == Move::none()) {
			stat.winner = 1 - side;
			stat.reason = "resign";
			break;
		}

		// 審判: 通常将棋ルールでの合法性
		if (!(pos.pseudo_legal_s<true>(m) && pos.legal(m))) {
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
		auto     a  = make_player(opt.p1, opt, s1);
		auto     b  = make_player(opt.p2, opt, s2);
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
	if (g_stats.thinks > 0)
		std::cout << "  belief diag: thinks=" << g_stats.thinks
		          << " avg_particles=" << (g_stats.particleSum / g_stats.thinks)
		          << " avg_relax=" << (double(g_stats.relaxSum) / g_stats.thinks)
		          << " zero_particle=" << g_stats.zeroParticle
		          << " avg_p_legal=" << (g_stats.pLegalPctSum / g_stats.thinks) << "%"
		          << " fouls=" << g_stats.fouls
		          << " (after_zero=" << g_stats.foulsAfterZero
		          << " after_relax=" << g_stats.foulsAfterRelax << ")"
		          << std::endl;
	g_stats = ArenaStats();
}

} // namespace Tsuitate
} // namespace YaneuraOu

#endif // TSUITATE_ENGINE
