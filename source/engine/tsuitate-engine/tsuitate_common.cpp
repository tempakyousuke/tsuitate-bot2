// ついたて将棋エンジン: OwnView・候補手生成・表記変換
#include "tsuitate_common.h"

#if defined(TSUITATE_ENGINE)

#include <algorithm>
#include <cmath>
#include <thread>

#include "../../bitboard.h"

namespace YaneuraOu {
namespace Tsuitate {

// ---------------------------------------------------------------------------
// OwnView
// ---------------------------------------------------------------------------

namespace {

// from と to の間のマス(両端を含まない)。飛角香の経路。
// 直線上にない(桂・玉など)なら空。
std::vector<Square> between_squares(Square from, Square to) {
	std::vector<Square> out;
	int df = int(file_of(to)) - int(file_of(from));
	int dr = int(rank_of(to)) - int(rank_of(from));
	int steps = std::max(std::abs(df), std::abs(dr));
	if (steps <= 1)
		return out;
	// 直線(縦・横・斜め)でなければ経路はない
	if (!(df == 0 || dr == 0 || std::abs(df) == std::abs(dr)))
		return out;
	int sf = (df > 0) - (df < 0);
	int sr = (dr > 0) - (dr < 0);
	for (int k = 1; k < steps; ++k)
		out.push_back(File(int(file_of(from)) + sf * k) | Rank(int(rank_of(from)) + sr * k));
	return out;
}

} // namespace

void OwnView::reset(Color us_) {
	us = us_;
	for (auto sq : SQ)
		board[sq] = NO_PIECE;
	hand = HAND_ZERO;
	ourFouls = oppFouls = 0;
	inCheckNow = false;
	lastOppCaptureSq = SQ_NB;
	for (int i = 0; i < 8; ++i)
		oppCaptured[i] = 0;

	// 演繹の初期状態: 平手初期局面は相手の配置も完全に既知なので、
	// 相手駒のないマスは全部「確実に空き(陳腐化度0)」から始まる。
	// 相手陣は相手から見て1段目(全筋)・2段目(2筋と8筋の角飛)・3段目(全筋の歩)。
	//
	// 段の向きに注意: relative_rank(c, r) は「cの最奥が8」なので、
	// 「相手から見た段」が欲しいときに引くのは relative_rank(opp, r) ではなく
	// relative_rank(us, r) のほう(us の最奥 = 8 = 相手から見て9段目)。
	oppMoveCount = 0;
	for (auto sq : SQ) {
		Rank rr = relative_rank(us, rank_of(sq));  // 0 = 相手の最奥
		File f  = file_of(sq);
		bool oppPiece = (rr == RANK_1) || (rr == RANK_3)
		                || (rr == RANK_2 && (f == FILE_2 || f == FILE_8));
		emptyStamp[sq] = oppPiece ? -9999 : 0;
	}

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
		Square to = m.to_sq();
		// 打てた = 着地マスは空いていた
		emptyStamp[to] = oppMoveCount;
		board[to] = make_piece(us, pt);
	} else {
		Square from = m.from_sq(), to = m.to_sq();
		Piece  pc   = board[from];
		// 通れた = 経路の通過マスは空いていた。移動元も自駒が退いただけで空き。
		emptyStamp[from] = oppMoveCount;
		emptyStamp[to]   = oppMoveCount;
		for (Square s : between_squares(from, to))
			emptyStamp[s] = oppMoveCount;
		board[from] = NO_PIECE;
		if (m.is_promote())
			pc = Piece(pc | PIECE_PROMOTE);
		board[to] = pc;
		if (capRole != NO_PIECE_TYPE)
			add_hand(hand, PieceType(capRole & 7)); // 成りを戻して持ち駒へ
	}
}

void OwnView::apply_opp_capture(Square capSq) {
	// 相手が1手指した = 空き情報がすべて1手ぶん古くなる
	++oppMoveCount;
	lastOppCaptureSq = capSq;
	if (capSq == SQ_NB)
		return;
	Piece pc = board[capSq];
	if (pc != NO_PIECE && type_of(pc) != KING)
		oppCaptured[pc & 7]++;
	board[capSq] = NO_PIECE;
	// 取られたマスには「取った相手の駒」が乗っている。ここを刻み直さないと、
	// 直前まで自駒がいたせいで emptyStamp が新しいまま
	//(= 陳腐化度が小さい = まだ空きらしい)になり、
	// 相手駒が確実にいるマスの重みを下げるという逆向きの演繹になってしまう。
	// 最新の1マスは synthesize の強制配置(forcedSq)が拾うが、
	// それ以前に取られたマスはここで「空きとは分かっていない」に戻しておく。
	emptyStamp[capSq] = -9999;
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
// 相手の意図(非千里眼モデル)
// ---------------------------------------------------------------------------

int fast_policy_score(const Position& pos, Color opp, Move m, bool inCheck,
                      const Config& cfg) {
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

	// 王手を宣言された側は「自分が王手されている」ことだけは知っている
	// (宣言は両者に届く)。ただしどの駒からの王手かは見えないので、
	// 確実に応じられる手は玉を動かすことしかない。素のpriorは玉移動を
	// 強く嫌う(下の -250)ので、王手中はその符号を逆転させる。
	const bool checkAware = inCheck && cfg.oppCheckPrior != 0;

	if (m.is_drop()) {
		// 打ちは移動手に比べて少数派。敵陣(=こちら側)への打ち込みは好まれる。
		s -= 150;
		if (relative_rank(opp, rank_of(to)) <= RANK_4)
			s += 80;
		// 王手されているのに打つのは「合駒」だが、どこを遮ればよいか見えないので
		// 相手にとってはほぼ当てずっぽう。玉を逃がす手に比べて選ばれにくい。
		if (checkAware)
			s -= 200;
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
		s += checkAware ? 400 : -250;  // 平時はむやみに動かさない / 王手なら逃げる
	return s;
}

void enumerate_opp_intents(const Position& pos, Color opp, std::vector<OppIntent>& out,
                           bool inCheck, const Config& cfg) {
	out.clear();
	// 相手に見えている盤 = 相手自身の駒だけ。こちらの駒は存在しないものとして
	// 手を生成するので、経路封鎖・打ちマス占有・自玉放置で反則になる手も混じる。
	// その割合がそのまま相手の反則確率なので、ここで除いてはいけない。
	const Bitboard oppOcc = pos.pieces(opp);

	// 盤上の駒の移動
	Bitboard movers = oppOcc;
	while (movers) {
		Square from = movers.pop();
		Piece  pc   = pos.piece_on(from);
		PieceType pt = type_of(pc);
		Bitboard att = effects_from(pc, from, oppOcc) & ~oppOcc;
		while (att) {
			Square to = att.pop();
			bool promotable = pt <= ROOK && canPromote(opp, from, to);
			if (piece_can_stay(opp, pt, to)) {
				Move m = make_move(from, to, opp, pt);
				out.push_back({m, fast_policy_score(pos, opp, m, inCheck, cfg)});
			}
			if (promotable) {
				Move m = make_move_promote(from, to, opp, pt);
				out.push_back({m, fast_policy_score(pos, opp, m, inCheck, cfg)});
			}
		}
	}

	// 打ち。二歩は相手自身の歩だけで判定できる(相手には自分の歩が見えている)。
	const Hand h = pos.hand_of(opp);
	for (PieceType pt : {PAWN, LANCE, KNIGHT, SILVER, GOLD, BISHOP, ROOK}) {
		if (!hand_exists(h, pt))
			continue;
		Bitboard banFiles = Bitboard(ZERO);
		if (pt == PAWN) {
			Bitboard pawns = pos.pieces(PAWN) & oppOcc;
			while (pawns)
				banFiles = banFiles | file_bb(file_of(pawns.pop()));
		}
		for (auto to : SQ) {
			if (oppOcc & to)
				continue;
			if (!piece_can_stay(opp, pt, to))
				continue;
			if (pt == PAWN && (banFiles & to))
				continue;
			Move m = make_move_drop(pt, to, opp);
			out.push_back({m, fast_policy_score(pos, opp, m, inCheck, cfg)});
		}
	}
}

double foul_value(double baseCp, double stepCp, int fouls) {
	// f=10 は反則負けで局が終わっているので到達しない。0除算を構造的に防ぐため
	// 上限9でクランプする(呼び出し側の累計がずれても壊れないように)。
	int f = std::clamp(fouls, 0, 9);
	return (baseCp + stepCp * double(f)) * (10.0 / double(10 - f));
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

// ---------------------------------------------------------------------------
// 並列ヘルパ
// ---------------------------------------------------------------------------

void run_workers(int nThreads, const std::function<void(int)>& fn) {
	if (nThreads <= 1) {
		fn(0);
		return;
	}
	std::vector<std::thread> ts;
	ts.reserve(size_t(nThreads - 1));
	for (int w = 1; w < nThreads; ++w)
		ts.emplace_back([&fn, w] { fn(w); });
	fn(0);
	for (auto& t : ts)
		t.join();
}

} // namespace Tsuitate
} // namespace YaneuraOu

#endif // TSUITATE_ENGINE
