// ついたて将棋エンジン: 共通型定義
//
// 用語:
//   観測(Observation) … サーバー(審判)がこちらに通知する情報の全量。
//     - 自分の着手が受理された(取った駒の種別つき) / 反則だった(理由なし)
//     - 相手が着手した(自駒が取られたマスつき) / 相手が反則した
//     - 王手宣言(どちらの玉か。両者に通知される)
//   履歴(GameHistory) … 観測の時系列。信念(粒子集合)の唯一の入力。
//
// 接続先サイト(beta.tsuitate.info)のルール:
//   - 反則 = 完全情報の通常将棋ルールで不正な手(打ち歩詰め・自玉放置・二歩等を含む)
//   - 反則しても手番は変わらない。累計10回で反則負け
//   - 王手宣言は正規手の直後に両者へ通知される
#ifndef TSUITATE_COMMON_H_INCLUDED
#define TSUITATE_COMMON_H_INCLUDED

#include "../../config.h"

#if defined(TSUITATE_ENGINE)

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "../../types.h"
#include "../../position.h"
#include "../../misc.h"

namespace YaneuraOu {
namespace Tsuitate {

// ---------------------------------------------------------------------------
// 観測イベント
// ---------------------------------------------------------------------------

enum class EvKind : uint8_t {
	OurMove,   // 自分の着手が受理された
	OurFoul,   // 自分の着手が反則だった(手番は変わらない)
	OppMove,   // 相手が着手した(内容は不明)
	OppFoul,   // 相手が反則した(内容は不明。手番は変わらない)
};

// 王手宣言の状態。着手イベント(OurMove/OppMove)の直後にのみ王手宣言が届くため、
// 「次のイベントか思考開始が来た時点で宣言なし=王手なし」が確定する。
enum class CheckAfter : int8_t {
	Pending = -1,  // まだ確定していない(直後の王手宣言待ち)
	No      = 0,   // 王手なしで確定
	Yes     = 1,   // この手で相手側の玉に王手が掛かった
};

struct HistEvent {
	EvKind     kind;
	Move       move       = Move::none();     // OurMove/OurFoul のみ(自駒情報込みの32bit Move)
	PieceType  capRole    = NO_PIECE_TYPE;    // OurMove: 取った相手駒の盤上種別(成駒あり)
	Square     capSq      = SQ_NB;            // OppMove: 自駒が取られたマス(なければSQ_NB)
	CheckAfter check      = CheckAfter::Pending; // OurMove/OppMove の後の王手宣言
};

// 対局全体の観測履歴。信念の再生成(リプレイ)の入力になる。
struct GameHistory {
	Color                  us = COLOR_NB;
	std::vector<HistEvent> events;

	void clear(Color us_) { us = us_; events.clear(); }

	// 直近の着手イベントの王手宣言を確定させる。
	// declared: 王手宣言が届いたか
	void finalize_pending_check(bool declared) {
		for (auto it = events.rbegin(); it != events.rend(); ++it) {
			if (it->kind == EvKind::OurMove || it->kind == EvKind::OppMove) {
				if (it->check == CheckAfter::Pending)
					it->check = declared ? CheckAfter::Yes : CheckAfter::No;
				return;
			}
		}
	}
};

// ---------------------------------------------------------------------------
// 自分視点の盤面(OwnView)
// ---------------------------------------------------------------------------
// 自分の駒・持ち駒は常に完全既知。全粒子で共有される確定情報であり、
// 候補手生成(相手駒を無視した手)の入力になる。

struct OwnView {
	Color  us = BLACK;
	Piece  board[SQ_NB];      // 自駒のみ(それ以外はNO_PIECE)
	Hand   hand;              // 自分の持ち駒
	int    ourFouls = 0;      // 自分の反則累計
	int    oppFouls = 0;      // 相手の反則累計
	bool   inCheckNow = false; // 直近の相手の手で自玉に王手が掛かっているか
	// 相手に取られた自駒の累計(rawロール別)。相手の持ち駒の上限になる
	// (相手が打った駒は観測できないため、正確な持ち駒枚数は分からない)。
	int    oppCaptured[8] = {};
	// 直前の相手の手で自駒が取られたマス(なければSQ_NB)。
	// 王手宣言と組み合わせると「王手駒はこのマスにいる」可能性が高い。
	Square lastOppCaptureSq = SQ_NB;

