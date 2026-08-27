// ついたて将棋エンジン: OwnView・候補手生成・表記変換
#include "tsuitate_common.h"

#if defined(TSUITATE_ENGINE)

#include "../../bitboard.h"

namespace YaneuraOu {
namespace Tsuitate {

// ---------------------------------------------------------------------------
// OwnView
// ---------------------------------------------------------------------------

void OwnView::reset(Color us_) {
	us = us_;
	for (auto sq : SQ)
		board[sq] = NO_PIECE;
	hand = HAND_ZERO;
	ourFouls = oppFouls = 0;
	inCheckNow = false;

	// 平手初期配置の自分側だけを置く。
	// 先手(BLACK)基準で並べ、後手ならInv()で180度回転する。
	auto put = [&](File f, Rank r, PieceType pt) {
		Square sq = f | r;
		if (us == WHITE)
			sq = Inv(sq);
		board[sq] = make_piece(us, pt);
	};
	for (File f = FILE_1; f <= FILE_9; ++f)
		put(f, RANK_7, PAWN);
	put(FILE_1, RANK_9, LANCE);  put(FILE_9, RANK_9, LANCE);
	put(FILE_2, RANK_9, KNIGHT); put(FILE_8, RANK_9, KNIGHT);
	put(FILE_3, RANK_9, SILVER); put(FILE_7, RANK_9, SILVER);
	put(FILE_4, RANK_9, GOLD);   put(FILE_6, RANK_9, GOLD);
	put(FILE_5, RANK_9, KING);
	put(FILE_8, RANK_8, BISHOP);
	put(FILE_2, RANK_8, ROOK);
}

void OwnView::apply_our_move(Move m, PieceType capRole) {
	if (m.is_drop()) {
		PieceType pt = m.move_dropped_piece();
		sub_hand(hand, pt);
		board[m.to_sq()] = make_piece(us, pt);
	} else {
		Square from = m.from_sq(), to = m.to_sq();
		Piece  pc   = board[from];
		board[from] = NO_PIECE;
		if (m.is_promote())
			pc = Piece(pc | PIECE_PROMOTE);
		board[to] = pc;
		if (capRole != NO_PIECE_TYPE)
			add_hand(hand, PieceType(capRole & 7)); // 成りを戻して持ち駒へ
	}
}

void OwnView::apply_opp_capture(Square capSq) {
	if (capSq == SQ_NB)
		return;
	Piece pc = board[capSq];
	if (pc != NO_PIECE && type_of(pc) != KING)
		oppCaptured[pc & 7]++;
	board[capSq] = NO_PIECE;
}

Bitboard OwnView::occupied() const {
	Bitboard bb = Bitboard(ZERO);
	for (auto sq : SQ)
		if (board[sq] != NO_PIECE)
			bb = bb | sq;
	return bb;
}

Square OwnView::king_square() const {
	for (auto sq : SQ)
		if (board[sq] != NO_PIECE && type_of(board[sq]) == KING)
			return sq;
	return SQ_NB;
}

// ---------------------------------------------------------------------------
// 候補手生成
// ---------------------------------------------------------------------------
// サイトの move-hints.ts と同じ意味論:
//   - 相手駒は存在しないものとして生成(合法性はサーバーのみが知る)
//   - スライダーは自駒にぶつかる手前まで
//   - 打ちは自駒のないマス全部(二歩は自分の歩とだけ判定)
//   - 行き所のない駒は強制成り、それ以外は成り/不成の両変種を生成

std::vector<Move> generate_candidates(const OwnView& view) {
	std::vector<Move> out;
	out.reserve(128);
	const Color    us  = view.us;
	const Bitboard own = view.occupied();

	// 盤上の駒の移動
	for (auto from : SQ) {
		Piece pc = view.board[from];
		if (pc == NO_PIECE)
			continue;
		PieceType pt = type_of(pc);
		Bitboard att = effects_from(pc, from, own) & ~own;
		while (att) {
			Square to = att.pop();
			bool promotable = pt <= ROOK && canPromote(us, from, to); // PAWN..ROOKのみ成れる
			if (piece_can_stay(us, pt, to))
				out.push_back(make_move(from, to, us, pt));
			if (promotable)
				out.push_back(make_move_promote(from, to, us, pt));
		}
	}

	// 打ち
	for (PieceType pt : {PAWN, LANCE, KNIGHT, SILVER, GOLD, BISHOP, ROOK}) {
		if (!hand_exists(view.hand, pt))
			continue;
		// 二歩: 自分の歩がある筋を除外
		Bitboard banFiles = Bitboard(ZERO);
		if (pt == PAWN) {
			for (auto sq : SQ)
				if (view.board[sq] == make_piece(us, PAWN))
					banFiles = banFiles | file_bb(file_of(sq));
		}
		for (auto to : SQ) {
			if (view.board[to] != NO_PIECE)
				continue;
			if (!piece_can_stay(us, pt, to))
				continue;
			if (pt == PAWN && (banFiles & to))
				continue;
			out.push_back(make_move_drop(pt, to, us));
		}
	}

	return out;
}

// ---------------------------------------------------------------------------
// 表記変換
// ---------------------------------------------------------------------------

PieceType role_from_site(const std::string& s) {
	if (s == "pawn") return PAWN;
	if (s == "lance") return LANCE;
	if (s == "knight") return KNIGHT;
	if (s == "silver") return SILVER;
	if (s == "gold") return GOLD;
	if (s == "bishop") return BISHOP;
	if (s == "rook") return ROOK;
	if (s == "king") return KING;
	if (s == "tokin") return PRO_PAWN;
	if (s == "promotedlance") return PRO_LANCE;
	if (s == "promotedknight") return PRO_KNIGHT;
	if (s == "promotedsilver") return PRO_SILVER;
	if (s == "horse") return HORSE;
	if (s == "dragon") return DRAGON;
	return NO_PIECE_TYPE;
}

std::string role_to_site(PieceType pt) {
	switch (pt) {
	case PAWN: return "pawn";
	case LANCE: return "lance";
	case KNIGHT: return "knight";
	case SILVER: return "silver";
	case GOLD: return "gold";
	case BISHOP: return "bishop";
	case ROOK: return "rook";
	case KING: return "king";
	case PRO_PAWN: return "tokin";
	case PRO_LANCE: return "promotedlance";
	case PRO_KNIGHT: return "promotedknight";
	case PRO_SILVER: return "promotedsilver";
	case HORSE: return "horse";
	case DRAGON: return "dragon";
	default: return "?";
	}
}

Square parse_usi_square(const std::string& s) {
	if (s.size() != 2)
		return SQ_NB;
	if (s[0] < '1' || s[0] > '9' || s[1] < 'a' || s[1] > 'i')
		return SQ_NB;
	return File(s[0] - '1') | Rank(s[1] - 'a');
}

std::string usi_square(Square sq) {
	std::string s;
	s += char('1' + file_of(sq));
	s += char('a' + rank_of(sq));
	return s;
}

Move parse_usi_move(const OwnView& view, const std::string& s) {
	if (s.size() < 4)
		return Move::none();
	if (s[1] == '*') {
		// 打ち: "P*5e"
		PieceType pt;
		switch (s[0]) {
		case 'P': pt = PAWN; break;
		case 'L': pt = LANCE; break;
		case 'N': pt = KNIGHT; break;
		case 'S': pt = SILVER; break;
		case 'G': pt = GOLD; break;
		case 'B': pt = BISHOP; break;
		case 'R': pt = ROOK; break;
		default: return Move::none();
		}
		Square to = parse_usi_square(s.substr(2, 2));
		if (to == SQ_NB)
			return Move::none();
		return make_move_drop(pt, to, view.us);
	}
	Square from = parse_usi_square(s.substr(0, 2));
	Square to   = parse_usi_square(s.substr(2, 2));
	if (from == SQ_NB || to == SQ_NB)
		return Move::none();
	Piece pc = view.board[from];
	if (pc == NO_PIECE)
		return Move::none();
	bool promote = s.size() >= 5 && s[4] == '+';
	return promote ? make_move_promote(from, to, view.us, type_of(pc))
	               : make_move(from, to, view.us, type_of(pc));
}

} // namespace Tsuitate
} // namespace YaneuraOu

#endif // TSUITATE_ENGINE
