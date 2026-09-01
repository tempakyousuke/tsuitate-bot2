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

// 確率混合の中で使う飽和。
//
// `squash_cp` は think() の集計用で、通常評価を ±2500 に、詰みを ±3000 に潰す。
// これを **相手ノードの子に掛けてはいけない**: oppModel=2 では子の部分木の中に
// さらに深い相手ノードがあり、その返り値は既に混合空間(最大 ±MIX_MAX)にいる。
// そこへ `squash_cp` を掛けると (2500, MIX_MAX] が 2500 に丸められ、
// 深いところの詰み寄りの評価が「ふつうの優勢」に潰れる。
//
// 空間の取り決め:
//   本物の詰み      → ±MATE_MIX (±3000)   … この値だけが詰みを意味する
//   それ以外        → ±MIX_MAX  (±2900)   … 通常評価も混合値もこの帯に収める
// MIX_MAX と MATE_MIX の間に 100cp の余白を空けているのは、think() の最終選択が
// 0〜4cp の乱数タイブレークを足すため。余白がないと
// 「確率的にほぼ詰み」が「証明された詰み」と同点になり、乱数で詰みを蹴りうる。
constexpr double MIX_MAX  = 2900.0;
constexpr double MATE_MIX = 3000.0;

double squash_for_mix(Value v) {
	if (v >= VALUE_MATE_IN_MAX_PLY)
		return MATE_MIX;
	if (v <= VALUE_MATED_IN_MAX_PLY)
		return -MATE_MIX;
	// 既に混合空間にいる値(|v| <= MIX_MAX)はそのまま通る = べき等
	return std::clamp(double(v), -MIX_MAX, MIX_MAX);
}

// --- 置換表の詰みスコア補正(標準的なもの) ---
// 詰みスコアは「rootからの距離」を含むので、そのまま保存すると別の深さから
// 再利用したときに距離がずれる。保存時は「このノードからの距離」に直し(to_tt)、
// 読むときに現在の ply を足し戻す(from_tt)。
// bound は types.h の共通 enum Bound を使う(独自enumを切ると数値が本体の
// BOUND_* とずれ、コピー適応したコードやデバッグ出力が静かに誤分類する)。

int16_t value_to_tt(Value v, int ply) {
	if (v >= VALUE_MATE_IN_MAX_PLY)
		return int16_t(v + ply);
	if (v <= VALUE_MATED_IN_MAX_PLY)
		return int16_t(v - ply);
	return int16_t(v);
}