	// --- 演繹層(deduction) ---
	// 「そのマスに相手駒がいない」と論理的に確定した時点を、相手の着手数で刻む。
	// 相手駒は相手の着手でしか動かないので、
	//   emptyStamp[sq] == oppMoveCount  ⟺  今も確実にそのマスに相手駒はいない
	// が厳密に成り立つ。差(=陳腐化度)が小さいほど「まだ空きらしい」。
	//
	// 空きが確定するのは:
	//   - 平手初期局面で相手駒がないマス(対局開始時は全部が陳腐化度0)
	//   - 自分の着手が受理されたとき、その経路の通過マス(塞がっていたら反則だった)
	//   - 移動元のマス(自駒が退いた後で、相手駒が湧くことはない)
	//   - 打ちの着地マス(埋まっていたら反則だった)
	int    oppMoveCount = 0;
	int    emptyStamp[SQ_NB];

	void reset(Color us_);
	// 自分の着手を反映(cap: 取った相手駒の盤上種別。持ち駒に入れる)
	void apply_our_move(Move m, PieceType capRole);
	// 相手の着手を反映(capSq: 自駒が取られたマス)
	void apply_opp_capture(Square capSq);

	Bitboard occupied() const;
	Square   king_square() const;

	// そのマスに相手駒がいないと分かってから相手が指した回数。
	// 0 = 確実に空き。大きいほど何が来ていてもおかしくない。
	int stale(Square sq) const { return oppMoveCount - emptyStamp[sq]; }
};

// この駒がtoに不成で存在できるか(行き所のない駒の判定)
inline bool piece_can_stay(Color us, PieceType pt, Square to) {
	Rank r = rank_of(to);
	Rank last  = us == BLACK ? RANK_1 : RANK_9;
	Rank last2 = us == BLACK ? RANK_2 : RANK_8;
	if (pt == PAWN || pt == LANCE)
		return r != last;
	if (pt == KNIGHT)
		return r != last && r != last2;
	return true;
}

// 候補手 = 「自分の駒だけを考慮した」指し手。盤上の見えない相手駒による
// 合法性(経路・打ちマス・自玉放置など)はサーバーのみが知る。
// スライダーは自駒にぶつかるまで、打ちは自駒のないマス全部を生成する。
// 二歩(自分の歩同士)・行き所のない駒・強制成りはここで処理する。
std::vector<Move> generate_candidates(const OwnView& view);

// ---------------------------------------------------------------------------
// 設定
// ---------------------------------------------------------------------------

struct Config {
	int  particles      = 256;   // 粒子数の目標
	int  stage1Samples  = 24;    // stage1(静止探索)に使う粒子数
	int  stage2Samples  = 48;    // stage2(深い探索)に使う粒子数
	int  stage2TopK     = 12;    // stage2に進める候補数
	int  searchDepth    = 6;     // stage2の探索深さの上限(時間が尽きれば手前で打ち切る)
	int  budgetMs       = 2000;  // 1手の思考予算の既定値(goで上書き可)
	int  regenTries     = 4000;  // 粒子再生成のリプレイ試行上限
	double policyTemp   = 120.0; // 相手手サンプリングのsoftmax温度(centipawn)
	double policyEps    = 0.10;  // 相手手サンプリングの一様混合率
	// 反則の期待コスト(centipawn)。この競技は実質「反則予算10回の消耗戦」で、
	// 対局のほとんどが反則負けで終わる。反則1回は局面評価の数十cpではなく
	// 「持ち点の1/10 + 信念の崩壊」に相当するので、初版の100は桁で足りていなかった。
	//
	// 最適値は信念の質に依存する。段の向きのバグを直して信念が良くなったあとは、
	// 800まで上げると逆に悪化した(20局で 350 が 13勝7敗)。信念が悪いほど
	// 「とにかく安全な手」に倒す価値が上がる、という関係になっているらしい。
	// 150 とはほぼ互角(11勝9敗)なので、150〜400あたりは平ら。
	double foulBaseCp   = 350.0;
	// 反則累計1回ごとの追加コスト。10/(10-f) の急騰が別に掛かるので、
	// ここを厚くしすぎると逆効果(160 vs 60 は 4勝8敗で負け越し)。
	double foulStepCp   = 60.0;
	// 相手の反則累計にもコストを連動させる度合い。相手が反則負けに近い =
	// こちらが勝勢なので、勝ちを守るためにリスクをさらに嫌う。0で無効。
	double foulOppW     = 0.0;
	// p_legal のシュリンク強度(疑似観測数)。粒子は複製で相関しているので
	// 有効サンプル数は粒子数より小さく、素の頻度だと「全粒子で合法=反則確率0」と
	// 言い切ってしまう。弱い事前分布(平均 pLegalPriorMean)へ引き寄せる。0で無効。
	double pLegalPrior     = 0.0;
	double pLegalPriorMean = 0.90;
	// 相手の反則を誘う配置への加点(centipawn / 期待妨害手1つあたり)。0で無効。
	// ついたて将棋の勝敗は実質「反則予算10回の消耗戦」なので、相手が指したい手の
	// 経路や打ち場所を塞ぐこと自体に価値がある。相手はこちらの駒が見えないので、
	// この妨害は「取られるリスクを負わずに」効く(見えている駒は避けられてしまう)。
	double blockCp      = 0.0;
	int    blockSamples = 16;   // 妨害マップに使う粒子数
	// 相手手の方策モデル: 0 = 千里眼(着手後の静的評価のsoftmax。こちらの駒が
	// 見えている前提) / 1 = 非千里眼(相手はこちらの駒が見えないので、自分の駒だけで
	// 決まる素朴な指し手prior)。1は評価関数を呼ばないので桁違いに速く、
	// リプレイでも同じ方策が使える(従来リプレイは一様サンプリングだった)。
	int  oppPolicy      = 1;
	// 演繹層: 観測から論理的に確定する情報(自分の手が通った経路は空だった/
	// この手番で反則になった手は現局面で不正)を合成粒子に効かせる
	int  deduce         = 1;
	// 合成粒子の配置に駒種ごとの事前分布を使うか。0にすると初版と同じ
	// 「空きマスから一様ランダム(玉だけ相手陣寄り)」に戻る。
	// deduce とは別軸(deduce=0 でも事前分布は効くので、A/B では両方を切ること)。
	int  synthPrior     = 1;
	// 1手の思考予算のうち信念の同期・再生成に回す割合(%)。
	// 反則が勝敗を決めるので、探索の深さより信念の質に配分するほうが利くことがある。
	int  syncPct        = 40;
	// 粒子数が目標のこの割合(%)以上あれば再生成をまるごと省く。
	// 低いと時間は浮くが人口が痩せたまま(=p_legalの分解能と信念の多様性が落ちる)。
	int  regenFloorPct  = 50;
	uint64_t seed       = 20260827;
	int  logLevel       = 1;     // 0:silent 1:info 2:debug
};

// サイトのPieceRole文字列 <-> PieceType
PieceType role_from_site(const std::string& s);
std::string role_to_site(PieceType pt);

// USI表記ヘルパ
Square parse_usi_square(const std::string& s);       // "7g" -> Square(SQ_NBなら不正)
std::string usi_square(Square sq);
// 自分の駒情報を使ってUSI手をMoveへ(候補手と同じ表現)。不正ならMove::none()
Move parse_usi_move(const OwnView& view, const std::string& s);

} // namespace Tsuitate
} // namespace YaneuraOu

#endif // TSUITATE_ENGINE
#endif // TSUITATE_COMMON_H_INCLUDED
