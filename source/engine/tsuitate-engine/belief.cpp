// ついたて将棋エンジン: 信念状態(粒子フィルタ)の実装
#include "belief.h"

#if defined(TSUITATE_ENGINE)

#include <algorithm>
#include <cmath>

#include "../../movegen.h"
#include "../../evaluate.h"

namespace YaneuraOu {
namespace Tsuitate {

void Belief::reset(Color us, const Config& cfg) {
	us_  = us;
	cfg_ = cfg;
	rng_ = PRNG(cfg.seed);
	cursor_ = 0;
	relaxLevel_ = 0;
	parts_.clear();
	graveyard_.clear();
	curFouls_.clear();
	for (int i = 0; i < cfg_.particles; ++i)
		parts_.push_back(std::make_unique<Particle>());
}

void Belief::bury(const Particle& p) {
	if (p.synthetic || p.oppMoves.empty())
		return;
	// リングバッファ(最古を上書き)。新しい死 = 最長生存 = 最良の種。
	if (graveyard_.size() >= 256) {
		graveyard_[buryIdx_ % graveyard_.size()] = p.oppMoves;
		++buryIdx_;
	} else {
		graveyard_.push_back(p.oppMoves);
	}
}

// 相手手のうち観測(取られたマス・王手宣言)と整合する手を列挙
void Belief::consistent_opp_moves(const Particle& p, const HistEvent& ev,
                                  std::vector<Move>& out, int relax) {
	out.clear();
	for (auto ext : MoveList<LEGAL_ALL>(p.pos)) {
		Move m = ext;
		// 捕獲の整合(常に厳守): 通知があればそのマスへの捕獲、なければ非捕獲
		if (ev.capSq != SQ_NB) {
			if (m.is_drop() || m.to_sq() != ev.capSq)
				continue;
			// 合法手で相手が取れる駒はこちらの駒しかないので piece_on の確認は不要
		} else {
			if (!m.is_drop() && p.pos.piece_on(m.to_sq()) != NO_PIECE)
				continue;
		}
		// 王手宣言の整合(緩和対象)
		if (ev.check == CheckAfter::Yes && relax < 2 && !p.pos.gives_check(m))
			continue;
		if (ev.check == CheckAfter::No && relax < 1 && p.pos.gives_check(m))
			continue;
		out.push_back(m);
	}
}

namespace {

// 非千里眼prior(oppPolicy=1)。
//
// 相手は「こちらの駒が見えない」ので、相手の着手の好みは相手自身の駒の展開だけで
// 決まる、というモデル。実際に観測できるのは「その手が合法だった」場合だけなので、
//   P(相手の手 | 観測) ∝ prior(手) × [その手が真の盤で合法]
// が正しい生成モデルで、合法性フィルタは consistent_opp_moves が担う。
//
// 従来の千里眼モデル(着手後の静的評価のsoftmax)は「相手がこちらの駒を見て
// 取りに来る/当たりを避ける」ことを前提にしていて、構造的にバイアスがある。
// さらにこちらは do_move も評価関数呼び出しも不要なので桁違いに速く、
// 粒子の再生成リプレイ(従来は一様サンプリングだった)でも同じ方策が使える。
int fast_policy_score(const Position& pos, Color opp, Move m) {
	// 駒種ごとの「前進したさ」(centipawn相当)
	static const int PUSH[PIECE_TYPE_NB] = {
		0,    // NO_PIECE_TYPE
		110,  // PAWN
		70,   // LANCE
		85,   // KNIGHT
		95,   // SILVER
		55,   // BISHOP
		65,   // ROOK
		80,   // GOLD
		-40,  // KING(前進はむしろ嫌う)
		90, 70, 80, 85,  // PRO_PAWN, PRO_LANCE, PRO_KNIGHT, PRO_SILVER
		70, 80,          // HORSE, DRAGON
	};
	const Square to = m.to_sq();
	// 端よりは中央
	int s = 10 * (4 - std::abs(int(file_of(to)) - int(FILE_5)));

	if (m.is_drop()) {
		// 打ちは移動手に比べて少数派。敵陣(=こちら側)への打ち込みは好まれる。
		s -= 150;
		if (relative_rank(opp, rank_of(to)) <= RANK_4)
			s += 80;
		return s;
	}

	const Square    from = m.from_sq();
	const PieceType pt   = type_of(pos.piece_on(from));
	// 相手から見た前進量。スライダーの大移動は「通ること」自体が稀なので頭打ちにする
	int adv = int(relative_rank(opp, rank_of(from))) - int(relative_rank(opp, rank_of(to)));
	adv = std::clamp(adv, -2, 3);
	s += PUSH[pt] * adv;
	if (m.is_promote())
		s += 300;
	if (pt == KING)
		s -= 250;  // 玉はむやみに動かさない
	return s;
}

} // namespace

// 特定の1手が観測と整合するかだけを確かめる。
//
// 部分若返り(種の先頭をそのまま再利用する)では「その手が今も整合するか」しか
// 要らないのに、consistent_opp_moves は全合法手を生成して gives_check を全手に
// 掛けてしまう。リプレイは1粒子あたり履歴長ぶん回るので、ここが再生成の律速だった。
bool Belief::opp_move_consistent(const Particle& p, const HistEvent& ev, Move m, int relax) {
	if (!p.pos.pseudo_legal_s<true>(m) || !p.pos.legal(m))
		return false;
	if (ev.capSq != SQ_NB) {
		if (m.is_drop() || m.to_sq() != ev.capSq)
			return false;
	} else {
		if (!m.is_drop() && p.pos.piece_on(m.to_sq()) != NO_PIECE)
			return false;
	}
	if (ev.check == CheckAfter::Yes && relax < 2 && !p.pos.gives_check(m))
		return false;
	if (ev.check == CheckAfter::No && relax < 1 && p.pos.gives_check(m))
		return false;
	return true;
}

// 方策 = softmax(スコア / 温度) + ε一様。excludeの手は候補から外す。
Move Belief::sample_policy(Particle& p, const std::vector<Move>& moves,
                           const std::vector<Move>& exclude) {
	std::vector<Move>  cand;
	for (Move m : moves)
		if (std::find(exclude.begin(), exclude.end(), m) == exclude.end())
			cand.push_back(m);
	if (cand.empty())
		return Move::none();
	if (cand.size() == 1)
		return cand[0];

	const Color opp = ~us_;
	std::vector<double> score(cand.size());
	double mx = -1e18;
	for (size_t i = 0; i < cand.size(); ++i) {
		if (cfg_.oppPolicy != 0) {
			score[i] = double(fast_policy_score(p.pos, opp, cand[i]));
		} else {
			StateInfo st;
			p.pos.do_move(cand[i], st);
			// 着手後は手番が自分側に戻るので、相手から見た評価は符号反転
			score[i] = -double(Eval::evaluate(p.pos));
			p.pos.undo_move(cand[i]);
		}
		mx = std::max(mx, score[i]);
	}
	std::vector<double> w(cand.size());
	double sum = 0;
	for (size_t i = 0; i < cand.size(); ++i) {
		w[i] = std::exp((score[i] - mx) / cfg_.policyTemp);
		sum += w[i];
	}
	// ε一様混合
	for (size_t i = 0; i < cand.size(); ++i)
		w[i] = (1.0 - cfg_.policyEps) * (w[i] / sum) + cfg_.policyEps / double(cand.size());

	double r = double(rng_.rand<uint64_t>() >> 11) / double(1ull << 53);
	double acc = 0;
	for (size_t i = 0; i < cand.size(); ++i) {
		acc += w[i];
		if (r <= acc)
			return cand[i];
	}
	return cand.back();
}

ParticlePtr Belief::clone_of(const GameHistory& hist, const Particle& src) {
	auto p = std::make_unique<Particle>();
	size_t k = 0;
	for (size_t i = 0; i < cursor_; ++i) {
		const HistEvent& ev = hist.events[i];
		if (ev.kind == EvKind::OurMove) {
			p->advance(ev.move);
		} else if (ev.kind == EvKind::OppMove) {
			Move m = src.oppMoves[k++];
			p->oppMoves.push_back(m);
			p->advance(m);
		}
	}
	return p;
}

void Belief::apply_event(const GameHistory& hist, const HistEvent& ev) {
	switch (ev.kind) {

	case EvKind::OurMove: {
		std::vector<ParticlePtr> alive;
		for (auto& p : parts_) {
			bool ok = p->legal(ev.move);
			if (ok) {
				// 捕獲の整合
				PieceType capPt = NO_PIECE_TYPE;
				if (!ev.move.is_drop()) {
					Piece cap = p->pos.piece_on(ev.move.to_sq());
					if (cap != NO_PIECE)
						capPt = type_of(cap);
				}
				ok = capPt == ev.capRole;
			}
			// 王手宣言の整合
			if (ok && ev.check != CheckAfter::Pending) {
				bool gc = p->pos.gives_check(ev.move);
				ok = gc == (ev.check == CheckAfter::Yes);
			}
			if (!ok) {
				bury(*p);
				continue;
			}
			p->advance(ev.move);
			alive.push_back(std::move(p));
		}
		parts_ = std::move(alive);
		break;
	}

	case EvKind::OurFoul: {
		// その手が合法だった粒子は真の局面と矛盾している
		std::vector<ParticlePtr> alive;
		for (auto& p : parts_) {
			if (!p->legal(ev.move)) {
				alive.push_back(std::move(p));
			} else {
				bury(*p);
			}
		}
		parts_ = std::move(alive);
		break;
	}

	case EvKind::OppFoul:
		// 相手の反則は真の局面に制約を与えない
		break;

	case EvKind::OppMove: {
		// 1) 各粒子の整合手を求める(空なら死)
		struct Pending {
			ParticlePtr        p;
			std::vector<Move>  moves;
			Move               chosen;
		};
		std::vector<Pending> pend;
		std::vector<Move>    buf;
		for (auto& p : parts_) {
			consistent_opp_moves(*p, ev, buf, /*relax*/ 0);
			if (buf.empty()) {
				bury(*p);
				continue;
			}
			Pending pd;
			pd.p     = std::move(p);
			pd.moves = buf;
			pend.push_back(std::move(pd));
		}

		// 2) 各親の着手をサンプリング
		for (auto& pd : pend)
			pd.chosen = sample_policy(*pd.p, pd.moves, {});

		// 3) 人口が目標を下回るぶんは、別の整合手で親を複製して補う
		std::vector<ParticlePtr> next;
		size_t want = size_t(cfg_.particles);
		size_t budget = want > pend.size() ? want - pend.size() : 0;
		// 複製はリプレイを伴うので1イベントあたりの上限を設ける
		budget = std::min(budget, size_t(cfg_.particles) / 2);
		size_t idx = 0;
		while (budget > 0 && !pend.empty()) {
			Pending& pd = pend[idx % pend.size()];
			if (!pd.p->synthetic && pd.moves.size() >= 2) {
				std::vector<Move> excl = {pd.chosen};
				Move alt = sample_policy(*pd.p, pd.moves, excl);
				if (alt != Move::none()) {
					auto child = clone_of(hist, *pd.p);
					child->oppMoves.push_back(alt);
					child->advance(alt);
					next.push_back(std::move(child));
					--budget;
				} else {
					// 代替手がない親はスキップ
				}
			}
			++idx;
			if (idx > pend.size() * 4)  // 代替手が尽きたら打ち切り
				break;
		}

		// 4) 親を進める
		for (auto& pd : pend) {
			pd.p->oppMoves.push_back(pd.chosen);
			pd.p->advance(pd.chosen);
			next.push_back(std::move(pd.p));
		}
		parts_ = std::move(next);
		break;
	}
	}
}

namespace {

// 相手の駒がそのマスにいる事前確率(相対重み)。
//
// 従来は「空きマスから一様ランダム」だったので、合成粒子は歩が敵陣最奥にいるような
// ありえない配置を量産していた。相手の駒は平手初期配置から普通に進むだけなので、
// 相手から見た段(relative_rank)と筋で素朴な事前分布を置くだけで大きく当たる。
// rr は相手から見た段(0 = 相手の最奥 = 相手の玉が初期にいる段)、
// rf は相手から見た筋(0起点で 1 = 飛車の初期筋、7 = 角の初期筋)。
// 呼び出し側は rr に relative_rank(us, ...) を渡すこと(相手の色を渡すと段が反転する)。
int placement_weight(PieceType raw, int rr, int rf) {
	static const int PAWN_W  [9] = { 2,  5, 40, 26, 16,  9,  5,  3,  2};
	static const int LANCE_W [9] = {45,  8,  8,  8,  6,  5,  4,  3,  3};
	static const int KNIGHT_W[9] = {40,  6, 10, 12,  8,  6,  4,  3,  3};
	static const int SILVER_W[9] = {30, 22, 16, 10,  7,  5,  4,  3,  3};
	static const int GOLD_W  [9] = {35, 25, 14,  8,  5,  4,  3,  2,  2};
	static const int BISHOP_W[9] = { 6, 30, 12, 10,  8,  8,  8,  6,  6};
	static const int ROOK_W  [9] = { 6, 32, 12, 10,  8,  8,  8,  6,  6};
	static const int KING_W  [9] = {40, 25, 12,  8,  5,  4,  3,  2,  1};

	switch (raw) {
	case PAWN:   return PAWN_W[rr];
	case LANCE:  return LANCE_W[rr]  * ((rf == 0 || rf == 8) ? 4 : 1);
	case KNIGHT: return KNIGHT_W[rr] * ((rf == 1 || rf == 7) ? 3 : 1);
	case SILVER: return SILVER_W[rr] * ((rf == 2 || rf == 6) ? 2 : 1);
	case GOLD:   return GOLD_W[rr]   * ((rf == 3 || rf == 5) ? 2 : 1);
	case BISHOP: return BISHOP_W[rr] * (rf == 7 ? 3 : 1);
	case ROOK:   return ROOK_W[rr]   * (rf == 1 ? 3 : 1);
	case KING:   return KING_W[rr]   * ((rf == 0 || rf == 8) ? 1 : 3);
	default:     return 8;
	}
}

// 演繹による重み係数: 「相手が指していないぶんだけ、そのマスは空いたまま」。
// stale==0 は論理的に確実に空きなので重み0(=置かない)。
double stale_factor(int st) {
	static const double F[6] = {0.0, 0.25, 0.50, 0.70, 0.85, 0.95};
	return st < 0 ? 1.0 : st < 6 ? F[st] : 1.0;
}

} // namespace

// 合成粒子の生成。観測から一意に決まる駒勘定:
//   相手の持ち駒 = 自分が失った駒(初期20枚 - 盤上の自駒)
//   相手の盤上駒 = 相手の初期20枚 - 自分の持ち駒
// を満たす配置を、上の事前分布と演繹(確実な空きマス・確実に相手駒がいるマス)に
// 従ってサンプリングし、王手状態と「この手番で反則になった手」の整合を確認する。
ParticlePtr Belief::synthesize(const OwnView& view) {
	const Color us  = view.us;
	const Color opp = ~us;

	// 駒勘定(rawロール)。index = PieceType(PAWN..GOLD)
	// 保存則: 相手側の駒総数 = 全体 - 自分の盤上 - 自分の持ち駒。
	// 盤上/持ち駒の分割は観測できない(相手の打ちは見えない)ので、
	// 「取られた駒の累計」を上限として持ち駒側をサンプリングする。
	static const int totalCount[8] = {0, 18, 4, 4, 4, 2, 2, 4};  // -,P,L,N,S,B,R,G
	int oppTotal[8];
	int oppBoard[8];
	int oppHand[8];
	int ourOnBoard[8] = {};
	for (auto sq : SQ) {
		Piece pc = view.board[sq];
		if (pc != NO_PIECE && type_of(pc) != KING)
			ourOnBoard[pc & 7]++;
	}

	// 空きマス(自駒のないマス)
	std::vector<Square> empties;
	for (auto sq : SQ)
		if (view.board[sq] == NO_PIECE)
			empties.push_back(sq);

	// 演繹: 相手が最後に自駒を取ったマスには、いま確実に相手の駒がいる
	// (それ以降に動いたのは自分の駒だけで、取り返していれば自駒が乗っている)。
	Square forcedSq = SQ_NB;
	if (cfg_.deduce && view.lastOppCaptureSq != SQ_NB
	    && view.board[view.lastOppCaptureSq] == NO_PIECE)
		forcedSq = view.lastOppCaptureSq;

	// 演繹: この手番で反則になった手から「相手駒がいるマス」を割り出す。
	// 合成粒子は最後に curFouls_ との整合を棄却検査するが、ランダム配置が
	// 偶然そこを埋める確率は低いので、能動的に置いて採択率を上げる。
	//
	//   打ちの反則 … 打ちは自駒を動かさないので自玉を晒すことはない。王手中でなく
	//     歩打ち(打ち歩詰めの可能性)でもなければ、着地マスが埋まっている以外に
	//     反則の理由がない ⇒ そのマスに相手駒がいる(確実)
	//   スライダー移動の反則 … 経路のどこかが埋まっている(ピンの可能性もあるので
	//     確実ではないが、経路封鎖のほうが圧倒的に多い)。経路から1マス選んで置く
	std::vector<Square> forcedExtra;
	if (cfg_.deduce && !view.inCheckNow) {
		for (Move fm : curFouls_) {
			Square sq = SQ_NB;
			if (fm.is_drop()) {
				if (fm.move_dropped_piece() == PAWN)
					continue;  // 打ち歩詰めなら着地マスは空でも反則になる
				sq = fm.to_sq();
			} else {
				// 経路(両端を除く)からランダムに1マス
				std::vector<Square> path;
				Square from = fm.from_sq(), to = fm.to_sq();
				int df = int(file_of(to)) - int(file_of(from));
				int dr = int(rank_of(to)) - int(rank_of(from));
				int steps = std::max(std::abs(df), std::abs(dr));
				if (steps >= 2 && (df == 0 || dr == 0 || std::abs(df) == std::abs(dr))) {
					int sf = (df > 0) - (df < 0), sr = (dr > 0) - (dr < 0);
					for (int k = 1; k < steps; ++k) {
						Square t = File(int(file_of(from)) + sf * k) | Rank(int(rank_of(from)) + sr * k);
						if (view.board[t] == NO_PIECE)
							path.push_back(t);
					}
				}
				if (!path.empty() && (rng_.rand<uint64_t>() % 100) < 65)
					sq = path[rng_.rand<uint64_t>() % path.size()];
			}
			if (sq == SQ_NB || view.board[sq] != NO_PIECE || sq == forcedSq)
				continue;
			if (std::find(forcedExtra.begin(), forcedExtra.end(), sq) == forcedExtra.end())
				forcedExtra.push_back(sq);
		}
	}

	auto in_our_camp = [&](Square sq) {
		Rank r = rank_of(sq);
		return us == BLACK ? r >= RANK_7 : r <= RANK_3;
	};
	// 相手陣(相手から見た1〜3段目) = synthPrior=0 のときの玉のバイアス用
	auto in_opp_camp = [&](Square sq) {
		return relative_rank(us, rank_of(sq)) <= RANK_3;
	};
	auto rel_file = [&](Square sq) {
		int f = int(file_of(sq));
		return opp == BLACK ? f : 8 - f;
	};

	for (int attempt = 0; attempt < 30; ++attempt) {
		// 後半の試行では演繹の重みを緩める(駒勘定の推定ずれで詰むのを防ぐ)
		const bool strictStale = attempt < 20;

		for (int pt = PAWN; pt <= GOLD; ++pt) {
			oppTotal[pt] = totalCount[pt] - ourOnBoard[pt] - hand_count(view.hand, PieceType(pt));
			if (oppTotal[pt] < 0)
				return nullptr;  // 駒勘定が壊れている(観測処理のバグ)
			int capturedAvail = std::min(view.oppCaptured[pt], oppTotal[pt]);
			int h = 0;
			for (int c = 0; c < capturedAvail; ++c)
				if ((rng_.rand<uint64_t>() % 100) < 70)  // 取られた駒は7割がまだ持ち駒と仮定
					++h;
			oppHand[pt]  = h;
			oppBoard[pt] = oppTotal[pt] - h;
		}

		Piece board[SQ_NB];
		for (auto sq : SQ)
			board[sq] = view.board[sq];
		bool pawnFile[FILE_NB] = {};
		bool ok = true;

		// 事前分布に従って空きマスを1つ選び、駒を置く。
		// forced が指定されていればそのマスに固定する(演繹による強制配置)。
		auto place = [&](PieceType raw, Square forced) -> bool {
			// 不成/成りの候補ごとに重みを積む
			double     total = 0;
			double     wbuf[81];
			size_t     n = empties.size();
			for (size_t k = 0; k < n; ++k) {
				Square sq = empties[k];
				double w  = 0;
				if (board[sq] == NO_PIECE && (forced == SQ_NB || sq == forced)) {
					if (cfg_.synthPrior) {
						// 0 = 相手の最奥。relative_rank は「引数の色の最奥が8」なので、
						// 相手から見た段は relative_rank(us, ...) のほうであることに注意
						int rr = int(relative_rank(us, rank_of(sq)));
						w = double(placement_weight(raw, rr, rel_file(sq)));
					} else {
						// 初版と同じ一様配置。玉だけは相手陣寄りに引く
						// (初版は「相手陣でなければ70%で捨てる」= およそ3倍のバイアス)。
						w = (raw == KING && in_opp_camp(sq)) ? 3.3 : 1.0;
					}
					// 演繹による減衰(強制配置のマスは演繹そのものなので掛けない)
					if (cfg_.deduce && strictStale && forced == SQ_NB)
						w *= stale_factor(view.stale(sq));
					// 二歩になる筋は「と金なら置ける」ので、重み0にはしない。
					// 0にすると後段の「二歩なら成りに倒す」が死にコードになり、
					// 相手の生歩が空き筋より多いとき(と金が要るとき)に
					// place(PAWN) が必ず失敗して合成粒子が作れなくなる。
					if (raw == PAWN && pawnFile[file_of(sq)])
						w *= 0.15;
				}
				wbuf[k] = w;
				total += w;
			}
			if (total <= 0)
				return false;
			double r    = double(rng_.rand<uint64_t>() >> 11) / double(1ull << 53) * total;
			size_t pick = SIZE_MAX;
			double acc  = 0;
			for (size_t k = 0; k < n; ++k) {
				if (wbuf[k] <= 0)
					continue;
				pick = k;              // 最後に見た正の重み(丸め誤差時のフォールバック)
				acc += wbuf[k];
				if (r <= acc)
					break;
			}
			if (pick == SIZE_MAX)
				return false;
			Square sq = empties[pick];

			// 成りの決定(相手から見た敵陣=自陣側にあるなら成りやすい)
			bool promoted = false;
			if (raw != KING && raw != GOLD) {
				int pr = in_our_camp(sq) ? 25 : 2;
				promoted = (rng_.rand<uint64_t>() % 100) < uint64_t(pr);
				// 不成では存在できないマス(行き所のない駒)は強制成り
				if (!promoted && !piece_can_stay(opp, raw, sq))
					promoted = raw == PAWN || raw == LANCE || raw == KNIGHT;
				if (!promoted && raw == PAWN && pawnFile[file_of(sq)])
					promoted = true;
			}
			if (!promoted && !piece_can_stay(opp, raw, sq))
				return false;
			board[sq] = make_piece(opp, promoted ? PieceType(raw | 8) : raw);
			if (!promoted && raw == PAWN)
				pawnFile[file_of(sq)] = true;
			return true;
		};

		// 王手を受けているときは、王手をかけている駒を最後に明示的に置く
		// (ランダム配置が偶然王手になる確率は低すぎる)。
		PieceType checkerRaw = NO_PIECE_TYPE;
		if (view.inCheckNow) {
			std::vector<PieceType> avail;
			for (int pt = PAWN; pt <= GOLD; ++pt)
				if (oppBoard[pt] > 0)
					avail.push_back(PieceType(pt));
			if (avail.empty())
				continue;
			checkerRaw = avail[rng_.rand<uint64_t>() % avail.size()];
		}

		// 演繹による強制配置(相手が最後に取ったマス)。
		// 王手中なら王手駒がそこにいる可能性が高いので、そちらの配置に任せる。
		PieceType forcedRaw = NO_PIECE_TYPE;
		if (forcedSq != SQ_NB && !(view.inCheckNow)) {
			std::vector<PieceType> avail;
			for (int pt = PAWN; pt <= GOLD; ++pt)
				if (oppBoard[pt] > 0)
					avail.push_back(PieceType(pt));
			if (!avail.empty())
				forcedRaw = avail[rng_.rand<uint64_t>() % avail.size()];
		}

		// 演繹で決まっているマスを先に埋める。玉を先に置くと、玉がたまたま
		// 強制配置のマスに乗ってしまい、そのあとの place(forced) が必ず失敗して
		// 30試行のうち1枠を丸ごと捨てることになる(玉の置き場所は60マス以上あるので、
		// 先に強制配置を埋めても玉が置けなくなることはまずない)。
		if (forcedRaw != NO_PIECE_TYPE) {
			if (!place(forcedRaw, forcedSq))
				continue;
			oppBoard[forcedRaw]--;
		}
		// 反則から割り出したマスを埋める(駒種は盤上に残っているものからランダム)
		for (Square fs : forcedExtra) {
			std::vector<PieceType> avail;
			for (int pt = PAWN; pt <= GOLD; ++pt)
				if (oppBoard[pt] - (checkerRaw == pt ? 1 : 0) > 0)
					avail.push_back(PieceType(pt));
			if (avail.empty())
				break;
			PieceType raw = avail[rng_.rand<uint64_t>() % avail.size()];
			if (!place(raw, fs)) {
				ok = false;
				break;
			}
			oppBoard[raw]--;
		}
		if (!ok)
			continue;
		if (!place(KING, SQ_NB))
			continue;
		for (int pt = PAWN; pt <= GOLD && ok; ++pt) {
			int cnt = oppBoard[pt] - (checkerRaw == pt ? 1 : 0);
			for (int c = 0; c < cnt && ok; ++c)
				ok = place(PieceType(pt), SQ_NB);
		}
		if (!ok)
			continue;

		if (checkerRaw != NO_PIECE_TYPE) {
			// 自玉に利く空きマスへ王手駒を置く(成り/不成の両方を試す)。
			// 「駒種tの駒がsqから玉に利く ⟺ sqは玉位置からの自分側の同駒種の利き内」
			// という対称性を使う。スライダーは配置済みの駒を遮蔽として考慮する。
			Square ksq = view.king_square();
			Bitboard occ = Bitboard(ZERO);
			for (auto sq : SQ)
				if (board[sq] != NO_PIECE)
					occ = occ | sq;
			bool placed = false;

			// 直前に自駒が取られたマスがあるなら、王手駒はそこにいる可能性が高い
			// (取ったその駒が王手をかけている)。まずそのマスへの配置を試す。
			Square hint = view.lastOppCaptureSq;
			if (hint != SQ_NB && board[hint] == NO_PIECE
			    && (rng_.rand<uint64_t>() % 100) < 85) {
				for (int promo = 1; promo >= 0 && !placed; --promo) {  // 成り優先(と金攻めが典型)
					if (checkerRaw == GOLD && promo)
						continue;
					PieceType pt = promo ? PieceType(checkerRaw | 8) : checkerRaw;
					if (effects_from(make_piece(opp, pt), hint, occ) & ksq) {
						if (!promo && checkerRaw == PAWN
						    && (pawnFile[file_of(hint)] || !piece_can_stay(opp, PAWN, hint)))
							continue;
						if (!promo && (checkerRaw == LANCE || checkerRaw == KNIGHT)
						    && !piece_can_stay(opp, checkerRaw, hint))
							continue;
						board[hint] = make_piece(opp, pt);
						placed = true;
					}
				}
			}

			for (int promo = 0; promo < 2 && !placed; ++promo) {
				if (checkerRaw == GOLD && promo)
					break;
				PieceType pt = promo ? PieceType(checkerRaw | 8) : checkerRaw;
				Bitboard cand = effects_from(make_piece(us, pt), ksq, occ);
				while (cand) {
					Square sq = cand.pop();
					if (board[sq] != NO_PIECE)
						continue;
					if (!promo && checkerRaw == PAWN
					    && (pawnFile[file_of(sq)] || !piece_can_stay(opp, PAWN, sq)))
						continue;
					if (!promo && (checkerRaw == LANCE || checkerRaw == KNIGHT)
					    && !piece_can_stay(opp, checkerRaw, sq))
						continue;
					board[sq] = make_piece(opp, pt);
					placed = true;
					break;
				}
			}
			if (!placed)
				continue;
		}

		// SFEN組み立て
		std::string sfen;
		for (Rank r = RANK_1; r <= RANK_9; ++r) {
			int run = 0;
			for (File f = FILE_9; f >= FILE_1; --f) {
				Piece pc = board[f | r];
				if (pc == NO_PIECE) {
					++run;
					continue;
				}
				if (run) { sfen += std::to_string(run); run = 0; }
				static const char* letters = " PLNSBRGK";
				PieceType raw = PieceType(pc & 7);
				char ch = type_of(pc) == KING ? 'K' : letters[raw];
				if (type_of(pc) != KING && (pc & PIECE_PROMOTE))
					sfen += '+';
				sfen += color_of(pc) == BLACK ? ch : char(ch - 'A' + 'a');
			}
			if (run)
				sfen += std::to_string(run);
			if (r != RANK_9)
				sfen += '/';
		}
		sfen += us == BLACK ? " b " : " w ";
		std::string hands;
		auto emit_hand = [&](Color c, int pt, int cnt) {
			if (cnt <= 0)
				return;
			static const char* letters = " PLNSBRG";
			if (cnt > 1)
				hands += std::to_string(cnt);
			char ch = letters[pt];
			hands += c == BLACK ? ch : char(ch - 'A' + 'a');
		};
		for (int pt = PAWN; pt <= GOLD; ++pt)
			emit_hand(us, pt, hand_count(view.hand, PieceType(pt)));
		for (int pt = PAWN; pt <= GOLD; ++pt)
			emit_hand(opp, pt, oppHand[pt]);
		sfen += hands.empty() ? "-" : hands;
		sfen += " 1";

		auto p = std::make_unique<Particle>();
		if (!p->init_from_sfen(sfen))
			continue;
		// 王手状態の整合: 手番(自分)の玉の王手有無が観測と一致すること
		if (p->pos.in_check() != view.inCheckNow)
			continue;
		// 相手玉に自分の利きが当たっていたら、相手の直前の手が不正だったことになる
		Square oppKing = p->pos.square<KING>(opp);
		if (p->pos.attackers_to(us, oppKing))
			continue;
		// 演繹: この手番ですでに反則になった手は、真の局面では必ず不正。
		// 合成粒子でそれが合法になっているなら、その配置は真の局面ではありえない。
		bool contradicted = false;
		for (Move fm : curFouls_)
			if (p->legal(fm)) { contradicted = true; break; }
		if (contradicted)
			continue;
		return p;
	}
	return nullptr;
}

void Belief::force_resynthesize(const OwnView& view, TimePoint deadline) {
	parts_.clear();
	int misses = 0;
	size_t target = std::max<size_t>(16, size_t(cfg_.particles) / 4);
	// synthesize は1回あたり最大30試行回るので、回数だけでなく時計でも止める
	// (sync が予算を使ったあとに呼ばれるため、ここで時間切れを起こしうる)。
	while (parts_.size() < target && misses < 400) {
		// 締め切りを過ぎていても、1粒子も作れていないうちは少しだけ粘る
		// (空の信念で戻ると呼び出し側が手を選べなくなる)。ただし無制限にはしない
		// ―― 粘る回数を絞らないと 400ミス×30試行ぶん予算を食い潰しうる。
		if (now() >= deadline && (!parts_.empty() || misses >= 40))
			break;
		auto p = synthesize(view);
		if (p)
			parts_.push_back(std::move(p));
		else
			++misses;
	}
	relaxLevel_ = 3;
}

ParticlePtr Belief::replay_one(const GameHistory& hist, int relax,
                               const std::vector<Move>* seed, size_t resample,
                               size_t* failIdx) {
	auto p = std::make_unique<Particle>();
	std::vector<Move> buf;
	// 種の先頭 reuse 手はそのまま使う(整合チェックは行うので安全)
	const size_t reuse = seed && seed->size() > resample ? seed->size() - resample : 0;
	for (size_t i = 0; i < cursor_; ++i) {
		if (failIdx)
			*failIdx = i;
		const HistEvent& ev = hist.events[i];
		switch (ev.kind) {
		case EvKind::OurMove: {
			if (!p->legal(ev.move))
				return nullptr;
			PieceType capPt = NO_PIECE_TYPE;
			if (!ev.move.is_drop()) {
				Piece cap = p->pos.piece_on(ev.move.to_sq());
				if (cap != NO_PIECE)
					capPt = type_of(cap);
			}
			if (capPt != ev.capRole) {
				// 緩和時は成り状態の不一致を許す(歩⇔と金など)。
				// 取った後の局面には成り状態は影響しないので、ほぼ無損失な緩和。
				bool rawMatch = capPt != NO_PIECE_TYPE && ev.capRole != NO_PIECE_TYPE
				                && PieceType(capPt & 7) == PieceType(ev.capRole & 7);
				if (!(relax >= 1 && rawMatch))
					return nullptr;
			}
			if (ev.check != CheckAfter::Pending) {
				bool gc = p->pos.gives_check(ev.move);
				bool want = ev.check == CheckAfter::Yes;
				if (gc != want) {
					// 自分の手の王手宣言も緩和レベルに従う
					if (want ? relax < 2 : relax < 1)
						return nullptr;
				}
			}
			p->advance(ev.move);
			break;
		}
		case EvKind::OurFoul:
			if (relax < 1 && p->legal(ev.move))
				return nullptr;
			break;
		case EvKind::OppFoul:
			break;
		case EvKind::OppMove: {
			Move m = Move::none();
			if (p->oppMoves.size() < reuse) {
				// 種の再利用: その手の整合だけ直接確かめる(全合法手の列挙は不要)
				Move sm = (*seed)[p->oppMoves.size()];
				if (opp_move_consistent(*p, ev, sm, relax))
					m = sm;
			} else {
				consistent_opp_moves(*p, ev, buf, relax);
				if (!buf.empty()) {
					// oppPolicy=1 の非千里眼priorは評価関数を呼ばないので、
					// 回数の多いリプレイでもフィルタと同じ方策が使える。
					// (従来は一様サンプリングで、フィルタとリプレイで相手モデルが
					//  食い違っていた ＝ 再生成した粒子だけ相手の指し手がでたらめ)
					m = cfg_.oppPolicy != 0 ? sample_policy(*p, buf, {})
					                        : buf[rng_.rand<uint64_t>() % buf.size()];
				}
			}
			if (m == Move::none())
				return nullptr;
			p->oppMoves.push_back(m);
			p->advance(m);
			break;
		}
		}
	}
	return p;
}

void Belief::sync(const GameHistory& hist, const OwnView& view, TimePoint deadline) {
	// この手番でこれまでに反則になった自分の手を拾う(履歴末尾の OurFoul の連なり)。
	// 「現局面でこの手は不正」は演繹的に確実な制約なので、合成粒子の棄却に使う。
	curFouls_.clear();
	if (cfg_.deduce)
		for (auto it = hist.events.rbegin(); it != hist.events.rend(); ++it) {
			if (it->kind == EvKind::OurFoul)
				curFouls_.push_back(it->move);
			else if (it->kind == EvKind::OurMove || it->kind == EvKind::OppMove)
				break;
		}

	// 王手宣言が確定しているイベントまで適用
	while (cursor_ < hist.events.size()) {
		const HistEvent& ev = hist.events[cursor_];
		bool isMove = ev.kind == EvKind::OurMove || ev.kind == EvKind::OppMove;
		if (isMove && ev.check == CheckAfter::Pending)
			break;
		apply_event(hist, ev);
		++cursor_;
	}

	// 再生成(枯渇・不足時)。
	// 緩和(relax>0)した粒子は観測と部分的に矛盾していて合法率の推定を汚すので、
	// 目標数(want)へは厳密粒子のみで向かい、緩和は「最低限の粒子数(hardMin)の
	// 確保」のためだけに使う。
	size_t want    = size_t(cfg_.particles);
	size_t hardMin = std::max<size_t>(8, want / 16);
	if (parts_.size() >= want * size_t(cfg_.regenFloorPct) / 100)
		return;

	// 時間配分: 厳密(relax=0)に50%、relax=1に25%、relax=2に残り。
	// 失敗回数でも時間でも先に尽きたほうでエスカレーションする。
	// 各レベル内では「死んだ粒子の部分若返り(直近R手だけ再サンプリング)」を
	// R=2,4,8,16 の順で優先し、最後にゼロからの全リプレイを試す。
	// 全滅の原因(反則・捕獲不一致・整合手なし)は直近の相手手にしか依存しない
	// ことが多く、若返りは全リプレイよりはるかに成功率が高い。
	TimePoint t0    = now();
	TimePoint total = std::max<TimePoint>(1, deadline - t0);
	// 末尾の20%は合成粒子(最終フォールバック)のために予約する
	TimePoint ladderEnd = t0 + total * 4 / 5;
	TimePoint gate[3] = { t0 + total * 2 / 5, t0 + total * 3 / 5, ladderEnd };

	int relax = 0;
	int fails = 0;
	int tries = 0;
	while (tries < cfg_.regenTries) {
		size_t target = relax == 0 ? want : hardMin;
		if (parts_.size() >= target)
			break;
		bool exhausted = now() >= gate[relax] || fails >= 400;
		if (exhausted) {
			// 厳密粒子が最低限あるなら緩和はしない(緩和粒子は推定を汚す)
			if (parts_.size() >= hardMin || relax >= 2)
				break;
			++relax;
			fails = 0;
			continue;
		}
		++tries;
		ParticlePtr p;
		size_t failIdx = 0;
		// 種の質 = 長さ(どこまで観測と整合して生きたか)。長い種を優先する。
		size_t maxLen = 0;
		for (const auto& s : graveyard_)
			maxLen = std::max(maxLen, s.size());
		const std::vector<Move>* seed = nullptr;
		if (!graveyard_.empty() && (tries % 13) != 0) {
			// 最長クラスの種が引けるまで数回引き直す
			for (int k = 0; k < 8; ++k) {
				const auto& s = graveyard_[rng_.rand<uint64_t>() % graveyard_.size()];
				if (s.size() + 2 >= maxLen) { seed = &s; break; }
			}
		}
		if (seed) {
			static const size_t RS[4] = {2, 4, 8, 16};
			size_t r = RS[std::min<size_t>(3, fails / 25)];  // 失敗が続くほど幅を広げる(戻さない)
			p = replay_one(hist, relax, seed, r, &failIdx);
		} else {
			p = replay_one(hist, relax, nullptr, 0, &failIdx);
		}
		if (p) {
			parts_.push_back(std::move(p));
			fails = 0;
		} else {
			++fails;
			failHist_[std::min<size_t>(failIdx < cursor_ ? cursor_ - 1 - failIdx : 0, 15)]++;
			failKind_[size_t(hist.events[failIdx].kind)]++;
		}
	}
	relaxLevel_ = relax;

	// 最終フォールバック: 合成粒子(常に成功する)。
	// リプレイでは再現できない稀な相手の指し回しに遭遇したときの保険で、
	// 「駒勘定と王手状態だけ合う配置」でも 0粒子(当てずっぽう)よりはるかにまし。
	if (parts_.size() < hardMin) {
		size_t synthTarget = std::max(hardMin, want / 4);
		int    misses      = 0;
		while (parts_.size() < synthTarget && misses < 200 && now() < deadline + 100) {
			auto p = synthesize(view);
			if (p)
				parts_.push_back(std::move(p));
			else
				++misses;
		}
		if (!parts_.empty())
			relaxLevel_ = 3;
	}

	if (parts_.empty() && cfg_.logLevel >= 1) {
		std::string h = "info string regen_fail cursor=" + std::to_string(cursor_) + " dist:";
		for (int k = 0; k < 16; ++k)
			h += " " + std::to_string(failHist_[k]);
		h += " kind(OurMove,OurFoul,OppMove,OppFoul):";
		for (int k = 0; k < 4; ++k)
			h += " " + std::to_string(failKind_[k]);
		sync_cout << h << sync_endl;
	}
	std::fill(std::begin(failHist_), std::end(failHist_), 0);
	std::fill(std::begin(failKind_), std::end(failKind_), 0);
}

} // namespace Tsuitate
} // namespace YaneuraOu

#endif // TSUITATE_ENGINE
