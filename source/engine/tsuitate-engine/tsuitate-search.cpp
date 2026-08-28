// ついたて将棋エンジン: エントリポイントとプロトコルループ
//
// 標準のUSIとは別の、ついたて将棋用の行プロトコルを話す。
// (相手の指し手が見えないためUSIのposition/movesでは局面を伝えられない。
//  ブリッジ(bridge/)がサイトのSocket.IOイベントとこのプロトコルを変換する)
//
//   usi / isready / usinewgame / quit      … USI互換の起動シーケンス
//   set <key> <value>                      … 設定(particles, budget, seed, ...)
//   new <sente|gote>                       … 対局開始(自分の手番色)
//   moveok <usi> [cap <role>]              … 自分の着手が受理された(role=取った駒)
//   foul <usi>                             … 自分の着手が反則だった
//   oppmove [cap <sq>]                     … 相手が着手(sq=自駒が取られたマス)
//   oppfoul                                … 相手が反則
//   check <you|opp>                        … 王手宣言(直前の着手イベントに付く)
//   go [mytime <ms>] [opptime <ms>] [inc <ms>] [budget <ms>]
//                                          → bestmove <usi> | bestmove resign
//   state                                  … デバッグ出力
//   arena games <n> [p1 <kind>] [p2 <kind>] [budget <ms>] [seed <n>]
//         [p1cfg <key> <val>] [p2cfg <key> <val>]
//                                          … ローカル自己対戦(審判つき)。
//                                            p1cfg/p2cfgで片側だけ設定を変えれば
//                                            同一バイナリ内でA/B比較ができる
#include "../../config.h"

#if defined(TSUITATE_ENGINE)

#include <iostream>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <string>

#include "../../types.h"
#include "../../position.h"
#include "../../usi.h"
#include "../../misc.h"

#include "tsuitate_common.h"
#include "think.h"
#include "arena.h"

