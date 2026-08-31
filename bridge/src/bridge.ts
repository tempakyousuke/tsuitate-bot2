/**
 * beta.tsuitate.info ⇔ ついたて将棋エンジン(C++) ブリッジ
 *
 * サイトの Socket.IO 対局API(tsuitate リポジトリ docs/bot-api.md)のイベントを、
 * エンジンの行プロトコル(source/engine/tsuitate-engine/tsuitate-search.cpp 冒頭参照)
 * へ変換する。エンジンは子プロセスとして起動する。
 *
 * 実行:
 *   TSUITATE_URL=https://beta.tsuitate.info \
 *   TSUITATE_BOT_TOKEN=tsb_... \
 *   npm start
 *
 * イベント順序について:
 *   サーバーは着手イベント(game:moveAccepted / game:opponentMoved)の直後に
 *   王手宣言(game:check)を同期的にemitする。Socket.IOは同一接続でFIFOなので、
 *   ブリッジが受信順にエンジンへ流せば、エンジン側の
 *   「次のイベントかgoが来た時点で王手なし確定」という規約がそのまま成り立つ。
 *   goの前に game:sync の往復を挟むことで、直前の王手宣言の到着も保証される。
 */
import { spawn, type ChildProcessByStdio } from 'node:child_process';
import type { Readable, Writable } from 'node:stream';
import { appendFileSync, mkdirSync } from 'node:fs';
import { join } from 'node:path';
import { availableParallelism } from 'node:os';
import { createInterface } from 'node:readline';
import { io, type Socket } from 'socket.io-client';

// ---------------------------------------------------------------------------
// 設定
// ---------------------------------------------------------------------------
const url = process.env.TSUITATE_URL ?? 'https://beta.tsuitate.info';
const token = process.env.TSUITATE_BOT_TOKEN;
const enginePath = process.env.TSUITATE_ENGINE_PATH ?? '../source/YaneuraOu-by-gcc';
const queueRetryMs = Number(process.env.TSUITATE_QUEUE_RETRY_MS ?? 60_000);
const thinkDelayMs = Number(process.env.TSUITATE_THINK_MS ?? 200);
const engineOptions = process.env.TSUITATE_ENGINE_OPTS ?? ''; // 例: "particles 256,depth 4"
const recordDir = process.env.TSUITATE_RECORD_DIR ?? 'records'; // 空文字で無効

if (!token) {
	console.error('環境変数 TSUITATE_BOT_TOKEN にAPIトークンを設定してください');
	process.exit(1);
}

// ---------------------------------------------------------------------------
// サイト側の型(tsuitate/src/lib/shared の必要部分のみ)
// ---------------------------------------------------------------------------
type Color = 'sente' | 'gote';
interface ClockState {
	senteMs: number;
	goteMs: number;
	running: Color | null;
	serverTime: number;
}
interface PlayerView {
	gameId: string;
	yourColor: Color;
	turn: Color;
	moveNumber: number;
	clocks: ClockState;
	fouls: { you: number; opponent: number };
	youInCheck: boolean;
	opponentInCheck: boolean;
	status: 'playing' | 'ended';
}
type MoveAck =
	| { ok: true }
	| { ok: false; reason: 'foul'; foulCount: number }
	| { ok: false; reason: 'error'; error: string };

// ---------------------------------------------------------------------------
// エンジン子プロセス
// ---------------------------------------------------------------------------
class Engine {
	private proc: ChildProcessByStdio<Writable, Readable, null>;
	private waiters: ((line: string) => boolean)[] = [];

	constructor(path: string) {
		this.proc = spawn(path, [], { stdio: ['pipe', 'pipe', 'inherit'] });
		this.proc.on('exit', (code) => {
			console.error(`エンジンが終了しました (code=${code})`);
			process.exit(1);
		});
		const rl = createInterface({ input: this.proc.stdout });
		rl.on('line', (line) => {
			if (line.startsWith('info')) {
				console.log(`[engine] ${line}`);
				return;
			}
			// bestmove などの応答を待っているwaiterへ渡す
			this.waiters = this.waiters.filter((w) => !w(line));
		});
	}

