// ついたて将棋エンジン: 思考部の実装
#include "think.h"
#include "dsearch.h"

#if defined(TSUITATE_ENGINE)

#include <algorithm>
#include <cmath>

#include "../../evaluate.h"

namespace YaneuraOu {
namespace Tsuitate {

namespace {

// 粒子が1つも作れなかったときの保険: 自駒のみのヒューリスティック。
// 信念がない状態では反則リスクを直接推定できないので、
// 「見えない駒に当たりにくい安全な手」(短い移動・打たない)を優先する。
double fallback_score(const OwnView& view, Move m, PRNG& rng) {
	double s = double(rng.rand<uint64_t>() % 1000) / 500.0;
	if (m.is_drop())
		return s - 6.0;  // 打ちマスの占有は見えない(反則リスク大)
	Square from = m.from_sq(), to = m.to_sq();
	int adv = view.us == BLACK ? int(rank_of(from)) - int(rank_of(to))
	                           : int(rank_of(to)) - int(rank_of(from));
	s += adv * 0.5;
	if (m.is_promote())
		s += 2;
	if (type_of(view.board[from]) == KING)
		s -= 2;
	int d = dist(from, to);
	if (d > 1)
		s -= 2.0 * (d - 1);  // 長い移動ほど経路が相手駒に当たりやすい
	return s;
}

} // namespace

ThinkResult Thinker::think(const OwnView& view, Belief& belief, const GameHistory& hist,
                           const std::vector<Move>& foulTried, int budgetMs,
                           const Config& cfg, PRNG& rng) {
	const TimePoint t0       = now();
	const TimePoint deadline = t0 + budgetMs;
	ThinkResult res;

	// 1) 信念の同期(再生成には予算の40%まで使う)
	belief.sync(hist, view, t0 + budgetMs * 2 / 5);
	res.nParticles = belief.size();
	res.relaxLevel = belief.relaxLevel();

	// 2) 候補手(この手番で反則になった手は除外)
	std::vector<Move> cands;
	for (Move m : generate_candidates(view))
		if (std::find(foulTried.begin(), foulTried.end(), m) == foulTried.end())
			cands.push_back(m);
	if (cands.empty())
		return res;  // 投了

	const auto& parts = belief.particles();
	const size_t N    = parts.size();

	// 粒子ゼロ: ヒューリスティックで指す(それでも投了よりまし)
	if (N == 0) {
		double best = -1e18;
		for (Move m : cands) {
			double s = fallback_score(view, m, rng);
			if (s > best) { best = s; res.best = m; }
		}
		res.elapsedMs = now() - t0;
		return res;
	}

	// 3) 合法率と「合法な粒子のindex」を全候補について求める
	const size_t M = cands.size();
	std::vector<std::vector<uint32_t>> legalIdx(M);
	for (size_t j = 0; j < N; ++j)
		for (size_t i = 0; i < M; ++i)
			if (parts[j]->legal(cands[i]))
				legalIdx[i].push_back(uint32_t(j));

	auto p_legal = [&](size_t i) { return double(legalIdx[i].size()) / double(N); };

	// 反則コスト(centipawn)。累計10回で反則負けなので、残り予算が減るほど急騰させる。
	double remain = std::max(1.0, 10.0 - view.ourFouls);
	double foulCp = -(cfg.foulBaseCp + cfg.foulStepCp * view.ourFouls) * (10.0 / remain);
	// 信念の品質が悪い(緩和粒子・粒子不足)ときはp_legalの推定が信用できないので、
	// リスクをさらに重く見る。反則→粒子死→さらに反則、のスパイラルを断つ。
	foulCp *= 1.0 + 1.5 * belief.relaxLevel();
	if (N * 4 < size_t(cfg.particles))
		foulCp *= 2.0;

	auto combined = [&](size_t i, double meanCp) {
		double pl = p_legal(i);
		return pl * meanCp + (1.0 - pl) * foulCp;
	};

	// 均等間隔で粒子を選ぶ(粒子は生成順に相関があるため)
	auto pick_particles = [&](const std::vector<uint32_t>& idx, size_t k) {
		std::vector<uint32_t> out;
		if (idx.empty())
			return out;
		k = std::min(k, idx.size());
		for (size_t t = 0; t < k; ++t)
			out.push_back(idx[t * idx.size() / k]);
		return out;
	};

	// 4) stage1: 全候補を静止探索で粗く評価
	std::vector<double> mean1(M, 0.0), comb1(M);
	for (size_t i = 0; i < M; ++i) {
		auto sel = pick_particles(legalIdx[i], size_t(cfg.stage1Samples));
		double sum = 0;
		for (uint32_t j : sel) {
			Position& pos = parts[j]->pos;
			StateInfo st;
			DSearch ds;
			ds.nodesLimit = 20000;
			pos.do_move(cands[i], st);
			Value v = -ds.qsearch(pos, -VALUE_INFINITE, VALUE_INFINITE, 1);
			pos.undo_move(cands[i]);
			sum += squash_cp(v);
		}
		mean1[i] = sel.empty() ? 0.0 : sum / double(sel.size());
		comb1[i] = combined(i, mean1[i]);
	}

	// 5) stage2: 上位候補を深く読む(時間があれば深さを上げる)
	std::vector<size_t> order(M);
	for (size_t i = 0; i < M; ++i)
		order[i] = i;
	std::sort(order.begin(), order.end(),
	          [&](size_t a, size_t b) { return comb1[a] > comb1[b]; });

	size_t T = std::min(size_t(cfg.stage2TopK), M);
	std::vector<size_t> top(order.begin(), order.begin() + T);

	std::vector<double> comb2 = comb1;
	int depthDone = 0;
	for (int d = 2; d <= cfg.searchDepth; d += 2) {
		// この深さのパスを最後まで回す時間がなさそうなら打ち切る
		if (now() > t0 + budgetMs * 3 / 5 && depthDone > 0)
			break;
		std::vector<double> pass(M, 0.0);
		bool aborted = false;
		for (size_t i : top) {
			auto sel = pick_particles(legalIdx[i], size_t(cfg.stage2Samples));
			double sum = 0;
			size_t cnt = 0;
			for (uint32_t j : sel) {
				if (now() > deadline - 50) { aborted = true; break; }
				Position& pos = parts[j]->pos;
				StateInfo st;
				DSearch ds;
				ds.nodesLimit = 60000;
				pos.do_move(cands[i], st);
				Value v = -ds.search(pos, d - 1, -VALUE_INFINITE, VALUE_INFINITE, 1);
				pos.undo_move(cands[i]);
				sum += squash_cp(v);
				++cnt;
			}
			pass[i] = cnt > 0 ? combined(i, sum / double(cnt)) : comb1[i];
			if (aborted)
				break;
		}
		if (!aborted) {
			// パスを完走したときだけ採用する(部分的な値で序列を壊さない)
			for (size_t i : top)
				comb2[i] = pass[i];
			depthDone = d;
		} else {
			break;
		}
	}
	res.depthReached = depthDone;

	// 6) 最終選択(同点付近は乱数タイブレーク)
	double best = -1e18;
	size_t bestI = size_t(-1);
	for (size_t i : top) {
		double noise = double(rng.rand<uint64_t>() % 1000) / 250.0;  // 0〜4cp
		double s = comb2[i] + noise;
		if (s > best) { best = s; bestI = i; }
	}
	if (bestI == size_t(-1))
		bestI = order[0];

	res.best       = cands[bestI];
	res.pLegal     = p_legal(bestI);
	res.expectedCp = comb2[bestI];
	res.elapsedMs  = now() - t0;
	return res;
}

// ---------------------------------------------------------------------------
// BotCore
// ---------------------------------------------------------------------------

void BotCore::new_game(Color us, const Config& cfg) {
	cfg_ = cfg;
	rng_ = PRNG(cfg.seed ^ 0x9e3779b97f4a7c15ull);
	view_.reset(us);
	hist_.clear(us);
	belief_.reset(us, cfg);
	foulTried_.clear();
}

void BotCore::finalize_no_check() {
	hist_.finalize_pending_check(false);
}

void BotCore::on_our_move_accepted(Move m, PieceType capRole) {
	finalize_no_check();
	HistEvent ev;
	ev.kind    = EvKind::OurMove;
	ev.move    = m;
	ev.capRole = capRole;
	hist_.events.push_back(ev);
	view_.apply_our_move(m, capRole);
	view_.inCheckNow = false;  // 受理された = 王手は解消している
	foulTried_.clear();
}

void BotCore::on_our_foul(Move m) {
	finalize_no_check();
	HistEvent ev;
	ev.kind = EvKind::OurFoul;
	ev.move = m;
	ev.check = CheckAfter::No;  // 反則に王手宣言はない
	hist_.events.push_back(ev);
	view_.ourFouls++;
	foulTried_.push_back(m);
}

void BotCore::on_opp_move(Square capSq) {
	finalize_no_check();
	HistEvent ev;
	ev.kind  = EvKind::OppMove;
	ev.capSq = capSq;
	hist_.events.push_back(ev);
	view_.apply_opp_capture(capSq);
	foulTried_.clear();
}

void BotCore::on_opp_foul() {
	finalize_no_check();
	HistEvent ev;
	ev.kind = EvKind::OppFoul;
	ev.check = CheckAfter::No;
	hist_.events.push_back(ev);
	view_.oppFouls++;
}

void BotCore::on_check_declared(bool onUs) {
	hist_.finalize_pending_check(true);
	if (onUs)
		view_.inCheckNow = true;
}

ThinkResult BotCore::think(int budgetMs) {
	finalize_no_check();
	return thinker_.think(view_, belief_, hist_, foulTried_, budgetMs, cfg_, rng_);
}

} // namespace Tsuitate
} // namespace YaneuraOu

#endif // TSUITATE_ENGINE