namespace YaneuraOu {
namespace Tsuitate {

namespace {

// -fno-exceptions でビルドしているので std::stoi / std::stod は使えない。
// 不正な文字列を渡すと例外が投げられ、そのまま std::terminate でプロセスが落ちる
// (`set particles`(値なし)や `arena p1cfg foulbase`(値なし)で実際に落ちていた)。
// 失敗を戻り値で返すパーサに置き換える。
bool parse_ll(const std::string& s, long long& out) {
	if (s.empty())
		return false;
	errno = 0;
	char* end = nullptr;
	long long v = std::strtoll(s.c_str(), &end, 10);
	if (errno != 0 || end == s.c_str() || *end != '\0')
		return false;
	out = v;
	return true;
}

bool parse_d(const std::string& s, double& out) {
	if (s.empty())
		return false;
	errno = 0;
	char* end = nullptr;
	double v = std::strtod(s.c_str(), &end);
	// -ffast-math が有効なので isfinite は使えない(UB)。
	// 桁あふれ(HUGE_VAL)や桁落ちは strtod が errno=ERANGE で知らせるので、
	// それと下の範囲検査で十分。
	if (errno != 0 || end == s.c_str() || *end != '\0')
		return false;
	out = v;
	return true;
}

// 設定キーの適用。`set`(実対局)と `arena p1cfg/p2cfg`(A/B比較)で共有する。
// 未知のキー・不正な値・範囲外の値はすべて false を返す(適用しない)。
bool set_config_key(Config& c, const std::string& key, const std::string& val) {
	long long iv = 0;
	double    dv = 0;
	const bool okI = parse_ll(val, iv);
	const bool okD = parse_d(val, dv);
	bool ok = true;

	// 整数キー: 範囲外なら適用しない(0除算やNaNを構造的に防ぐ)
	auto I = [&](long long lo, long long hi) -> int {
		if (!okI || iv < lo || iv > hi) { ok = false; return 0; }
		return int(iv);
	};
	// 実数キー
	auto D = [&](double lo, double hi) -> double {
		if (!okD || dv < lo || dv > hi) { ok = false; return 0.0; }
		return dv;
	};
	auto apply_i = [&](int& dst, long long lo, long long hi) {
		int v = I(lo, hi);
		if (ok) dst = v;
	};
	auto apply_d = [&](double& dst, double lo, double hi) {
		double v = D(lo, hi);
		if (ok) dst = v;
	};

	if      (key == "particles")    apply_i(c.particles, 1, 100000);
	else if (key == "stage1")       apply_i(c.stage1Samples, 1, 100000);
	else if (key == "stage2")       apply_i(c.stage2Samples, 1, 100000);
	else if (key == "topk")         apply_i(c.stage2TopK, 1, 1000);
	else if (key == "depth")        apply_i(c.searchDepth, 0, 64);
	else if (key == "budget")       apply_i(c.budgetMs, 1, 3600000);
	else if (key == "regentries")   apply_i(c.regenTries, 0, 10000000);
	else if (key == "blocksamples") apply_i(c.blockSamples, 1, 100000);
	else if (key == "opppolicy")    apply_i(c.oppPolicy, 0, 1);
	else if (key == "deduce")       apply_i(c.deduce, 0, 1);
	else if (key == "synthprior")   apply_i(c.synthPrior, 0, 1);
	else if (key == "syncpct")      apply_i(c.syncPct, 0, 100);
	else if (key == "regenfloor")   apply_i(c.regenFloorPct, 0, 100);
	else if (key == "loglevel")     apply_i(c.logLevel, 0, 2);
	else if (key == "policytemp")   apply_d(c.policyTemp, 1.0, 100000.0);
	else if (key == "policyeps")    apply_d(c.policyEps, 0.0, 1.0);
	else if (key == "foulbase")     apply_d(c.foulBaseCp, 0.0, 1e6);
	else if (key == "foulstep")     apply_d(c.foulStepCp, 0.0, 1e6);
	else if (key == "foulopp")      apply_d(c.foulOppW, 0.0, 100.0);
	else if (key == "plegalprior")  apply_d(c.pLegalPrior, 0.0, 1e6);
	else if (key == "blockcp")      apply_d(c.blockCp, 0.0, 1e6);
	else if (key == "seed") {
		if (!okI || iv < 0) return false;
		c.seed = uint64_t(iv);
	}
	else return false;
	return ok;
}

class ProtocolLoop {
public:
	int run() {
		std::string line;
		while (std::getline(std::cin, line)) {
			if (!dispatch(line))
				break;
		}
		return 0;
	}

private:
	Config  cfg_;
	BotCore core_;
	bool    inGame_ = false;

	bool dispatch(const std::string& line) {
		std::istringstream is(line);
		std::string cmd;
		is >> cmd;
		if (cmd.empty())
			return true;

		if (cmd == "quit")
			return false;
		else if (cmd == "usi") {
			sync_cout << "id name TsuitateYaneuraOu (fork of YaneuraOu, " << EVAL_TYPE_NAME << ")\n"
			          << "id author tempakyousuke + YaneuraOu project" << sync_endl;
			sync_cout << "usiok" << sync_endl;
		}
		else if (cmd == "isready")
			sync_cout << "readyok" << sync_endl;
		else if (cmd == "usinewgame") {
			// 対局状態は new コマンドで初期化する
		}
		else if (cmd == "set")
			cmd_set(is);
		else if (cmd == "new")
			cmd_new(is);
		else if (cmd == "moveok")
			cmd_moveok(is);
		else if (cmd == "foul")
			cmd_foul(is);
		else if (cmd == "oppmove")
			cmd_oppmove(is);
		else if (cmd == "oppfoul")
			core_.on_opp_foul();
		else if (cmd == "check")
			cmd_check(is);
		else if (cmd == "go")
			cmd_go(is);
		else if (cmd == "state")
			cmd_state();
		else if (cmd == "arena")
			cmd_arena(is);
		else
			sync_cout << "info string unknown command: " << cmd << sync_endl;
		return true;
	}