	send(line: string): void {
		this.proc.stdin.write(line + '\n');
	}

	/** prefixで始まる次の行を待つ */
	wait(prefix: string, timeoutMs = 120_000): Promise<string> {
		return new Promise((resolve, reject) => {
			const timer = setTimeout(() => reject(new Error(`engine timeout: ${prefix}`)), timeoutMs);
			this.waiters.push((line) => {
				if (!line.startsWith(prefix)) return false;
				clearTimeout(timer);
				resolve(line);
				return true;
			});
		});
	}
}

const engine = new Engine(enginePath);
engine.send('usi');
// 既定でCPUコアぶんのワーカースレッドを使う(§3.1 粒子並列。4コアで実効3.8倍)。
// TSUITATE_ENGINE_OPTS に threads の指定があればそちらを優先する。
if (!/(^|,)\s*threads[\s=:]/.test(engineOptions)) {
	const n = Math.max(1, Math.min(availableParallelism(), 16));
	engine.send(`set threads ${n}`);
}
for (const opt of engineOptions.split(',')) {
	const kv = opt.trim();
	if (kv) engine.send(`set ${kv.replace(/[=:]/g, ' ')}`);
}

// ---------------------------------------------------------------------------
// Socket.IO 接続と対局ループ
// ---------------------------------------------------------------------------
const socket: Socket = io(url, { auth: { token }, transports: ['websocket'] });

let gameId: string | null = null;
let myColor: Color = 'sente';
/** 直近にサーバーへ送った手(moveAcceptedイベントとの対応付けに使う) */
let lastSentUsi: string | null = null;
let thinkTimer: ReturnType<typeof setTimeout> | null = null;
let thinking = false;

/** 対局記録(JSONL)。観測イベントをそのまま時系列で残す(後の分析・再現用) */
function record(event: string, payload: unknown): void {
	if (!recordDir || !gameId) return;
	try {
		mkdirSync(recordDir, { recursive: true });
		appendFileSync(
			join(recordDir, `${gameId}.jsonl`),
			JSON.stringify({ ts: Date.now(), event, payload }) + '\n'
		);
	} catch {
		// 記録失敗で対局を止めない
	}
}

function joinQueue(): void {
	socket.emit('queue:join', (res: { ok: boolean; error?: string }) => {
		if (res.ok) {
			console.log('キューに参加しました');
		} else {
			console.log(`キュー参加失敗(${res.error ?? '受付時間外'})。${queueRetryMs / 1000}s後に再試行`);
			setTimeout(joinQueue, queueRetryMs);
		}
	});
}

function scheduleThink(delayMs: number): void {
	if (thinkTimer) clearTimeout(thinkTimer);
	thinkTimer = setTimeout(() => void think(), delayMs);
}

async function think(): Promise<void> {
	if (!gameId || thinking) return;
	thinking = true;
	try {
		// syncの往復で「自分の手番か」を確認しつつ、直前のイベント
		// (王手宣言まで)がすべて到着していることを保証する
		const view = await new Promise<PlayerView | null>((resolve) => {
			socket.emit('game:sync', { gameId }, (res: { state: PlayerView | null }) =>
				resolve(res.state)
			);
		});
		if (!view || view.status !== 'playing') {
			// 対局が消えている(サーバー再起動など) → キューへ戻る
			if (view === null && gameId) {
				console.log('対局が見つかりません。キューへ戻ります');
				gameId = null;
				joinQueue();
			}
			return;
		}
		if (view.turn !== view.yourColor) return;

		const my = myColor === 'sente' ? view.clocks.senteMs : view.clocks.goteMs;
		const opp = myColor === 'sente' ? view.clocks.goteMs : view.clocks.senteMs;
		engine.send(`go mytime ${my} opptime ${opp} inc 3000`);
		const line = await engine.wait('bestmove');
		const usi = line.split(/\s+/)[1];

		if (!gameId) return; // 思考中に終局した
		if (usi === 'resign' || !usi) {
			socket.emit('game:resign', { gameId }, () => {});
			return;
		}
		lastSentUsi = usi;
		socket.emit('game:move', { gameId, usi }, (ack: MoveAck) => {
			if (ack.ok) {
				// 受理の確定と取った駒の情報は game:moveAccepted イベント側で処理する
				// (ackとイベントの到着順に依存しないようにする)
			} else if (ack.reason === 'foul') {
				console.log(`反則: ${usi} (累計${ack.foulCount})`);
				record('foul', { usi, foulCount: ack.foulCount });
				engine.send(`foul ${usi}`);
				scheduleThink(100);
			} else {
				console.error(`着手エラー: ${ack.error}`);
				scheduleThink(1000);
			}
		});
	} catch (e) {
		console.error('思考中のエラー:', e);
	} finally {
		thinking = false;
	}
}

