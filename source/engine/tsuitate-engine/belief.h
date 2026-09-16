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
	// この粒子がどれだけ観測と厳密かを表す緩和レベル
	// (0=全制約を満たすリプレイ / 1,2=制約を外したリプレイ / 3=合成粒子)。
	// 信念全体の品質はこれを粒子集合から集計して決める(relax_level_of_set)。
	int                   relax = 0;
	// §2 SIR: 対数重み。cfg.sir=1 のときだけ動く(0なら常に0=等重み)。
	// 相手イベントの尤度(反則: 1−p_ok / 着手: 整合手の方策質量)を掛けていき、
	// sync の最後に平均1へ正規化する。新粒子(再生成・合成)は 0 = 平均重みで入る。
	double                logw = 0.0;

	Particle() { init(); }

	void init() {
		sts.clear();
		oppMoves.clear();
		synthetic = false;
		relax = 0;
		logw = 0.0;
		sts.emplace_back();
		pos.set_hirate(&sts.back());
	}

	// SFENから直接作る(合成粒子用)。失敗したらfalse。
	bool init_from_sfen(const std::string& sfen) {
		sts.clear();
		oppMoves.clear();
		synthetic = true;
		relax = 3;
		logw = 0.0;  // 合成 = 観測重みなし(平均重み)で入る
		sts.emplace_back();
		return !pos.set(sfen, &sts.back()).has_value();
	}

	// 手を進める。呼び出し側がこの局面での合法性を検査済みであること
	// (apply_event / replay_one はすべて直前に legal / 整合フィルタを通している)。
	void advance(Move m) {
		sts.emplace_back();
		pos.do_move(m, sts.back());
	}
	// 合法性を確かめてから進める。不正なら適用せず false。
	// 唯一の未検査経路だった clone_of(親の oppMoves 列をそのまま進める)が使う。
	// 不正な手を do_move に渡すと segfault する(docs/strengthening.md 9.5章)。
	bool try_advance(Move m) {
		if (!legal(m))
			return false;
		advance(m);
		return true;
	}

	// この粒子上でmが(通常将棋ルールで)合法か
	bool legal(Move m) const {
		return pos.pseudo_legal_s<true>(m) && pos.legal(m);
	}
};

using ParticlePtr = std::unique_ptr<Particle>;

// 異常(不正な手・親と一致しない複製など)の記録枠。プロセス全体で最初の 20 件
// だけ true を返す(大量に出ても原因調査には役立たず、出力を埋めるだけ)。
// 記録は他の診断と同じく `info string`(sync_cout)で出す。回数の集計は
// 呼び出し側(Belief::badAdvance_ / ThinkResult::badJobs)が持ち、対局・側ごとに
// 帰属できるようにする。
bool anomaly_report_slot();
// 「この局面で不正な手を進めようとした」の記録(where = 呼び出し元の名前)
void report_illegal_move(const Position& pos, Move m, const char* where);

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
	// deadline: この時刻を過ぎたら足りなくても打ち切る。
	void force_resynthesize(const OwnView& view, TimePoint deadline);

	const std::vector<ParticlePtr>& particles() const { return parts_; }
	size_t size() const { return parts_.size(); }
	// 診断表示用の整数レベル(0..3)。思考には relax_mean() のほうを使う。
	int    relaxLevel() const { return relaxLevel_; }
	double relaxMean() const { return relaxMean_; }
	size_t cursor() const { return cursor_; }
	// §2 SIR: この同期で観測(相手イベント)を効かせた直後の実効サンプル数
	// (リサンプリング前に測る。sir=0 なら常に粒子数と同じ)。診断用。
	double ess() const { return essLast_; }
	// この対局で「不正な手・親と一致しない複製」として捨てた回数(9.5章の診断)
	long long bad_advance() const { return badAdvance_; }
	// §2 SIR: 正規化した重み(合計 = 粒子数。等重みなら全要素1.0)。
	// think() の p_legal と評価粒子の選択が使う。sir=0 では全要素1.0。
	void normalized_weights(std::vector<double>& out) const;

