// ついたて将棋エンジン: 思考部の実装
#include "think.h"
#include "dsearch.h"

#if defined(TSUITATE_ENGINE)

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>

#include "../../evaluate.h"
#include "../../bitboard.h"

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

// 相手の反則を誘うマスの重み(妨害マップ)。
//
// ついたて将棋の勝敗は実質「反則予算10回の消耗戦」なので、相手に反則をさせること
// 自体に価値がある。相手はこちらの駒が見えないから、こちらの駒が
//   - スライダーの**通過マス**にいる    → 経路封鎖で反則
//   - 打ちたいマスにいる                → 打ちマス占有で反則
// になる。着地マスにいるだけなら「取られる」(合法手)ので数えない。
//
// 粒子上の相手の駒・持ち駒から「相手が(こちらの駒を無視して)指したい手」を
// 数え上げ、そのうち妨害になるマスに1票ずつ入れて、指したい手の総数で割る。
// 出力は「相手の次の1手がそのマスで反則になる確率」の粒子平均。
void block_map(const std::vector<ParticlePtr>& parts, Color us, size_t k,
               const Config& cfg, double out[SQ_NB]) {
	for (auto sq : SQ)
		out[sq] = 0.0;
	k = std::min(k, parts.size());
	if (k == 0)
		return;  // 粒子なし、または blockSamples=0(0除算でマップ全体がNaNになる)
	const Color opp = ~us;
	double votes[SQ_NB];
	std::vector<OppIntent> intents;
	for (size_t t = 0; t < k; ++t) {
		const Position& pos = parts[t * parts.size() / k]->pos;
		// 相手の「指したい手」は探索の相手ノード(dsearch)・信念の方策と
		// **同じ生成器**から取る。ここに独自の列挙を書くと、成り変種や
		// 行き所のない駒の扱いが食い違って別の相手モデルになる
		// (実際、以前はこの関数だけ成り変種と piece_can_stay を落としていた)。
		// inCheck は「**相手が**王手されているか」。粒子は常にこちらの手番なので、
		// 相手が王手されていることは定義上ありえない(ありえたら相手の玉を
		// 取れる不正な局面)。pos.in_check() はこちら側の王手状態を返すので、
		// ここに渡すのは誤り。妨害マップが見ているのは「次の相手の手番に
		// 何を指したいか」であり、そのときの王手状態は予測できないので false。
		enumerate_opp_intents(pos, opp, intents, /*inCheck=*/false, cfg);
		for (auto sq : SQ)
			votes[sq] = 0.0;
		double total = 0.0;  // 相手の「指したい手」の総数(正規化用)

		for (const auto& it : intents) {
			const Move m = it.m;
			if (m.is_drop()) {
				// 打ちたいマスにこちらの駒があれば、打ちマス占有で反則になる
				total += 1.0;
				votes[m.to_sq()] += 1.0;
				continue;
			}
			// **成り変種は数えない。** enumerate_opp_intents は同じ from→to に
			// 成りと不成の2つの意図を出すが、妨害の観点ではどちらも
			// 「その経路を通ろうとする1つの試み」でしかなく、経路は完全に同じ。
			// 両方数えると、成れる手だけが打ちに対して2倍の重みを持ち、
			// マップの正規化が静かにずれる(= 過去に較正した blockcp の値が
			// 別の意味になる)。強制成りの手は不成の変種が生成されないので、
			// 「不成が存在しない成り」だけは数える必要がある。
			if (m.is_promote()
			    && piece_can_stay(opp, type_of(pos.piece_on(m.from_sq())), m.to_sq()))
				continue;  // 同じ from→to の不成が別途あるので、そちらで数える
			total += 1.0;
			// 妨害できるのはスライダーの通過マスだけ(桂は飛ぶ、1マス駒は経路がない)。
			// 着地マスにいるだけなら「取られる」= 合法手なので数えない。
			Bitboard mid = between_bb(m.from_sq(), m.to_sq());
			while (mid)
				votes[mid.pop()] += 1.0;
		}

		if (total <= 0.0)
			continue;
		const double inv = 1.0 / total;
		for (auto sq : SQ)
			out[sq] += votes[sq] * inv;
	}
	const double invK = 1.0 / double(k);
	for (auto sq : SQ)
		out[sq] *= invK;
}

} // namespace

