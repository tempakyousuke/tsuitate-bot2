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

// 方策 = softmax(着手後の静的評価 / 温度) + ε一様。excludeの手は候補から外す。
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

	std::vector<double> score(cand.size());
	double mx = -1e18;
	for (size_t i = 0; i < cand.size(); ++i) {
		StateInfo st;
		p.pos.do_move(cand[i], st);
		// 着手後は手番が自分側に戻るので、相手から見た評価は符号反転
		score[i] = -double(Eval::evaluate(p.pos));
		p.pos.undo_move(cand[i]);
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

// 合成粒子の生成。観測から一意に決まる駒勘定:
//   相手の持ち駒 = 自分が失った駒(初期20枚 - 盤上の自駒)
//   相手の盤上駒 = 相手の初期20枚 - 自分の持ち駒
// を満たすランダム配置を作り、王手状態の整合を確認する。
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

	// 空きマス(自駒のないマス)
	std::vector<Square> empties;
	for (auto sq : SQ)
		if (view.board[sq] == NO_PIECE)
			empties.push_back(sq);

	const Rank oppBack[3] = {opp == WHITE ? RANK_1 : RANK_9,
	                         opp == WHITE ? RANK_2 : RANK_8,
	                         opp == WHITE ? RANK_3 : RANK_7};
	auto in_opp_camp = [&](Square sq) {
		Rank r = rank_of(sq);
		return r == oppBack[0] || r == oppBack[1] || r == oppBack[2];
	};
	auto in_our_camp = [&](Square sq) {
		Rank r = rank_of(sq);
		return us == BLACK ? r >= RANK_7 : r <= RANK_3;
	};

	for (int attempt = 0; attempt < 30; ++attempt) {
		// 配置(シャッフルして先頭から使う)
		std::vector<Square> pool = empties;
		for (size_t i = pool.size(); i > 1; --i)
			std::swap(pool[i - 1], pool[rng_.rand<uint64_t>() % i]);

		Piece board[SQ_NB];
		for (auto sq : SQ)
			board[sq] = view.board[sq];
		bool pawnFile[FILE_NB] = {};
		size_t poolIdx = 0;
		bool ok = true;

		auto place = [&](PieceType raw) -> bool {
			// 玉は相手陣を優先
			for (size_t k = poolIdx; k < pool.size(); ++k) {
				Square sq = pool[k];
				// 玉の相手陣バイアス: 序盤の玉は動いていないことが多い
				if (raw == KING && !in_opp_camp(sq)
				    && (rng_.rand<uint64_t>() % 100) < 70)
					continue;
				// 成りの決定(相手から見た敵陣=自陣側にあるなら成りやすい)
				bool promoted = false;
				if (raw != KING && raw != GOLD) {
					int pr = in_our_camp(sq) ? 25 : 2;
					promoted = (rng_.rand<uint64_t>() % 100) < uint64_t(pr);
				}
				if (!promoted && raw == PAWN) {
					if (pawnFile[file_of(sq)])
						continue;
					if (!piece_can_stay(opp, PAWN, sq))
						continue;
				}
				if (!promoted && raw == LANCE && !piece_can_stay(opp, LANCE, sq))
					continue;
				if (!promoted && raw == KNIGHT && !piece_can_stay(opp, KNIGHT, sq))
					continue;
				Piece pc = make_piece(opp, promoted ? PieceType(raw | 8) : raw);
				board[sq] = pc;
				if (!promoted && raw == PAWN)
					pawnFile[file_of(sq)] = true;
				std::swap(pool[k], pool[poolIdx]);
				++poolIdx;
				return true;
			}
			return false;
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

		if (!place(KING))
			continue;
		for (int pt = PAWN; pt <= GOLD && ok; ++pt) {
			int cnt = oppBoard[pt] - (checkerRaw == pt ? 1 : 0);
			for (int c = 0; c < cnt && ok; ++c)
				ok = place(PieceType(pt));
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
		return p;
	}
	return nullptr;
}

void Belief::force_resynthesize(const OwnView& view) {
	parts_.clear();
	int misses = 0;
	size_t target = std::max<size_t>(16, size_t(cfg_.particles) / 4);
	while (parts_.size() < target && misses < 400) {
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
				// 種の再利用(整合だけ確認する)
				Move sm = (*seed)[p->oppMoves.size()];
				consistent_opp_moves(*p, ev, buf, relax);
				for (Move c : buf)
					if (c == sm) { m = sm; break; }
			} else {
				consistent_opp_moves(*p, ev, buf, relax);
				if (!buf.empty())
					// リプレイは回数が多いので評価なしの一様サンプリング
					m = buf[rng_.rand<uint64_t>() % buf.size()];
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
	if (parts_.size() >= want / 2)
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