private:
	// 1イベントを全粒子に適用する(相手手はサンプリング＋分岐)
	void apply_event(const GameHistory& hist, const HistEvent& ev);

	// 履歴リプレイによる粒子生成。成功したらnullptrでないParticleを返す。
	// relax: 緩和レベル(0=全制約 / 1=自反則と「王手なし」を無視 / 2=さらに「王手あり」も無視)
	// seed:  死んだ粒子の相手手列。先頭 (seed->size() - resample) 手をそのまま使い、
	//        残りだけ整合サンプリングし直す(部分若返り)。nullptrなら全手サンプリング。
	// rng:   乱数源。並列再生成ではワーカーごとに独立の PRNG を渡す
	//        (rng_ を複数スレッドで共有すると競合する)。逐次経路は rng_ を渡す。
	ParticlePtr replay_one(const GameHistory& hist, int relax, PRNG& rng,
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

	// 粒子集合から信念全体の緩和度を決める(粒子ごとの relax の平均)。
	//
	// 連続値にしているのは、think() の反則コスト割増がこれに比例するため。
	// 「非厳密な粒子が過半なら最悪レベル」のような階段にすると、
	// 50%の境界をまたいだだけで割増が 1.0倍 ⇔ 5.5倍 と飛び、
	// その境界付近を動かすA/Bが解釈できなくなる。
	double relax_mean() const;

	// 方策で1手選ぶ。excludeに入っている手は除く。
	// cfg_.oppPolicy で相手モデルを切り替える(0=千里眼評価softmax / 1=非千里眼prior)。
	// rng は replay_one と同じ理由で引数(並列時はワーカーの PRNG)。
	Move sample_policy(Particle& p, const std::vector<Move>& moves,
	                   const std::vector<Move>& exclude, PRNG& rng);

	// --- §2 SIR(cfg_.sir=1 のときだけ使う) ---
	// 相手の反則の尤度: P(反則 | 粒子) = 1 − p_ok。p_ok は「相手の意図
	// (相手視界での指したい手)のうち粒子上で合法な質量」で、探索の相手ノード
	// (dsearch の opp_node)と同じ量・同じ重み付け(softmax+ε一様)。
	double opp_foul_likelihood(const Particle& p) const;
	// 相手の着手の尤度: 粒子の合法手の方策質量のうち、観測(取られたマス・
	// 王手宣言)と整合する手が占める割合。consistent は consistent_opp_moves の出力。
	double opp_move_likelihood(const Particle& p, const std::vector<Move>& consistent) const;
	// 重みの正規化(平均→1)と ESS 計測、ESS < 粒子数/2 なら系統的リサンプリング。
	// sync の「全イベント適用後」に1回だけ呼ぶ(clone_of が cursor_ に依存するため、
	// イベント適用の途中では呼べない)。
	void weights_normalize_and_resample(const GameHistory& hist);

	// この手番でこれまでに反則になった自分の手(履歴の末尾から導出)。
	// 現局面に対する強い制約なので、合成粒子の棄却に使う。
	std::vector<Move> curFouls_;

	// 観測と矛盾して死んだ粒子の相手手列(部分若返りの種)
	void bury(const Particle& p);

	// 合成粒子: 履歴を使わず、既知の駒勘定(相手の持ち駒・盤上駒種は観測から
	// 一意に決まる)と現在の王手状態だけを満たす配置を直接サンプリングする。
	// リプレイが全滅したときの最終フォールバック。
	// rng は replay_one と同じ理由で引数(並列時はワーカーの PRNG)。
	ParticlePtr synthesize(const OwnView& view, PRNG& rng);

	// 合成粒子で target まで埋める(逐次・並列共通。sync の最終フォールバックと
	// force_resynthesize で共有)。keepGoing(粒子数, ミス数) はロック内で
	// 評価される継続判定。push は target を上限に再検査する。
	void synth_fill(const OwnView& view, size_t target,
	                const std::function<bool(size_t, int)>& keepGoing);

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
	double                   relaxMean_  = 0;  // 同上の連続値(反則コスト割増に使う)
	double                   essLast_    = 0;  // §2 SIR: 直近syncのESS(診断)
	long long                badAdvance_ = 0;  // 9.5章: 不正として捨てた粒子の数(対局内)
};

} // namespace Tsuitate
} // namespace YaneuraOu

#endif // TSUITATE_ENGINE
#endif
