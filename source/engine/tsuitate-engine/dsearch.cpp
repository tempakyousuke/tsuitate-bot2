// ついたて将棋エンジン: 確定化局面探索の実装
#include "dsearch.h"

#if defined(TSUITATE_ENGINE)

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "../../movegen.h"
#include "../../evaluate.h"
#include "../../mate/mate.h"

namespace YaneuraOu {
namespace Tsuitate {

namespace {

// 簡易ムーブオーダリング: 捕獲(MVV-LVA)＞成り＞その他
int order_score(const Position& pos, Move m) {
	int s = 0;
	if (!m.is_drop()) {
		Piece cap = pos.piece_on(m.to_sq());
		if (cap != NO_PIECE)
			s += Eval::CapturePieceValue[type_of(cap)] * 8 - Eval::PieceValue[type_of(pos.moved_piece_before(m))];
		if (m.is_promote())
			s += 300;
	}
	return s;
}

} // namespace

Value DSearch::qsearch(Position& pos, Value alpha, Value beta, int ply) {
	++nodes;
	if (ply >= MAX_PLY || nodes > nodesLimit)
		return Eval::evaluate(pos);

	const bool inCheck = pos.in_check();

	Value best;
	if (inCheck) {
		best = mated_in(ply);  // 逃げられなければ詰み
	} else {
		best = Eval::evaluate(pos);  // stand pat
		if (best >= beta)
			return best;
		alpha = std::max(alpha, best);
#if defined(USE_MATE_1PLY)
		Move mm = Mate::mate_1ply(pos);
		if (mm != Move::none())
			return mate_in(ply + 1);
#endif
	}

	// 王手時は全応手、平時は捕獲のみ
	std::vector<std::pair<int, Move>> moves;
	if (inCheck) {
		for (auto ext : MoveList<EVASIONS_ALL>(pos)) {
			Move m = ext;
			if (pos.legal(m))
				moves.emplace_back(order_score(pos, m), m);
		}
	} else {
		for (auto ext : MoveList<CAPTURES_ALL>(pos)) {
			Move m = ext;
			if (pos.legal(m))
				moves.emplace_back(order_score(pos, m), m);
		}
	}
	if (inCheck && moves.empty())
		return mated_in(ply);

	std::sort(moves.begin(), moves.end(),
	          [](const auto& a, const auto& b) { return a.first > b.first; });

	for (auto& [s, m] : moves) {
		StateInfo st;
		pos.do_move(m, st);
		Value v = -qsearch(pos, -beta, -alpha, ply + 1);
		pos.undo_move(m);
		if (v > best) {
			best = v;
			if (v > alpha) {
				alpha = v;
				if (alpha >= beta)
					break;
			}
		}
	}
	return best;
}

// 相手ノードの非千里眼評価。返り値は相手視点。
//
// 素のnegamaxは相手ノードで「こちらの駒が全部見えている相手の最善手」を選ぶ。
// ついたて将棋の相手にはこちらの駒が見えないので、これは
//   - 前進の過小評価(進めた駒は必ず取られると読む)
//   - 王手の過小評価(探索内の相手は必ず正しく逃げる)
//   - 妨害の無価値化(探索内の相手は反則しない)
// という構造的なバイアスになる。ここを信念側と同じ生成モデルに揃える。
Value DSearch::opp_node(Position& pos, int depth, int ply) {
	const Color opp     = ~us;
	const bool  inCheck = pos.in_check();

	// 相手の視界(相手自身の駒だけ)での「指したい手」。真の盤では反則になる手も
	// 含まれていて、その割合が相手の反則確率になる。
	std::vector<OppIntent> intents;
	enumerate_opp_intents(pos, opp, intents, inCheck, *cfg);
	const size_t n = intents.size();
	if (n == 0)
		return mated_in(ply);  // 動かせる駒も打てる駒もない

	// prior の softmax + ε一様。信念の sample_policy と同じ重み付けにする
	// (相手モデルが探索と信念で食い違うと、p_legal と探索値が別の相手を仮定する)。
	double mx = -1e18;
	for (const auto& it : intents)
		mx = std::max(mx, double(it.score));
	std::vector<double> w(n);
	double sum = 0;
	for (size_t i = 0; i < n; ++i) {
		w[i] = std::exp((double(intents[i].score) - mx) / cfg->policyTemp);
		sum += w[i];
	}
	for (size_t i = 0; i < n; ++i)
		w[i] = (1.0 - cfg->policyEps) * (w[i] / sum) + cfg->policyEps / double(n);

	// 合法な意図の質量 p_ok。相手は反則しても手番が変わらず指し直しになるので、
	// 1手番あたりの期待反則回数は幾何分布で (1 − p_ok)/p_ok。
	//
	// enumerate_opp_intents は相手の合法手を必ず含む(見えないこちらの駒は
	// スライダーの利きを伸ばす方向にしか効かないので、意図の集合は合法手の上位集合)。
	// したがって「合法な意図がゼロ = 相手に合法手がない = 相手の負け」が厳密に言える。
	std::vector<uint32_t> legal;
	legal.reserve(n);
	double pOk = 0;
	for (size_t i = 0; i < n; ++i)
		if (pos.pseudo_legal_s<true>(intents[i].m) && pos.legal(intents[i].m)) {
			legal.push_back(uint32_t(i));
			pOk += w[i];
		}
	if (legal.empty())
		return mated_in(ply);

	// 展開する応手 = prior上位k手。
	int kWant = (ply == 1) ? (oppK1 > 0 ? oppK1 : cfg->oppReplyK) : cfg->oppReplyKDeep;
	size_t k  = std::min(size_t(std::max(1, kWant)), legal.size());
	std::partial_sort(legal.begin(), legal.begin() + k, legal.end(),
	                  [&](uint32_t a, uint32_t b) { return w[a] > w[b]; });
	std::vector<uint32_t> sel(legal.begin(), legal.begin() + k);

	// これに「最も価値の高い捕獲」を必ず足す。λ項(千里眼の最善応手)はこの集合の
	// 上でしか取れないので、priorの低い殺し手 —— まさに「こちらの駒が取られる筋」——
	// が漏れると、混合が丸ごと楽観に倒れる。期待値側は元の重み w のままなので、
	// prior の低いこの手を足しても期待値はほとんど動かない。
	{
		uint32_t bestCap = UINT32_MAX;
		int      bestVal = 0;
		for (uint32_t i : legal) {
			Move m = intents[i].m;
			if (m.is_drop())
				continue;
			Piece cap = pos.piece_on(m.to_sq());
			if (cap == NO_PIECE)
				continue;
			int v = Eval::CapturePieceValue[type_of(cap)];
			if (v > bestVal) { bestVal = v; bestCap = i; }
		}
		if (bestCap != UINT32_MAX && std::find(sel.begin(), sel.end(), bestCap) == sel.end())
			sel.push_back(bestCap);
	}

	double wSel = 0;
	for (uint32_t i : sel)
		wSel += w[i];
	if (!(wSel > 0))
		wSel = 1.0;  // ε一様混合があるので通常ありえないが、0除算だけは構造的に防ぐ

	// 期待値ノードなのでαβ窓は使えない(どの子も重み付きで効くため打ち切れない)。
	double expVal = 0;      // 相手視点の期待値
	double maxVal = -1e18;  // 同じ集合の上での千里眼最善
	// 「選んだ応手が相手の合法手を網羅していて、そのすべてが同じ決着」なら、
	// 確率モデルによらず本物の詰み。top-k で間引いているときは網羅していないので
	// 詰みとは言えない。どちら向きの詰みも保存する必要がある:
	//   allWeMate   … どの応手でも相手が詰む(こちらの勝ち)
	//   allTheyMate … どの応手でもこちらが詰まされる(こちらの負け)
	bool  allWeMate   = true;
	bool  allTheyMate = true;
	Value bestMateV   = -VALUE_INFINITE;  // 相手視点で最も粘れる詰み手順
	Value slowestMateV = VALUE_INFINITE;  // 相手視点で最も遅い「こちらを詰ます」手順
	const bool exhaustive = sel.size() == legal.size();
	for (uint32_t i : sel) {
		StateInfo st;
		pos.do_move(intents[i].m, st);
		Value v = -search(pos, depth - 1, -VALUE_INFINITE, VALUE_INFINITE, ply + 1);
		pos.undo_move(intents[i].m);
		if (v <= VALUE_MATED_IN_MAX_PLY)
			bestMateV = std::max(bestMateV, v);
		else
			allWeMate = false;
		if (v >= VALUE_MATE_IN_MAX_PLY)
			slowestMateV = std::min(slowestMateV, v);
		else
			allTheyMate = false;
		// 「相手が3割の確率で詰みを見逃す」は大きな正の値であって詰みではない。
		// 期待値に混ぜる前に飽和させておかないと、詰みスコアが確率で薄まった値が
		// 詰みスコアの範囲に残って上位ノードの解釈を壊す。
		double s = squash_cp(v);
		expVal += (w[i] / wSel) * s;
		maxVal = std::max(maxVal, s);
	}

	// 相手の合法手を全部読んで全部こちらの勝ちだった = **確定した勝ち**。
	// 相手が何を指しても詰むし、相手が反則しても局面は変わらず指し直しになるだけ
	// (反則はむしろ相手の予算を削る)ので、この向きは無条件に詰みスコアでよい。
	// ここで詰みスコアを返さないと、深いところで見つけた詰みが下の ±2500 クランプで
	// 「ふつうの優勢」と同点になり、最終選択の乱数タイブレーク(0〜4cp)が詰みを蹴る。
	// 終盤は相手の合法手が k 以下に減ることが多いので、この経路は実際に効く。
	if (exhaustive && allWeMate)
		return bestMateV;

	// 逆向き(どの合法応手でもこちらが詰まされる)は **対称ではない**。
	// 相手にはこちらの駒が見えないので、その詰ます合法手に辿り着く前に
	// 反則を重ねる。exhaustive になるのは相手の合法手が少ない局面 = p_ok が
	// 小さい局面なので、まさに相手が反則を量産する状況でもある。
	// 相手の反則累計が多ければ、真の決着は「こちらの反則勝ち」でありうる。
	// つまりこれは確定した敗北ではなく、下の混合(反則項つき)で評価すべき量。
	//
	// ただし反則の価値を数えていない設定(foulGain == 0、既定)では、
	// モデル上「相手の反則は無価値」なので詰みとして扱ってよい。
	// この場合だけ短絡し、数えているときは混合に委ねて反則項を効かせる。
	// 最も遅い詰みを返すのは、相手が最善の詰まし方を選ぶとは限らないため。
	if (exhaustive && allTheyMate && foulGain <= 0.0)
		return slowestMateV;

	const double lambda = std::clamp(cfg->oppLambda, 0.0, 1.0);
	double v = (1.0 - lambda) * expVal + lambda * maxVal;

	// 相手の期待反則。相手視点の値なので減点する。
	// p_ok→0 で発散するが、p_ok は手書きpriorに依存する粗い量なので上限を掛ける
	// (青天井にすると「相手は必ず反則する」と信じ込んだ楽観的な読み筋を選ぶ)。
	const double eFoul = std::min(cfg->oppFoulCap, (1.0 - pOk) / std::max(pOk, 1e-6));
	// foul_value は「持ち点の1/10」のスケールで、相手の反則累計が増えると
	// 10/(10-f) で急騰する(f=9 で 8900cp)。これをそのまま局面評価に足すと
	// **相手が反則を重ねるほど全相手ノードが下のクランプに張り付き、
	// 位置評価が丸ごと消える**(反則経済が勝敗を決める終盤でそうなる)。
	// 局面評価と同じ通貨で足す以上、局面評価のレンジに収まる量に抑える必要がある。
	v -= std::min(foulGain * eFoul, cfg->oppFoulMaxCp);

	// 詰みスコアの範囲には入れない(この値は確率混合であって詰みの保証ではない)。
	// 本物の詰みを返すのは上の mated_in(2か所)と exhaustive&&allWeMate、
	// および foulGain==0 のときの exhaustive&&allTheyMate だけ。
	// なお「一部の応手だけが詰み」は詰みではない —— 相手にこちらの駒は見えないので、
	// 詰ます手を選んでくれるとは限らないし、避けてくれるとも限らない。
	// それは期待値として上の混合に入っているのが正しい。
	return Value(int(std::clamp(v, -2500.0, 2500.0)));
}

Value DSearch::search(Position& pos, int depth, Value alpha, Value beta, int ply) {
	if (depth <= 0)
		return qsearch(pos, alpha, beta, ply);
	++nodes;
	if (ply >= MAX_PLY || nodes > nodesLimit)
		return Eval::evaluate(pos);

	// 相手ノードの非千里眼モデル。mate_1ply より前に分岐すること:
	// 「相手が1手詰めを見つける」のはこちらの玉が見えている前提そのもので、
	// この施策が消したいバイアスの中心にある。
	if (cfg && cfg->oppModel > 0 && us != COLOR_NB && pos.side_to_move() != us
	    && (cfg->oppModel >= 2 || ply == 1))
		return opp_node(pos, depth, ply);

	const bool inCheck = pos.in_check();

#if defined(USE_MATE_1PLY)
	if (!inCheck) {
		Move mm = Mate::mate_1ply(pos);
		if (mm != Move::none())
			return mate_in(ply + 1);
	}
#endif

	std::vector<std::pair<int, Move>> moves;
	for (auto ext : MoveList<LEGAL_ALL>(pos)) {
		Move m = ext;
		moves.emplace_back(order_score(pos, m), m);
	}
	if (moves.empty())
		return mated_in(ply);  // 合法手なし = 負け(ステイルメイト含む)

	std::sort(moves.begin(), moves.end(),
	          [](const auto& a, const auto& b) { return a.first > b.first; });

	Value best = -VALUE_INFINITE;
	for (auto& [s, m] : moves) {
		StateInfo st;
		pos.do_move(m, st);
		Value v = -search(pos, depth - 1, -beta, -alpha, ply + 1);
		pos.undo_move(m);
		if (v > best) {
			best = v;
			if (v > alpha) {
				alpha = v;
				if (alpha >= beta)
					break;
			}
		}
	}
	return best;
}

double squash_cp(Value v) {
	if (v >= VALUE_MATE_IN_MAX_PLY)
		return 3000.0;
	if (v <= VALUE_MATED_IN_MAX_PLY)
		return -3000.0;
	double d = double(v);
	return std::clamp(d, -2500.0, 2500.0);
}

} // namespace Tsuitate
} // namespace YaneuraOu

#endif // TSUITATE_ENGINE