	void cmd_set(std::istringstream& is) {
		std::string key, val;
		is >> key >> val;
		if (!set_config_key(cfg_, key, val)) {
			sync_cout << "info string bad option: " << key << " = " << val << sync_endl;
			return;
		}
		sync_cout << "info string set " << key << " = " << val << sync_endl;
	}

	void cmd_new(std::istringstream& is) {
		std::string colorStr;
		is >> colorStr;
		Color us = colorStr == "gote" ? WHITE : BLACK;
		core_.new_game(us, cfg_);
		inGame_ = true;
		sync_cout << "info string new game as " << (us == BLACK ? "sente" : "gote") << sync_endl;
	}

	void cmd_moveok(std::istringstream& is) {
		std::string usi, tok, roleStr;
		is >> usi;
		PieceType capRole = NO_PIECE_TYPE;
		while (is >> tok)
			if (tok == "cap" && (is >> roleStr))
				capRole = role_from_site(roleStr);
		Move m = parse_usi_move(core_.view(), usi);
		if (m == Move::none()) {
			sync_cout << "info string ERROR bad move: " << usi << sync_endl;
			return;
		}
		core_.on_our_move_accepted(m, capRole);
	}

	void cmd_foul(std::istringstream& is) {
		std::string usi;
		is >> usi;
		Move m = parse_usi_move(core_.view(), usi);
		if (m == Move::none()) {
			sync_cout << "info string ERROR bad move: " << usi << sync_endl;
			return;
		}
		core_.on_our_foul(m);
	}

	void cmd_oppmove(std::istringstream& is) {
		std::string tok, sqStr;
		Square capSq = SQ_NB;
		while (is >> tok)
			if (tok == "cap" && (is >> sqStr))
				capSq = parse_usi_square(sqStr);
		core_.on_opp_move(capSq);
	}

	void cmd_check(std::istringstream& is) {
		std::string side;
		is >> side;
		core_.on_check_declared(side == "you");
	}

	void cmd_go(std::istringstream& is) {
		if (!inGame_) {
			sync_cout << "info string ERROR go before new" << sync_endl;
			sync_cout << "bestmove resign" << sync_endl;
			return;
		}
		long myTime = -1, inc = 3000, budget = -1;
		std::string tok;
		while (is >> tok) {
			if (tok == "mytime") is >> myTime;
			else if (tok == "opptime") { long x; is >> x; }
			else if (tok == "inc") is >> inc;
			else if (tok == "budget") is >> budget;
		}
		if (budget < 0) {
			if (myTime >= 0) {
				// フィッシャー時計: increment をほぼ使い切り、残り時間の一部を上乗せ
				budget = inc * 4 / 5 + myTime / 40;
				budget = std::max(300L, std::min(budget, 3000L));
				// 残り時間が少ないときは絞る
				if (myTime < 20000)
					budget = std::max(200L, myTime / 20);
			} else {
				budget = cfg_.budgetMs;
			}
		}

		ThinkResult r = core_.think(int(budget));
		if (cfg_.logLevel >= 1)
			sync_cout << "info string particles=" << r.nParticles
			          << " relax=" << r.relaxLevel
			          << " p_legal=" << int(r.pLegal * 100) << "%"
			          << " cp=" << int(r.expectedCp)
			          << " depth=" << r.depthReached
			          << " time=" << r.elapsedMs << "ms" << sync_endl;
		if (r.best == Move::none())
			sync_cout << "bestmove resign" << sync_endl;
		else
			sync_cout << "bestmove " << to_usi_string(r.best) << sync_endl;
	}

	void cmd_state() {
		const OwnView& v = core_.view();
		sync_cout << "info string fouls=" << v.ourFouls << "/" << v.oppFouls
		          << " inCheck=" << v.inCheckNow
		          << " particles=" << core_.belief().size()
		          << " events=" << core_.history().events.size() << sync_endl;
	}