socket.on('connect', () => {
	console.log(`接続しました: ${url}`);
	// 対局中の再接続なら game:active → 復帰。そうでなければキューへ
	if (!gameId) joinQueue();
});

socket.on('connect_error', (err: Error) => {
	console.error(`接続エラー: ${err.message}`);
});

socket.on('game:active', (payload: { gameId: string | null }) => {
	// 再接続時: 進行中の対局が残っていれば復帰(エンジン状態はプロセス内に保持)
	if (payload.gameId && payload.gameId === gameId) {
		console.log('対局へ再接続しました');
		scheduleThink(thinkDelayMs);
	}
});

socket.on('match:found', (payload: { gameId: string; yourColor: Color }) => {
	gameId = payload.gameId;
	myColor = payload.yourColor;
	lastSentUsi = null;
	engine.send(`new ${myColor}`);
	record('matchFound', payload);
	console.log(`マッチ成立: ${myColor} 番 (gameId=${payload.gameId})`);
});

socket.on('game:state', () => scheduleThink(thinkDelayMs));

socket.on('game:moveAccepted', (payload: { captured?: string }) => {
	record('moveAccepted', { usi: lastSentUsi, ...payload });
	if (lastSentUsi) {
		engine.send(`moveok ${lastSentUsi}` + (payload.captured ? ` cap ${payload.captured}` : ''));
		lastSentUsi = null;
	}
});

socket.on('game:opponentMoved', (payload: { capturedYourPieceAt?: string }) => {
	record('opponentMoved', payload);
	engine.send(
		'oppmove' + (payload.capturedYourPieceAt ? ` cap ${payload.capturedYourPieceAt}` : '')
	);
	scheduleThink(thinkDelayMs);
});

socket.on('game:foul', () => {
	// ackの側で処理済み(fouled手はエンジンへ通知済み)。時計精算のみのイベント
});

socket.on('game:opponentFoul', (payload: { opponentFoulCount: number }) => {
	record('opponentFoul', payload);
	engine.send('oppfoul');
});

socket.on('game:check', (payload: { inCheck: Color }) => {
	record('check', payload);
	engine.send(`check ${payload.inCheck === myColor ? 'you' : 'opp'}`);
});

socket.on('game:end', (payload: any) => {
	record('end', payload);
	console.log(
		`終局: ${payload.result} (${payload.reason}) — vs ${payload.opponent?.username} (R${payload.opponent?.rating})` +
			` レート: ${payload.ratingChange?.you?.before} → ${payload.ratingChange?.you?.after}`
	);
	gameId = null;
	lastSentUsi = null;
	if (thinkTimer) clearTimeout(thinkTimer);
	setTimeout(joinQueue, 3000);
});

socket.on('queue:closed', (payload: { reason: string }) => {
	console.log(`待機列が閉じられました(${payload.reason})。再試行します`);
	setTimeout(joinQueue, queueRetryMs);
});

process.on('SIGINT', () => {
	console.log('終了します');
	engine.send('quit');
	socket.close();
	process.exit(0);
});
