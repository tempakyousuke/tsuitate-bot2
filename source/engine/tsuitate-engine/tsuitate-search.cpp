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
//                                          … ローカル自己対戦(審判つき)
#include "../../config.h"

#if defined(TSUITATE_ENGINE)

#include <iostream>
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
		if (key == "particles") cfg_.particles = std::stoi(val);
		else if (key == "stage1") cfg_.stage1Samples = std::stoi(val);
		else if (key == "stage2") cfg_.stage2Samples = std::stoi(val);
		else if (key == "topk") cfg_.stage2TopK = std::stoi(val);
		else if (key == "depth") cfg_.searchDepth = std::stoi(val);
		else if (key == "budget") cfg_.budgetMs = std::stoi(val);
		else if (key == "seed") cfg_.seed = std::stoull(val);
		else if (key == "loglevel") cfg_.logLevel = std::stoi(val);
		else {
			sync_cout << "info string unknown option: " << key << sync_endl;
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
		opt.cfg = cfg_;
		std::string tok;
		while (is >> tok) {
			if (tok == "games") is >> opt.games;
			else if (tok == "p1") is >> opt.p1;
			else if (tok == "p2") is >> opt.p2;
			else if (tok == "budget") is >> opt.budgetMs;
			else if (tok == "seed") is >> opt.seed;
			else if (tok == "particles") is >> opt.cfg.particles;
			else if (tok == "verbose") opt.verbose = true;
			else if (tok == "maxplies") is >> opt.maxPlies;
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
