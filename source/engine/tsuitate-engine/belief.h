// ついたて将棋エンジン: 信念状態(粒子フィルタ)
//
// 粒子 = 「観測履歴と矛盾しない完全局面」1つ。相手の駒配置の仮説。
// 自分側の駒は全粒子で共通(完全既知)。
//
// 更新則:
//   - 自分の正規手: その手が粒子上で合法で、取った駒・王手宣言が一致する粒子のみ残し、進める
//   - 自分の反則手: その手が粒子上で「不正」である粒子のみ残す(合法だった粒子は矛盾)
//   - 相手の着手: 粒子上の相手の合法手のうち観測(取られたマス・王手宣言)と整合する手を
//     方策(浅い評価のsoftmax)でサンプリングして進める。整合手がない粒子は死ぬ
//   - 相手の反則: 真の局面には制約を与えない(カウントのみ)
//
// 粒子が枯渇したら履歴リプレイで再生成する。制約は強い順に
//   捕獲・自手合法性 > 王手宣言(あり) > 王手宣言(なし) > 自分の反則
// で、再生成が間に合わない場合は弱い制約から緩和する。
#ifndef TSUITATE_BELIEF_H_INCLUDED
#define TSUITATE_BELIEF_H_INCLUDED

#include "tsuitate_common.h"

#if defined(TSUITATE_ENGINE)

namespace YaneuraOu {
namespace Tsuitate {

struct Particle {
	Position              pos;
	std::deque<StateInfo> sts;       // do_moveごとに1つ(先頭は初期局面用)
	std::vector<Move>     oppMoves;  // この粒子が選んだ相手手(リプレイ用)
	bool                  synthetic = false;  // 履歴リプレイ不能な合成粒子(緊急ビリーフ)

	Particle() { init(); }

	void init() {
		sts.clear();
		oppMoves.clear();
		synthetic = false;
		sts.emplace_back();
		pos.set_hirate(&sts.back());
	}

	// SFENから直接作る(合成粒子用)。失敗したらfalse。
	bool init_from_sfen(const std::string& sfen) {
		sts.clear();
		oppMoves.clear();
		synthetic = true;
		sts.emplace_back();
		return !pos.set(sfen, &sts.back()).has_value();
	}

	void advance(Move m) {
		sts.emplace_back();
		pos.do_move(m, sts.back());
	}

	// この粒子上でmが(通常将棋ルールで)合法か
	bool legal(Move m) const {
		return pos.pseudo_legal_s<true>(m) && pos.legal(m);
	}
};

using ParticlePtr = std::unique_ptr<Particle>;

class Belief {
public:
	void reset(Color us, const Config& cfg);

	// 履歴のうち王手宣言が確定したイベントを順に適用する。
	// 適用後、粒子が目標数を下回っていれば時間の許す範囲で再生成する。
	// view は合成粒子(最終フォールバック)の駒勘定に使う。
	// deadline: 再生成に使ってよい時刻(now() + ms)。
	void sync(const GameHistory& hist, const OwnView& view, TimePoint deadline);

	// 信念の破産処理: 現在の粒子集合を破棄し、合成粒子だけで作り直す。
	// 「全候補手が全粒子で不正」のような、信念が確実に間違っている状況で呼ぶ
	// (詰みならサーバーが終局させるので、合法手は必ず存在する)。
	void force_resynthesize(const OwnView& view);

	const std::vector<ParticlePtr>& particles() const { return parts_; }
	size_t size() const { return parts_.size(); }
	int    relaxLevel() const { return relaxLevel_; }
	size_t cursor() const { return cursor_; }

private:
	// 1イベントを全粒子に適用する(相手手はサンプリング＋分岐)
	void apply_event(const GameHistory& hist, const HistEvent& ev);

	// 履歴リプレイによる粒子生成。成功したらnullptrでないParticleを返す。
	// relax: 緩和レベル(0=全制約 / 1=自反則と「王手なし」を無視 / 2=さらに「王手あり」も無視)
	// seed:  死んだ粒子の相手手列。先頭 (seed->size() - resample) 手をそのまま使い、
	//        残りだけ整合サンプリングし直す(部分若返り)。nullptrなら全手サンプリング。
	ParticlePtr replay_one(const GameHistory& hist, int relax,
	                       const std::vector<Move>* seed = nullptr,
	                       size_t resample = 0,
	                       size_t* failIdx = nullptr);

	// 親粒子のoppMoves列を使った複製(整合性チェックなしの高速リプレイ)
	ParticlePtr clone_of(const GameHistory& hist, const Particle& src);

	// 相手手のうち観測と整合するものを列挙
	static void consistent_opp_moves(const Particle& p, const HistEvent& ev,
	                                 std::vector<Move>& out, int relax);

	// 特定の1手だけの整合判定(全合法手の列挙を伴わない高速版)
	static bool opp_move_consistent(const Particle& p, const HistEvent& ev,
	                                Move m, int relax);

	// 方策で1手選ぶ。excludeに入っている手は除く。
	// cfg_.oppPolicy で相手モデルを切り替える(0=千里眼評価softmax / 1=非千里眼prior)。
	Move sample_policy(Particle& p, const std::vector<Move>& moves,
	                   const std::vector<Move>& exclude);

	// この手番でこれまでに反則になった自分の手(履歴の末尾から導出)。
	// 現局面に対する強い制約なので、合成粒子の棄却に使う。
	std::vector<Move> curFouls_;

	// 観測と矛盾して死んだ粒子の相手手列(部分若返りの種)
	void bury(const Particle& p);

	// 合成粒子: 履歴を使わず、既知の駒勘定(相手の持ち駒・盤上駒種は観測から
	// 一意に決まる)と現在の王手状態だけを満たす配置を直接サンプリングする。
	// リプレイが全滅したときの最終フォールバック。
	ParticlePtr synthesize(const OwnView& view);

	Color                    us_ = BLACK;
	Config                   cfg_;
	PRNG                     rng_{20260827};
	std::vector<ParticlePtr> parts_;
	std::vector<std::vector<Move>> graveyard_;  // 死んだ粒子の相手手列(最大256)
	size_t                   buryIdx_ = 0;      // 墓場リングバッファの書き込み位置
	long long                failHist_[16] = {};  // 再生成失敗位置(cursorからの距離)の分布
	long long                failKind_[4]  = {};  // 再生成失敗イベント種別の分布
	size_t                   cursor_     = 0;  // histのうち適用済みイベント数
	int                      relaxLevel_ = 0;  // 現在の粒子群の緩和レベル(観測指標)
};

} // namespace Tsuitate
} // namespace YaneuraOu

#endif // TSUITATE_ENGINE
#endif