Value value_from_tt(int16_t v, int ply) {
	if (v >= VALUE_MATE_IN_MAX_PLY)
		return Value(v - ply);
	if (v <= VALUE_MATED_IN_MAX_PLY)
		return Value(v + ply);
	return Value(v);
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
			// 同じ捕獲に成/不成の2変種があるとき、`>` だけだと partial_sort 後の
			// 未規定な順序で不成が残りうる。λ項が見たいのは「最も痛い応手」なので、
			// 価値が同じなら成りを選ぶ。
			int v = Eval::CapturePieceValue[type_of(cap)];
			const bool better = v > bestVal
			                 || (v == bestVal && m.is_promote() && bestCap != UINT32_MAX
			                     && !intents[bestCap].m.is_promote());
			if (better) { bestVal = v; bestCap = i; }
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
		//
		// ここで `squash_cp` を使うと、子の部分木にある**より深い相手ノード**が
		// 返した混合値(既に混合空間にいる)の (2500, 2900] が 2500 に丸められ、
		// 深いところの詰み寄りの評価が消える(oppmodel 2 で顕在化)。
		// べき等な `squash_for_mix` を使うこと。
		double s = squash_for_mix(v);
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

	// 逆向き(どの合法応手でもこちらが詰まされる)。
	//
	// 厳密には確定した敗北ではない: 相手にはこちらの駒が見えないので、その詰ます
	// 合法手に辿り着く前に反則を重ねうる。相手が反則予算(残り 10 − oppFouls 回)を
	// 使い切れば、真の決着は「こちらの反則勝ち」になる。
	//
	// **しかしその逃げ道を反則項で埋め合わせてはいけない。** 一度そうしたところ、
	//   確定詰み  → 3000 − min(foulGain·eFoul, oppFoulMaxCp) = 2200〜2300
	//   ただの劣勢 → 2500(クランプ上限)
	// となり、**確定で詰まされる筋がただの飛車損より高く評価される**逆転が起きた。
	// 反則項は「1手番あたりの期待反則回数」(高々 oppFoulCap = 2回)しか見ておらず、
	// 相手が反則負けするのに必要な残り10回ぶんの予算を表現していないので、
	// 終端の敗北に対する割引としては桁が合わない。
	//
	// よってここは詰みスコアを返し、「相手が先に反則負けする」逃げ道は
	// **モデル化しない**(既知の近似)。最も遅い詰みを返すのは、
	// 盲目の相手が最善の詰まし方を選ぶとは限らないため。
	if (exhaustive && allTheyMate)
		return slowestMateV;

	const double lambda = std::clamp(cfg->oppLambda, 0.0, 1.0);
	double v = (1.0 - lambda) * expVal + lambda * maxVal;

	// 相手の期待反則。相手視点の値なので減点する。
	//
	// **この項は ply==1(こちらの候補手の直後の相手の手番)でしか足さない。**
	// 設計上この項が表すのは「いま指す手が相手に何回反則させるか」であって、
	// 読み筋の先の相手の手番ぶんまで足し上げるものではない。
	// oppModel=2 では相手ノードが ply=1,3,5,… と並ぶので、各ノードで足すと
	//   root = E[評価] + foul(ply1) + foul(ply3) + foul(ply5) + …
	// となり、`oppFoulMaxCp` はノード単位の上限にしかならず**読み筋単位では
	// 青天井**になる。実測でも oppmodel 2 / foulgain 4 / oppfoulmax 2500 だと
	// 深さ2で cp=30、深さ4で cp=2139 と、相手ノードが1つ増えるだけで
	// 2000cp 以上動いた。行き着く先は全候補が ±MIX_MAX に張り付いて、
	// 手の選択が 0〜4cp の乱数タイブレークに退化する状態 ——
	// `oppFoulMaxCp` がまさに防ぐつもりだった「位置評価が消える」失敗そのもの。
	//
	// そもそも反則予算は対局全体で10回しかない有限資源なので、
	// 手番ごとに独立に足し上げる形が誤り。1手番ぶんだけ数える。
	if (ply == 1) {
		// p_ok→0 で発散するが、p_ok は手書きpriorに依存する粗い量なので上限を掛ける
		// (青天井にすると「相手は必ず反則する」と信じ込んだ楽観的な読み筋を選ぶ)。
		const double eFoul = std::min(cfg->oppFoulCap, (1.0 - pOk) / std::max(pOk, 1e-6));
		// foul_value は「持ち点の1/10」のスケールで、相手の反則累計が増えると
		// 10/(10-f) で急騰する(f=9 で 8900cp)。局面評価と同じ通貨で足す以上、
		// 局面評価のレンジに収まる量に抑える必要がある。
		v -= std::min(foulGain * eFoul, cfg->oppFoulMaxCp);
	}

	// 詰みスコアの範囲には入れない(この値は確率混合であって詰みの保証ではない)。
	// 本物の詰みを返すのは上の mated_in(2か所)と exhaustive&&allWeMate /
	// exhaustive&&allTheyMate だけ。
	// なお「一部の応手だけが詰み」は詰みではない —— 相手にこちらの駒は見えないので、
	// 詰ます手を選んでくれるとは限らないし、避けてくれるとも限らない。
	// それは期待値として上の混合に入っているのが正しい。
	//
	// クランプは **±MIX_MAX(2900)**。2つの理由がある:
	//   - ±2500 に潰すと、ply=1 より深くで見つけた詰みが通常評価の上限と同じ値になり、
	//     詰み筋が「ふつうの優勢」に見えて選ばれなくなる
	//     (例: p_legal 0.95 の詰み 2375 < p_legal 1.0 のふつうの優勢 2500)
	//   - かといって ±MATE_MIX(3000)まで許すと、**上で引いた反則項のせいで
	//     ただの優勢が飽和して 3000 に届き、証明された詰みと同点になる**。
	//     反則項は常に v を下げるので、相手視点で大きく負けている局面が
	//     −3000 に張り付く。それは squash_cp が詰みに使う符号そのもので、
	//     think() の 0〜4cp の乱数タイブレークが詰みでない手を詰みより上に置きうる。
	// MIX_MAX と MATE_MIX の 100cp の余白がこの衝突を防ぐ。
	//
	// この値は既に混合空間にいるので、think() 側では rootMixed を見て
	// 二重に squash しないこと。
	if (ply == 1)
		rootMixed = true;
	return Value(int(std::clamp(v, -MIX_MAX, MIX_MAX)));
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

	// --- 置換表プローブ(§3.2。ctx == nullptr なら従来と完全に同一の経路) ---
	//
	// 値によるカットオフは**同世代のエントリだけ**に許す(理由は SearchContext の
	// コメント)。旧世代・浅いエントリでも指し手(move16)はオーダリングに使う。
	const Value alphaOrig = alpha;
	uint16_t    ttRaw     = 0;
	TTEntry*    tte       = nullptr;
	if (ctx) {
		tte = &ctx->slot(pos.key());
		if (tte->key == pos.key()) {
			ttRaw = tte->move16;
			if (tte->gen == ctx->gen && tte->depth >= depth) {
				Value v = value_from_tt(tte->value, ply);
				if (tte->bound == BOUND_EXACT
				    || (tte->bound == BOUND_LOWER && v >= beta)
				    || (tte->bound == BOUND_UPPER && v <= alpha))
					return v;
			}
		}
	}

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
		int  s;
		if (!ctx) {
			s = order_score(pos, m);  // 従来どおり(MVV-LVA + 成り)
		} else {
			// TT手 > 捕獲(MVV-LVA) > killer > history。
			// 捕獲とquietの帯を分離する(従来は安い駒で大駒を取る手が quiet より
			// 下に沈むことがあった)。この並びは tt フラグの下でだけ有効。
			if (m.raw() == ttRaw && ttRaw != 0) {
				s = 1 << 30;
			} else if (!m.is_drop() && pos.piece_on(m.to_sq()) != NO_PIECE) {
				s = (1 << 20) + order_score(pos, m);
			} else {
				if (m == ctx->killer[ply][0])
					s = (1 << 18);
				else if (m == ctx->killer[ply][1])
					s = (1 << 18) - 1;
				else
					s = ctx->hist_of(pos.side_to_move(), m.raw());
				if (m.is_promote())
					s += 300;
			}
		}
		moves.emplace_back(s, m);
	}
	if (moves.empty())
		return mated_in(ply);  // 合法手なし = 負け(ステイルメイト含む)

	std::sort(moves.begin(), moves.end(),
	          [](const auto& a, const auto& b) { return a.first > b.first; });

	Value best = -VALUE_INFINITE;
	Move  bestMove = Move::none();
	for (auto& [s, m] : moves) {
		StateInfo st;
		pos.do_move(m, st);
		Value v = -search(pos, depth - 1, -beta, -alpha, ply + 1);
		pos.undo_move(m);
		if (v > best) {
			best = v;
			bestMove = m;
			if (v > alpha) {
				alpha = v;
				if (alpha >= beta) {
					// beta カット: quiet なら killer / history を更新
					if (ctx && (m.is_drop() || pos.piece_on(m.to_sq()) == NO_PIECE)) {
						if (ctx->killer[ply][0] != m) {
							ctx->killer[ply][1] = ctx->killer[ply][0];
							ctx->killer[ply][0] = m;
						}
						int16_t& h = ctx->hist_of(pos.side_to_move(), m.raw());
						h = int16_t(std::min<int>(SearchContext::HIST_MAX,
						                          int(h) + depth * depth));
					}
					break;
				}
			}
		}
	}

	// --- 置換表ストア ---
	// nodesLimit を超えた探索は途中で静的評価を返しており値が汚れているので書かない。
	// 置換は「同世代でより深い既存エントリ」だけを尊重し、それ以外は上書きする
	// (旧世代はどれだけ深くても置き換え対象。世代が違う値はカットオフに使えないため)。
	if (ctx && nodes <= nodesLimit) {
		const bool keep = tte->key == pos.key() && tte->gen == ctx->gen
		                  && tte->depth > depth;
		if (!keep) {
			tte->key    = pos.key();
			tte->value  = value_to_tt(best, ply);
			// fail-low(UPPER)の bestMove は「最善の証明」ではないが、
			// オーダリングのヒントとしては十分機能するので保存する
			tte->move16 = bestMove.raw();
			tte->depth  = int8_t(std::min(depth, 127));
			tte->bound  = best >= beta ? BOUND_LOWER : best > alphaOrig ? BOUND_EXACT : BOUND_UPPER;
			tte->gen    = ctx->gen;
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
