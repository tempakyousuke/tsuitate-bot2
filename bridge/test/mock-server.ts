/**
 * ローカルE2Eテスト用のモック対局サーバー。
 *
 * beta.tsuitate.info と同じイベント契約(tsuitate/docs/bot-api.md)・同じ裁定
 * (shogiops による通常将棋ルール判定、反則10回で負け)を最小実装し、
 * 接続してきたbot同士をマッチングして対局させる。
 *
 * 実行: npx tsx test/mock-server.ts  (PORT環境変数、既定 5199)
 *
 * ブリッジ+エンジンのE2E確認:
 *   ターミナル1: npx tsx test/mock-server.ts
 *   ターミナル2: TSUITATE_URL=http://localhost:5199 TSUITATE_BOT_TOKEN=tsb_a npm start
 *   ターミナル3: TSUITATE_URL=http://localhost:5199 TSUITATE_BOT_TOKEN=tsb_b npm start
 */
import { createServer } from 'node:http';
import { Server, type Socket } from 'socket.io';
import { initialSfen, parseSfen } from 'shogiops/sfen';
import { parseUsi, makeSquareName } from 'shogiops/util';
import { unpromote } from 'shogiops/variant/util';
import type { Shogi } from 'shogiops/variant/shogi';

const PORT = Number(process.env.PORT ?? 5199);
const MAX_FOULS = 10;
const MAX_GAMES = Number(process.env.MOCK_GAMES ?? 2);

type Color = 'sente' | 'gote';
const other = (c: Color): Color => (c === 'sente' ? 'gote' : 'sente');
const unpromoteStd = unpromote('standard');

interface Game {
	id: string;
	pos: Shogi;
	players: Record<Color, Socket>;
	names: Record<Color, string>;
	fouls: Record<Color, number>;
	moveNumber: number;
	ended: boolean;
}

let waiting: Socket | null = null;
let games = 0;
let gameSeq = 0;
const inGame = new Map<string, Game>();

const httpServer = createServer();
const ioServer = new Server(httpServer, { transports: ['websocket'] });

function clocks() {
	return { senteMs: 300_000, goteMs: 300_000, running: null, serverTime: Date.now() };
}

function playerView(g: Game, color: Color) {
	const board: { square: string; role: string }[] = [];
	for (const [sq, piece] of g.pos.board) {
		if (piece.color === color) board.push({ square: makeSquareName(sq), role: piece.role });
	}
	const hand: Record<string, number> = {};
	for (const [role, count] of g.pos.hands.color(color)) {
		if (count > 0) hand[role] = count;
	}
	return {
		gameId: g.id,
		yourColor: color,
		yourPieces: board,
		yourHand: hand,
		turn: g.pos.turn,
		moveNumber: g.moveNumber,
		clocks: clocks(),
		fouls: { you: g.fouls[color], opponent: g.fouls[other(color)] },
		youInCheck: g.pos.turn === color && g.pos.isCheck(),
		opponentInCheck: g.pos.turn !== color && g.pos.isCheck(),
		status: g.ended ? 'ended' : 'playing'
	};
}

function endGame(g: Game, result: string, reason: string) {
	g.ended = true;
	games++;
	for (const color of ['sente', 'gote'] as Color[]) {
		g.players[color].emit('game:end', {
			result,
			reason,
			finalSfen: '',
			moves: [],
			foulAttempts: [],
			ratingChange: { you: { before: 1200, after: 1200 }, opponent: { before: 1200, after: 1200 } },
			opponent: { username: 'mock', rating: 1200, isBot: true }
		});
		inGame.delete(g.players[color].id);
	}
	console.log(
		`game ${g.id}: ${result} (${reason}) after ${g.moveNumber - 1} moves, ` +
			`sente=${g.names.sente} gote=${g.names.gote} fouls=${g.fouls.sente}/${g.fouls.gote}`
	);
	if (games >= MAX_GAMES) {
		console.log('MOCK_DONE');
		setTimeout(() => process.exit(0), 500);
	}
}

function startGame(a: Socket, b: Socket) {
	const id = `g${++gameSeq}`;
	const pos = parseSfen('standard', initialSfen('standard')).unwrap() as Shogi;
	const g: Game = {
		id,
		pos,
		players: { sente: a, gote: b },
		names: {
			sente: String(a.handshake.auth?.token ?? '?'),
			gote: String(b.handshake.auth?.token ?? '?')
		},
		fouls: { sente: 0, gote: 0 },
		moveNumber: 1,
		ended: false
	};
	inGame.set(a.id, g);
	inGame.set(b.id, g);
	a.emit('match:found', { gameId: id, yourColor: 'sente' });
	b.emit('match:found', { gameId: id, yourColor: 'gote' });
	setTimeout(() => {
		a.emit('game:state', playerView(g, 'sente'));
		b.emit('game:state', playerView(g, 'gote'));
	}, 50);
	console.log(`game ${id}: start sente=${g.names.sente} gote=${g.names.gote}`);
}