	void cmd_arena(std::istringstream& is) {
		ArenaOptions opt;
		opt.cfg  = cfg_;
		opt.cfg2 = cfg_;
		// アリーナの既定は300ms/手(実対局の思考予算とは別物)。
		// `budget` で両者、`p1cfg budget` / `p2cfg budget` で片側だけ変えられる。
		opt.cfg.budgetMs = opt.cfg2.budgetMs = 300;

		// 数値引数は必ず検証する。`is >> int` に直接読ませると、値を書き忘れたときに
		// 0が入ったうえで失敗ビットが立ち、**以降の引数が全部黙って捨てられる**
		// (`arena games 1 budget p2 belief` が budget=0・相手はheuristicのまま
		//  何事もなかったように走っていた)。
		bool bad = false;
		auto num = [&](const char* name, long long lo, long long hi, long long& out) {
			std::string v;
			long long   x = 0;
			if (!(is >> v) || !parse_ll(v, x) || x < lo || x > hi) {
				sync_cout << "info string bad arena option: " << name << " = " << v << sync_endl;
				bad = true;
				return false;
			}
			out = x;
			return true;
		};

		std::string tok;
		while (!bad && is >> tok) {
			long long x = 0;
			// プレイヤー種別も検証する。素の `is >> opt.p1` だと、綴りを間違えても
			// make_player が黙って heuristic を返し、結果行にはその綴りが
			// 相手名として出るので「beliefと対戦したつもりが内蔵bot相当だった」
			// という取り違えに気づけない。
			auto kind = [&](const char* name, std::string& out) {
				std::string v;
				if (!(is >> v) || (v != "belief" && v != "heuristic")) {
					sync_cout << "info string bad arena option: " << name << " = " << v
					          << " (belief | heuristic)" << sync_endl;
					bad = true;
					return;
				}
				out = v;
			};
			if (tok == "games") { if (num("games", 1, 100000, x)) opt.games = int(x); }
			else if (tok == "p1") kind("p1", opt.p1);
			else if (tok == "p2") kind("p2", opt.p2);
			else if (tok == "maxplies") { if (num("maxplies", 1, 100000, x)) opt.maxPlies = int(x); }
			else if (tok == "verbose") opt.verbose = true;
			else if (tok == "budget") {
				if (num("budget", 1, 3600000, x))
					opt.cfg.budgetMs = opt.cfg2.budgetMs = int(x);
			}
			else if (tok == "particles") {
				if (num("particles", 1, 100000, x))
					opt.cfg.particles = opt.cfg2.particles = int(x);
			}
			else if (tok == "seed") {
				if (num("seed", 0, INT64_MAX, x))
					opt.seed = uint64_t(x);
			}
			// A/B比較: 片側だけ設定を変えて同一バイナリ内で対戦させる
			else if (tok == "p1cfg" || tok == "p2cfg") {
				std::string k, v;
				if (!(is >> k) || !(is >> v)) {
					sync_cout << "info string bad arena option: " << tok
					          << " needs <key> <value>" << sync_endl;
					bad = true;
				} else if (k == "seed") {
					// 局ごとに振り直すので片側だけ固定はできない。`arena seed N` を使う
					sync_cout << "info string " << tok
					          << " seed is ignored (use `arena seed N`)" << sync_endl;
					bad = true;
				} else if (!set_config_key(tok == "p1cfg" ? opt.cfg : opt.cfg2, k, v)) {
					sync_cout << "info string bad option: " << k << " = " << v << sync_endl;
					bad = true;
				}
			}
			else {
				sync_cout << "info string unknown arena option: " << tok << sync_endl;
				bad = true;
			}
		}
		if (bad) {
			sync_cout << "info string arena aborted" << sync_endl;
			return;
		}
		run_arena(opt);
	}
};

void engine_main() {
	ProtocolLoop loop;
	loop.run();
}

// 通常エンジン(priority 0)より高いpriorityで登録し、このバイナリの既定にする
static EngineFuncRegister r(engine_main, "TsuitateEngine", 10);

} // namespace
} // namespace Tsuitate
} // namespace YaneuraOu

#endif // TSUITATE_ENGINE