ThinkResult Thinker::think(const OwnView& view, Belief& belief, const GameHistory& hist,
                           const std::vector<Move>& foulTried, int budgetMs,
                           const Config& cfg, PRNG& rng) {
	const TimePoint t0       = now();
	const TimePoint deadline = t0 + budgetMs;
	ThinkResult res;

	// 1) 信念の同期(再生成には予算の syncPct% まで使う。-1=auto の解決込み)
	const int syncPct = resolved_sync_pct(cfg);
	belief.sync(hist, view, t0 + budgetMs * syncPct / 100);
	res.syncMs     = now() - t0;
	res.nParticles = belief.size();
	res.relaxLevel = belief.relaxLevel();
	res.ess        = belief.ess();

	// 2) 候補手(この手番で反則になった手は除外)
	std::vector<Move> cands;
	for (Move m : generate_candidates(view))
		if (std::find(foulTried.begin(), foulTried.end(), m) == foulTried.end())
			cands.push_back(m);
	if (cands.empty())
		return res;  // 投了

	const auto& parts = belief.particles();
	const size_t N    = parts.size();

	// 粒子ゼロ: ヒューリスティックで指す(それでも投了よりまし)。
	// 信念の破産処理(下)が作り直しに失敗した直後にも使う。
	auto heuristic_pick = [&]() {
		double best = -1e18;
		for (Move m : cands) {
			double s = fallback_score(view, m, rng);
			if (s > best) { best = s; res.best = m; }
		}
		res.elapsedMs = now() - t0;
		return res;
	};
	if (N == 0)
		return heuristic_pick();

	// 3) 合法率と「合法な粒子のindex」を全候補について求める
	size_t M = cands.size();
	std::vector<std::vector<uint32_t>> legalIdx(M);
	auto scan_legality = [&]() {
		for (auto& v : legalIdx)
			v.clear();
		for (size_t j = 0; j < belief.particles().size(); ++j)
			for (size_t i = 0; i < M; ++i)
				if (belief.particles()[j]->legal(cands[i]))
					legalIdx[i].push_back(uint32_t(j));
	};
	scan_legality();

	// §2 SIR: 粒子の正規化重み(合計 = 粒子数)。sir=0 では全要素1.0で、
	// 以下の重み付き式(p_legal・pick_particles)は従来の等重み式と厳密に一致する。
	// 粒子集合が入れ替わったら(破産処理後)必ず取り直すこと。
	std::vector<double> wts;
	belief.normalized_weights(wts);

	// 信念の破産検出: 全候補が全粒子で不正 = 信念が確実に間違っている
	// (真の局面に合法手がなければサーバーが終局させているはず)。
	// 粒子を捨てて合成粒子で作り直す。ここで反則を重ねても情報はゼロ
	// (「全滅の予測どおり」なので粒子が1つも死なない)なので、続行は最悪手。
	{
		size_t maxLegal = 0;
		for (size_t i = 0; i < M; ++i)
			maxLegal = std::max(maxLegal, legalIdx[i].size());
		if (maxLegal == 0) {
			// sync が予算の syncPct% を使ったあと。作り直しにも同じだけ上限を与える。
			// sync は締め切りを最大100ms超過しうるので、ここで既に過去の時刻に
			// なっていることがある。最低限の時間は必ず与える(でないと1粒子も
			// 作らずに空の信念で先へ進んでしまう)。
			TimePoint reDeadline = std::min(now() + budgetMs * syncPct / 100,
			                                deadline - 50);
			belief.force_resynthesize(view, std::max(reDeadline, now() + 20));
			res.nParticles = belief.size();
			res.relaxLevel = belief.relaxLevel();
			// 作り直しにも失敗した(合成粒子が1つも作れない)。
			// このまま進むと全候補の評価が反則コストで並んで選択が無意味になるので、
			// 「見えない駒に当たりにくい手」のヒューリスティックに委ねる。
			if (belief.particles().empty())
				return heuristic_pick();
			scan_legality();
			belief.normalized_weights(wts);
		}
	}
	const size_t NP = belief.particles().size() ? belief.particles().size() : 1;

	// 確実な反則(どの粒子でも不正)は候補から外す。指しても情報が得られない
	// (「予測どおりの反則」は粒子を1つも殺さない)うえに1回を捨てるだけ。
	// 悪い局面では foulCp が「悪いが合法な手」の評価を上回って確実反則が
	// 選ばれてしまう、という実対戦で観測されたパターンの直接の対策。
	{
		std::vector<Move>                  keptC;
		std::vector<std::vector<uint32_t>> keptL;
		for (size_t i = 0; i < M; ++i)
			if (!legalIdx[i].empty()) {
				keptC.push_back(cands[i]);
				keptL.push_back(std::move(legalIdx[i]));
			}
		if (!keptC.empty()) {
			cands    = std::move(keptC);
			legalIdx = std::move(keptL);
			M        = cands.size();
		}
		// keptCが空 = 破産処理後もなお全滅。この場合は元の候補のまま進み、
		// 下の「合法粒子ゼロでも評価」経路(ヒューリスティック相当)に任せる。
	}

	// 合法率。cfg.pLegalPrior > 0 なら弱い事前分布へシュリンクする
	// (粒子は複製で相関しているので、素の頻度は自信過剰になりやすい)。
	const double prA = cfg.pLegalPrior * cfg.pLegalPriorMean;
	const double prB = cfg.pLegalPrior * (1.0 - cfg.pLegalPriorMean);
	auto p_legal = [&](size_t i) {
		// §2 SIR: 重み付き合法率。等重み(sir=0)では mass = 粒子数なので従来と同一。
		double mass;
		if (cfg.sir) {
			mass = 0.0;
			for (uint32_t j : legalIdx[i])
				mass += wts[j];
		} else {
			mass = double(legalIdx[i].size());
		}
		return (mass + prA) / (double(NP) + prA + prB);
	};

	// 反則コスト(centipawn)。累計10回で反則負けなので、残り予算が減るほど急騰させる。
	// 値付けは foul_value に一本化してある(相手の反則の利得も同じ式の鏡像を使う)。
	double foulCp = -foul_value(cfg.foulBaseCp, cfg.foulStepCp, view.ourFouls);
	// 相手が反則負けに近い = こちらが勝勢。勝ちを守るためリスクをさらに嫌う。
	foulCp *= 1.0 + cfg.foulOppW * view.oppFouls;
	// 信念の品質が悪い(緩和粒子・粒子不足)ときはp_legalの推定が信用できないので、
	// リスクをさらに重く見る。反則→粒子死→さらに反則、のスパイラルを断つ。
	// 緩和度は粒子集合の平均(連続値)。整数レベルで掛けると、
	// 粒子の入れ替わりで境界をまたいだだけで割増が飛んでしまう。
	foulCp *= 1.0 + 1.5 * belief.relaxMean();
	if (NP * 4 < size_t(cfg.particles))
		foulCp *= 2.0;

	// 相手の反則1回のこちらから見た価値。探索の相手ノード(oppModel)が
	// 「相手がこの局面で何回反則しそうか」に掛けて使う。自分の反則コストと
	// 同じ foul_value を使うので、値付けが2か所に分かれない。
	const double foulGain =
	    cfg.foulGainScale * foul_value(cfg.foulBaseCp, cfg.foulStepCp, view.oppFouls);

	// §3.2: 探索コンテキスト(置換表 + history)。ワーカースレッドごとに1つ。
	// begin_think(初回の16MB割り当て・世代進め・history半減)は呼び出し
	// スレッドでまとめてやらず、各ワーカーが最初に ctx を要求した時点で行う
	// (全ワーカー分のゼロ初期化と first-touch を1スレッドに直列に乗せない)。
	const int nWorkers = effective_threads(cfg);
	++thinkStamp_;
	if (cfg.tt)
		while (int(ctx_.size()) < nWorkers)
			ctx_.push_back(std::make_unique<SearchContext>());
	auto ctx_for = [&](int w) -> SearchContext* {
		if (!cfg.tt)
			return nullptr;
		SearchContext* c = ctx_[size_t(w)].get();
		// コンテキストは1リージョン内では担当ワーカーだけが触り、リージョン間は
		// run_workers の join が順序づけるので、素の比較で足りる(競合しない)。
		if (c->stamp != thinkStamp_) {
			c->stamp = thinkStamp_;
			c->begin_think(view.oppFouls);
		}
		return c;
	};

	// stage1 の時間縮退ポリシー(候補1つあたりのサンプル数上限)。**定義はここ1つ**:
	// 逐次版の k1 クランプ・並列版のパス開始時の k1・parallel_eval のジョブ間引きの
	// 3か所すべてがこれを参照する。閾値を再調整するとき(実対局予算〜2.4sへの
	// 適合は 3.4章の保留事項)に一部だけ更新されると、時間切迫の局面でだけ
	// threads=1 と threads>1 が別の規則で縮退し、対のA/Bを静かに汚すため。
	//
	// §9 stage1Pct > 0 のときは、1ジョブあたりの実測 wall 時間(jobMs1_、対局内で
	// 学習)から「予算の stage1Pct% に収まる粒子数」を上限に重ねる(下限2)。
	// 候補が数百手ある中盤では stage1 だけで予算の半分以上を食い、stage2 に
	// 一度も入れない決定が3割を占めていた(docs/strengthening.md 9章)。
	// 初手(未観測)は従来どおり stage1Samples。
	size_t k1Budget = size_t(cfg.stage1Samples);
	if (cfg.stage1Pct > 0 && jobMs1_ > 0.0 && M > 0) {
		const double maxJobs = double(budgetMs) * double(cfg.stage1Pct) / 100.0 / jobMs1_;
		k1Budget = size_t(std::clamp(maxJobs / double(M), 2.0, double(cfg.stage1Samples)));
	}
	auto stage1_cap = [&](TimePoint t) -> size_t {
		size_t cap = std::min(size_t(cfg.stage1Samples), k1Budget);  // 設定値が4未満ならそちらが上限
		if (t > deadline - 100)
			cap = std::min<size_t>(cap, 1);
		else if (t > t0 + budgetMs * 7 / 10)
			cap = std::min<size_t>(cap, 4);
		// §9 stage1Pct の時間門: 配分を過ぎたら「まだ1サンプルもない候補」だけ
		// (コストモデルの読み違い(局面の複雑さの急変)に対する保険)
		if (cfg.stage1Pct > 0 && t > t0 + budgetMs * cfg.stage1Pct / 100)
			cap = std::min<size_t>(cap, 1);
		return cap;
	};

	// stage2 の締め切り(パスの破棄判定)。従来は固定で締め切りの 50ms 手前。
	// §9 passGate では予算の10%(10〜50ms)にする: 200ms のアリーナ予算では
	// 固定50msが予算の25%を占めて深さ4のパスが構造的に入らない一方、
	// 実対局の予算(≥300ms、典型2.4s)では従来と同じ50msに収束する。
	// 1ジョブの上限は nodesLimit2 で抑えてあるので、超過は高々1ジョブぶん。
	const TimePoint s2Margin   = cfg.passGate ? std::clamp<TimePoint>(budgetMs / 10, 10, 50) : 50;
	const TimePoint s2Deadline = deadline - s2Margin;

	// (候補手, 粒子) 1ジョブの評価。並列(parallel_eval)・逐次(stage1/stage2)の
	// **すべてのループがこれを呼ぶ**。二重実装だと DSearch のフィールド追加や
	// squash 規則の変更が片側にしか入らず、A/Bの対照(threads 1)と配備(threads>1)の
	// 評価が乖離する(実際、ds.ctx の配線は3か所に別々に書く羽目になっていた)。
	// depth==0 は stage1(qsearch / oppReplyKStage1>0 なら深さ1の探索)。
	// stage1 を千里眼qsearchのままにすると「進めた駒は必ず取られる」序列で
	// 上位が決まり、相手モデルが評価したい前進手が stage2 に届かない(Config参照)。
	// truncOut: ノード上限に当たって値が汚れたジョブ数(§9 診断。stage2 だけ集計する)
	auto eval_one = [&](Position& pos, Move m, int depth, uint64_t nodesLimit,
	                    SearchContext* sctx, uint64_t& nodesOut, uint64_t& truncOut) -> double {
		StateInfo st;
		DSearch   ds;
		ds.nodesLimit = nodesLimit;
		ds.cfg        = &cfg;
		ds.us         = view.us;
		ds.foulGain   = foulGain;
		ds.ctx        = sctx;
		// 保険: legalIdx で合法な粒子だけが来るはずだが、不正な手を do_move に
		// 渡すと segfault する(9.5章)。ここで弾いて記録し、値は静的評価で代用する
		if (!(pos.pseudo_legal_s<true>(m) && pos.legal(m))) {
			static std::atomic<int> reported{0};
			if (reported.fetch_add(1) < 20)
				fprintf(stderr, "tsuitate: BAD JOB move=%s depth=%d sfen=%s\n",
				        to_usi_string(m).c_str(), depth, pos.sfen().c_str());
			return squash_cp(Value(0));
		}
		pos.do_move(m, st);
		Value v;
		if (depth == 0) {
			ds.oppK1 = cfg.oppReplyKStage1;
			if (cfg.oppModel > 0 && cfg.oppReplyKStage1 > 0)
				v = -ds.search(pos, 1, -VALUE_INFINITE, VALUE_INFINITE, 1);
			else
				v = -ds.qsearch(pos, -VALUE_INFINITE, VALUE_INFINITE, 1);
		} else {
			v = -ds.search(pos, depth - 1, -VALUE_INFINITE, VALUE_INFINITE, 1);
		}
		pos.undo_move(m);
		nodesOut += ds.nodes;
		if (ds.nodes > ds.nodesLimit)
			++truncOut;
		// 相手ノードが確率混合を返したときは、その値は既に squash 済みの空間に
		// いるので二重に squash しない(詰みが通常評価の上限に潰れる)。
		return ds.rootMixed ? double(v) : squash_cp(v);
	};

	// 妨害マップ(相手の反則を誘う配置への加点)。合法だったときにだけ効くので
	// p_legal を掛ける。移動元を空けるぶんは差し引く。
	//
	// この加点を落としてよいのは、探索の相手ノードが同じ価値を
	// **候補手の序列を決める段(stage1)でも**数えているときだけ。3条件が要る:
	//   oppModel > 0        … 相手ノードを modeling している
	//   foulGainScale > 0   … その相手ノードが反則の価値を実際に数えている
	//   oppReplyKStage1 > 0 … stage1 でも相手ノードを展開している
	// 3つ目が要るのは、上位 stage2TopK 手を切るのが stage1 だから。
	// stage1 が千里眼qsearchのままだと、妨害の価値はstage1のどこにも入らず、
	// 妨害が狙いの候補手は stage2 に到達する前に切り落とされる
	// (= blockcp のA/Bが丸ごと無意味になる)。
	//
	// なお oppModel == 1 では ply==1 の相手ノードしか modeling しないので、
	// それ以降の相手の手番に対する妨害の価値はどちらの経路でも数えていない。
	const bool oppNodeCountsFouls =
	    cfg.oppModel > 0 && cfg.foulGainScale > 0.0 && cfg.oppReplyKStage1 > 0;
	std::vector<double> blockBonus(M, 0.0);
	if (cfg.blockCp > 0.0 && !oppNodeCountsFouls && NP > 0) {
		double map[SQ_NB];
		block_map(parts, view.us, size_t(cfg.blockSamples), cfg, map);
		for (size_t i = 0; i < M; ++i) {
			Move m = cands[i];
			double d = map[m.to_sq()];
			if (!m.is_drop())
				d -= map[m.from_sq()];
			blockBonus[i] = cfg.blockCp * d;
		}
	}

	auto combined = [&](size_t i, double meanCp) {
		double pl = p_legal(i);
		return pl * (meanCp + blockBonus[i]) + (1.0 - pl) * foulCp;
	};

	// 均等間隔で粒子を選ぶ(粒子は生成順に相関があるため)。
	// §2 SIR では重みに比例した系統抽出になる: 選ばれた粒子の等重み平均が
	// 重み付き期待値の近似になるので、評価ループ側は変更なしで済む。
	// 重い粒子は複数回選ばれうる(それが正しい重み付け)。sir=0 の経路は従来と同一。
	auto pick_particles = [&](const std::vector<uint32_t>& idx, size_t k) {
		std::vector<uint32_t> out;
		if (idx.empty())
			return out;
		k = std::min(k, idx.size());
		if (cfg.sir) {
			double total = 0;
			for (uint32_t j : idx)
				total += wts[j];
			if (total > 0) {
				size_t i = 0;
				double cum = wts[idx[0]];
				for (size_t t = 0; t < k; ++t) {
					const double pos = (double(t) + 0.5) / double(k) * total;
					while (cum < pos && i + 1 < idx.size()) {
						++i;
						cum += wts[idx[i]];
					}
					out.push_back(idx[i]);
				}
				return out;
			}
			// total==0 は正規化の作りからありえないが、0除算だけは構造的に防ぐ
		}
		for (size_t t = 0; t < k; ++t)
			out.push_back(idx[t * idx.size() / k]);
		return out;
	};

	// --- 並列評価(cfg.threads > 1 のときだけ使う) ---------------------------
	//
	// ジョブ = (候補手, 粒子)。同じ粒子の Position に複数スレッドが do_move すると
	// 競合するので、**粒子側でグループ化**し、1つの粒子のジョブは必ず同じスレッドが
	// 連続で処理する(スレッドは粒子グループを atomic カウンタで動的に取る)。
	// ジョブごとの評価値は配列に保存してから固定順(粒子昇順→グループ内の候補順)で
	// 還元するので、浮動小数の和の順序はスレッドスケジュールに依存しない。
	//
	// ※ この「どのスレッドがどの粒子を処理しても合計は同じ」が成り立つのは
	//   **時間ベースの縮退が働かない範囲**での話。stage1 のジョブ間引き
	//   (stage1_cap)や stage2 の締め切り破棄は now() と完了順に依存するので、
	//   時間切迫時はどのサンプル集合が評価されるか自体が実行ごとに変わる
	//   (逐次版も k1 縮退が時間依存なので同格。決定的なのは
	//   「同じジョブ集合が完了したなら同じ値」まで)。
	//
	// candIdx: 評価する候補のindex(cands への添字)
	// sels   : candIdx と同じ長さ。候補ごとの評価粒子(pick_particles の出力)
	// depth  : 0 = stage1(qsearch / oppReplyKStage1 の深さ1探索) / >0 = stage2 の深さ
	// abortable: true なら deadline-50 で中断してパスを破棄する(stage2 の意味論)。
	//            false(stage1)では中断しないが、deadline-100 を過ぎたら
	//            「まだ1サンプルもない候補」以外のジョブを飛ばす
	//            (逐次版の「締め切りが迫ったら k1=1 に絞る」に対応する縮退)
	// 返り値: aborted(パス破棄)。outSum/outCnt は candIdx と同じ長さ
	auto parallel_eval = [&](const std::vector<size_t>& candIdx,
	                         const std::vector<std::vector<uint32_t>>& sels,
	                         int depth, uint64_t nodesLimit, bool abortable,
	                         std::vector<double>& outSum, std::vector<size_t>& outCnt,
	                         uint64_t& outNodes, uint64_t& outTrunc) -> bool {
		const size_t C = candIdx.size();
		// 粒子ごとのジョブリスト(値 = candIdx への添字)
		std::vector<std::vector<uint32_t>> perPart(NP);
		size_t nJobs = 0;
		for (size_t c = 0; c < C; ++c)
			for (uint32_t j : sels[c]) {
				perPart[j].push_back(uint32_t(c));
				++nJobs;
			}
		// 非空の粒子グループと、固定順還元のためのジョブ開始オフセット
		std::vector<uint32_t> groups;
		std::vector<size_t>   offset;
		size_t                cum = 0;
		for (size_t j = 0; j < NP; ++j)
			if (!perPart[j].empty()) {
				groups.push_back(uint32_t(j));
				offset.push_back(cum);
				cum += perPart[j].size();
			}
		std::vector<double>  vals(nJobs, 0.0);
		std::vector<uint8_t> done(nJobs, 0);
		std::atomic<size_t>  nextGroup{0};
		std::atomic<bool>    aborted{false};
		std::atomic<uint64_t> nodesTotal{0};
		std::atomic<uint64_t> truncTotal{0};
		// stage1 の縮退用: 候補ごとの完了サンプル数(時間切迫時の判定にだけ使う)。
		// stage2(abortable)は読まないので確保も更新もしない(隣接atomicへの
		// fetch_add は深い探索パスで無意味な偽共有トラフィックになる)。
		std::vector<std::atomic<int>> cnt1(abortable ? 0 : C);
		for (auto& a : cnt1)
			a.store(0, std::memory_order_relaxed);

		run_workers(nWorkers, [&](int w) {
			SearchContext* sctx = ctx_for(w);
			uint64_t myNodes = 0, myTrunc = 0;
			while (true) {
				if (aborted.load(std::memory_order_relaxed))
					break;
				size_t g = nextGroup.fetch_add(1, std::memory_order_relaxed);
				if (g >= groups.size())
					break;
				const uint32_t j    = groups[g];
				Position&      pos  = parts[j]->pos;
				const auto&    jobs = perPart[j];
				for (size_t q = 0; q < jobs.size(); ++q) {
					const uint32_t c = jobs[q];
					if (abortable) {
						if (now() > s2Deadline) {
							aborted.store(true, std::memory_order_relaxed);
							break;
						}
					} else {
						// stage1 の縮退。逐次版はループ内で k1 自体を絞るが、
						// 並列版はワークロードをパス開始時に固定するので、
						// 実行時にジョブを間引いて同じ縮退を実現する
						// (でないと時間切迫後も全サンプルを回し続け、
						// 鋭い局面で stage2 の時間窓を食い潰す)。
						// 上限の定義は stage1_cap ただ1つ(逐次版と共有)。
						const int have = cnt1[c].load(std::memory_order_relaxed);
						if (have > 0 && size_t(have) >= stage1_cap(now()))
							continue;
					}
					vals[offset[g] + q] =
					    eval_one(pos, cands[candIdx[c]], depth, nodesLimit, sctx, myNodes, myTrunc);
					done[offset[g] + q] = 1;
					if (!abortable)
						cnt1[c].fetch_add(1, std::memory_order_relaxed);
				}
			}
			nodesTotal.fetch_add(myNodes, std::memory_order_relaxed);
			truncTotal.fetch_add(myTrunc, std::memory_order_relaxed);
		});
		outNodes += nodesTotal.load();
		outTrunc += truncTotal.load();
		// 固定順の還元(粒子昇順 → グループ内の候補順)。破棄(abort)時も
		// outCnt は埋める: 完了したジョブ数はコストモデルの観測に使う
		// (破棄されたパスからも「この深さは高い」を学ばないと、推定が固着する)
		outSum.assign(C, 0.0);
		outCnt.assign(C, 0);
		if (abortable && aborted.load()) {
			for (size_t g = 0; g < groups.size(); ++g) {
				const auto& jobs = perPart[groups[g]];
				for (size_t q = 0; q < jobs.size(); ++q)
					if (done[offset[g] + q])
						outCnt[jobs[q]]++;
			}
			return true;
		}
		for (size_t g = 0; g < groups.size(); ++g) {
			const auto& jobs = perPart[groups[g]];
			for (size_t q = 0; q < jobs.size(); ++q)
				if (done[offset[g] + q]) {
					outSum[jobs[q]] += vals[offset[g] + q];
					outCnt[jobs[q]]++;
				}
		}
		return false;
	};

	const TimePoint tStage1 = now();
	// 4) stage1: 全候補を静止探索で粗く評価。
	// 締め切りが迫ったらサンプル数を段階的に絞る(全候補に必ず何らかの値を付ける)
	std::vector<double> mean1(M, 0.0), comb1(M);
	if (nWorkers > 1) {
		// 並列版はワークロードをパス開始時に固定する(その後の時間切迫は
		// parallel_eval 内のジョブ間引きが同じ stage1_cap で縮退させる)
		size_t k1 = stage1_cap(now());
		std::vector<size_t>                candIdx(M);
		std::vector<std::vector<uint32_t>> sels(M);
		for (size_t i = 0; i < M; ++i) {
			candIdx[i] = i;
			sels[i]    = pick_particles(legalIdx[i], k1);
		}
		std::vector<double> sum;
		std::vector<size_t> cnt;
		uint64_t            trunc1 = 0;  // stage1 の打ち切りは集計しない
		parallel_eval(candIdx, sels, /*depth=*/0, /*nodesLimit=*/20000,
		              /*abortable=*/false, sum, cnt, res.nodes, trunc1);
		for (size_t i = 0; i < M; ++i) {
			mean1[i] = cnt[i] ? sum[i] / double(cnt[i]) : 0.0;
			comb1[i] = combined(i, mean1[i]);
			res.jobs1 += cnt[i];
		}
	} else {
		// 逐次版(threads 1 の対照経路。並列版と本体を統一しないのは、
		// (a) 候補ごとに now() を見て k1 を絞る縮退の意味論が並列と異なる、
		// (b) 浮動小数の加算順が変わると対照側の挙動が1ulpでも動くため。
		// ブレースで囲ってあるのは、`} else` + 裸の for だと後から文を足したとき
		// 分岐の外に置いてしまう編集事故が起きやすいため)
		uint64_t trunc1 = 0;  // stage1 の打ち切りは集計しない
		for (size_t i = 0; i < M; ++i) {
			auto sel = pick_particles(legalIdx[i], stage1_cap(now()));
			double sum = 0;
			for (uint32_t j : sel)
				sum += eval_one(parts[j]->pos, cands[i], /*depth=*/0,
				                /*nodesLimit=*/20000, ctx_for(0), res.nodes, trunc1);
			mean1[i] = sel.empty() ? 0.0 : sum / double(sel.size());
			comb1[i] = combined(i, mean1[i]);
			res.jobs1 += sel.size();
		}
	}

	res.stage1Ms = now() - tStage1;
	res.cands    = M;
	// コストモデルの更新(stage1)。ジョブ数が少なすぎる観測は分解能(1ms)の
	// ノイズが大きいので捨てる。stage1Pct=0 でも学習だけはしておく(害はない)
	// この決定の局面の「複雑さ」= stage1 の実測単価 / その EMA。qsearch の木の
	// 大きさは同じ局面の深い探索の木の大きさと相関するので、stage2 の推定単価を
	// これで補正する(EMA だけでは中盤の急変に追いつかず、3倍読み違えていた)。
	double complexity = 1.0;
	if (res.jobs1 >= 64 && res.stage1Ms >= 2) {
		const double x = double(res.stage1Ms) / double(res.jobs1);
		if (jobMs1_ > 0.0)
			complexity = std::clamp(x / jobMs1_, 0.5, 4.0);
		jobMs1_ = ema_update(jobMs1_, x);
	}
	const TimePoint tStage2 = now();
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
	// §9 stage2 スケジューリング。既定(passGate 0 / halving 0 / depthStep 2)では
	// 従来と同一: 予算の60%を過ぎていたら次のパスを始めず、毎パス同じ候補集合を
	// stage2Samples 粒子で読み直す。
	const int      step        = std::clamp(cfg.depthStep, 1, 2);
	const uint64_t nodesLimit2 = uint64_t(std::max(1, cfg.nodesLimit2));
	TimePoint      lastPassMs  = 0;  // 直前に完走したパスの所要時間
	uint64_t       lastPassJobs = 0; // そのジョブ数
	// この深さの1ジョブあたり wall 時間の推定(ms)。実測(EMA)があればそれ、
	// なければ直前のパスの単価 × passGrowth で外挿、どちらも無ければ 0(=予測不能)。
	// いずれも局面の複雑さで補正する。
	auto job_ms_est = [&](int d) -> double {
		double base = 0.0;
		if (d < Thinker::JOB_MS_SLOTS && jobMs2_[d] > 0.0)
			base = jobMs2_[d];
		else if (lastPassMs > 0 && lastPassJobs > 0)
			base = double(lastPassMs) / double(lastPassJobs) * cfg.passGrowth;
		return base * complexity;
	};
	// 候補集合 cs を k 粒子ずつ読むときの実ジョブ数(合法粒子が k 未満の候補は少ない)
	auto count_jobs = [&](const std::vector<size_t>& cs, size_t k) -> uint64_t {
		uint64_t n = 0;
		for (size_t i : cs)
			n += std::min(k, legalIdx[i].size());
		return n;
	};
	for (int d = 2; d <= cfg.searchDepth; d += step) {
		size_t k2 = size_t(cfg.stage2Samples);
		if (depthDone > 0) {
			// 次のパスに入れるかの判定。
			//   従来: 予算の60%を過ぎていたら入らない(固定ゲート)。
			//   予測: 実ジョブ数 × 推定単価が締め切りまでに収まるなら入る。
			//         **楽観側に倒す**(推定の70%で判定): 間に合わなかったパスは
			//         破棄されるだけで強さには響かず(CPUを捨てるだけ)、走らせた
			//         ぶんの観測で単価の推定が更新される。悲観側に倒すと
			//         「高いと思い込んだ深さを二度と試さず、推定が固着する」。
			// どちらも「始めたが間に合わなかった」パスは下で破棄される。
			std::vector<size_t> nextTop = top;
			if (cfg.halving && top.size() > 2) {
				// 候補の絞り込み(直前に完走したパスの序列で上位 ceil(n/2)、下限2)。
				std::stable_sort(nextTop.begin(), nextTop.end(),
				                 [&](size_t a, size_t b) { return comb2[a] > comb2[b]; });
				nextTop.resize(std::max<size_t>(2, (top.size() + 1) / 2));
			}
			bool go;
			if (cfg.passGate) {
				const double est = job_ms_est(d) * double(count_jobs(nextTop, k2));
				go = now() + TimePoint(est * 0.7) <= s2Deadline;
			} else {
				go = now() <= t0 + budgetMs * 3 / 5;
			}
			// 予測ゲートの較正用トレース(stderr。loglevel 2 のときだけ)
			if (cfg.logLevel >= 2)
				fprintf(stderr, "gate d=%d go=%d t=%lld est=%.1f s2dl=%lld top=%zu next=%zu jm=%.4f cx=%.2f last=%lld\n",
				        d, int(go), (long long)(now() - t0), job_ms_est(d) * double(count_jobs(nextTop, k2)),
				        (long long)(s2Deadline - t0), top.size(), nextTop.size(), job_ms_est(d), complexity, (long long)lastPassMs);
			if (!go) {
				res.gateSkipped = 1;
				break;
			}
			// 絞られた候補は最終選択の対象からも外れる: 浅いパスの値と深いパスの
			// 値を同じ土俵で比べると、深く読んで下がった本命より浅い値の残った
			// 脇の手が勝ってしまう。
			top = std::move(nextTop);
		} else if (cfg.passGate) {
			// 最初のパス(d=2)。丸ごと収まらない予測なら粒子数を収まる範囲に
			// 減らす(下限8)。深さ0(stage1 の qsearch だけ)で終わるより、粒子を
			// 減らしてでも全候補に深さ2の値を付けるほうがよい。予測不能(未観測)
			// なら従来どおり全粒子で試す。
			const double est   = job_ms_est(d);
			const double avail = double(s2Deadline - now());
			if (est > 0.0 && avail > 0.0 && !top.empty()) {
				const double perSample = est * double(top.size());  // 1候補1粒子ぶんの上限
				const double fit = avail / perSample;
				if (fit < double(k2))
					k2 = size_t(std::max(8.0, std::floor(fit)));
			}
		}
		const TimePoint     passT0 = now();
		std::vector<double> pass(M, 0.0);
		bool                aborted = false;
		uint64_t            passTrunc = 0;
		uint64_t            passJobs  = 0;  // 実際に評価し終えたジョブ数(破棄時も数える)
		if (nWorkers > 1) {
			std::vector<std::vector<uint32_t>> sels(top.size());
			for (size_t t = 0; t < top.size(); ++t)
				sels[t] = pick_particles(legalIdx[top[t]], k2);
			std::vector<double> sum;
			std::vector<size_t> cnt;
			aborted = parallel_eval(top, sels, /*depth=*/d, nodesLimit2,
			                        /*abortable=*/true, sum, cnt, res.nodes, passTrunc);
			for (size_t t = 0; t < top.size(); ++t)
				passJobs += cnt[t];
			if (!aborted)
				for (size_t t = 0; t < top.size(); ++t) {
					size_t i = top[t];
					pass[i] = cnt[t] > 0 ? combined(i, sum[t] / double(cnt[t])) : comb1[i];
				}
		} else {
			// 逐次版(stage1 と同じ理由で分けたまま。ブレースも同様)
			for (size_t i : top) {
				auto sel = pick_particles(legalIdx[i], k2);
				double sum = 0;
				size_t cnt = 0;
				for (uint32_t j : sel) {
					if (now() > s2Deadline) { aborted = true; break; }
					sum += eval_one(parts[j]->pos, cands[i], /*depth=*/d,
					                nodesLimit2, ctx_for(0), res.nodes, passTrunc);
					++cnt;
				}
				passJobs += cnt;
				pass[i] = cnt > 0 ? combined(i, sum / double(cnt)) : comb1[i];
				if (aborted)
					break;
			}
		}
		// コストモデルの更新(深さ別)。破棄されたパスも完了ジョブぶんは観測になる。
		// ジョブが少ない・1ms未満の観測は分解能のノイズが大きいので捨てる。
		// 局面の複雑さの補正ぶんは割り戻して「標準的な局面での単価」として持つ。
		const TimePoint passMs = now() - passT0;
		if (d < Thinker::JOB_MS_SLOTS && passJobs >= 16 && passMs >= 2)
			jobMs2_[d] = ema_update(jobMs2_[d], double(passMs) / double(passJobs) / complexity);
		if (!aborted) {
			// パスを完走したときだけ採用する(部分的な値で序列を壊さない)
			for (size_t i : top)
				comb2[i] = pass[i];
			depthDone    = d;
			lastPassMs   = passMs;
			lastPassJobs = passJobs;
			res.jobs2  += passJobs;
			res.trunc2 += passTrunc;
			res.passes.emplace_back(d, int(passMs));
		} else {
			if (cfg.logLevel >= 2)
				fprintf(stderr, "abort d=%d t=%lld s2dl=%lld top=%zu k2=%zu jobs=%llu s1=%lld cx=%.2f\n", d,
				        (long long)(now() - t0), (long long)(s2Deadline - t0), top.size(), k2,
				        (unsigned long long)passJobs, (long long)res.stage1Ms, complexity);
			break;
		}
	}
	res.depthReached = depthDone;
	res.stage2Ms     = now() - tStage2;

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
	thinker_.new_game();  // 置換表・historyを破棄(前対局の値を持ち越さない)
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
