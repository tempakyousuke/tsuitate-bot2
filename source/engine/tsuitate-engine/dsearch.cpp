// ついたて将棋エンジン: 確定化局面探索の実装
#include "dsearch.h"

#if defined(TSUITATE_ENGINE)

#include <algorithm>

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

Value DSearch::search(Position& pos, int depth, Value alpha, Value beta, int ply) {
	if (depth <= 0)
		return qsearch(pos, alpha, beta, ply);
	++nodes;
	if (ply >= MAX_PLY || nodes > nodesLimit)
		return Eval::evaluate(pos);

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