ioServer.on('connection', (socket) => {
	const token = socket.handshake.auth?.token;
	if (!token || !String(token).startsWith('tsb_')) {
		socket.disconnect();
		return;
	}
	console.log(`connected: ${token}`);
	socket.emit('game:active', { gameId: null });

	socket.on('queue:join', (ack: (res: { ok: boolean; error?: string }) => void) => {
		ack({ ok: true });
		if (waiting && waiting.connected && waiting.id !== socket.id) {
			const a = waiting;
			waiting = null;
			startGame(a, socket);
		} else {
			waiting = socket;
		}
	});

	socket.on('game:sync', (payload: { gameId: string }, ack: (res: { state: unknown }) => void) => {
		const g = inGame.get(socket.id);
		if (!g || g.id !== payload.gameId || g.ended) {
			ack({ state: null });
			return;
		}
		const color: Color = g.players.sente.id === socket.id ? 'sente' : 'gote';
		ack({ state: playerView(g, color) });
	});

	socket.on('game:resign', (_payload: { gameId: string }, ack: (res: { ok: boolean }) => void) => {
		ack({ ok: true });
		const g = inGame.get(socket.id);
		if (!g || g.ended) return;
		const color: Color = g.players.sente.id === socket.id ? 'sente' : 'gote';
		endGame(g, `${other(color)}_win`, 'resign');
	});

	socket.on(
		'game:move',
		(payload: { gameId: string; usi: string }, ack: (res: unknown) => void) => {
			const g = inGame.get(socket.id);
			if (!g || g.ended || g.id !== payload.gameId) {
				ack({ ok: false, reason: 'error', error: 'no game' });
				return;
			}
			const color: Color = g.players.sente.id === socket.id ? 'sente' : 'gote';
			if (g.pos.turn !== color) {
				ack({ ok: false, reason: 'error', error: 'not your turn' });
				return;
			}
			const md = parseUsi(payload.usi);
			if (!md || !g.pos.isLegal(md)) {
				g.fouls[color]++;
				ack({ ok: false, reason: 'foul', foulCount: g.fouls[color] });
				socket.emit('game:foul', { foulCount: g.fouls[color], clocks: clocks() });
				g.players[other(color)].emit('game:opponentFoul', {
					opponentFoulCount: g.fouls[color],
					clocks: clocks()
				});
				if (g.fouls[color] >= MAX_FOULS) endGame(g, `${other(color)}_win`, 'foul_limit');
				return;
			}

			// 取られる駒(相手側から見た通知用)
			let captured: string | undefined;
			let capturedSquare: string | undefined;
			if (!('role' in md)) {
				const target = g.pos.board.get(md.to);
				if (target && target.color !== color) {
					captured = unpromoteStd(target.role) ?? target.role;
					capturedSquare = makeSquareName(md.to);
				}
			}
			g.pos.play(md);
			g.moveNumber++;
			ack({ ok: true });
			socket.emit('game:moveAccepted', {
				moveNumber: g.moveNumber,
				clocks: clocks(),
				captured
			});
			g.players[other(color)].emit('game:opponentMoved', {
				moveNumber: g.moveNumber,
				clocks: clocks(),
				capturedYourPieceAt: capturedSquare
			});
			if (g.pos.isCheck()) {
				const inCheck = g.pos.turn;
				g.players.sente.emit('game:check', { inCheck });
				g.players.gote.emit('game:check', { inCheck });
			}
			const outcome = g.pos.outcome();
			if (outcome && (outcome.result === 'checkmate' || outcome.result === 'stalemate')) {
				endGame(g, `${outcome.winner}_win`, outcome.result);
			}
		}
	);

	socket.on('disconnect', () => {
		if (waiting?.id === socket.id) waiting = null;
		const g = inGame.get(socket.id);
		if (g && !g.ended) {
			const color: Color = g.players.sente.id === socket.id ? 'sente' : 'gote';
			endGame(g, `${other(color)}_win`, 'disconnect');
		}
	});
});

httpServer.listen(PORT, () => {
	console.log(`mock tsuitate server listening on :${PORT}`);
});
