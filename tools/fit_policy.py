#!/usr/bin/env python3
"""§4 prior較正: アリーナの教師データ(arena ... dump <path> のJSONL)に
条件付きロジット(softmax回帰)を当て、fast_policy_score の重み表
(POLICY_W_FIT)を出力する。

  python3 tools/fit_policy.py dump1.jsonl [dump2.jsonl ...]

モデル: 決定 d で意図 i が選ばれる確率 ∝ exp(φ_i · β)。
実行時は exp(score / policyTemp) なので、出力の重みは w = round(β × 120)
(policyTemp の既定 120 を織り込む。温度を変えるなら読み替えること)。

評価は局(g)単位の 80/20 held-out で、
  - uniform     … 一様(情報なしの下限)
  - hand        … 手書きの重み(POLICY_W_HAND、配備時の oppCheckPrior=0 相当。
                   王手系の特徴を平時側へ畳み直して評価する)
  - hand+chk    … 手書き + oppCheckPrior=1(王手の反映あり)
  - fit         … 適合した重み
の log-loss / decision を並べる。採用ゲートは「fit が hand に held-out で勝つ」。

注意(design docの§4): 教師はアリーナ集団(belief=強い相手の代理、
heuristic=サイト内蔵bot相当)であり、サイトの人間・他botの実データではない。
"""
import json
import math
import sys

import numpy as np

PF_DIM = 24
PF_DROP_CHECK = 3
PF_KING_QUIET = 21
PF_KING_CHECK = 22

# PolicyWeights のフィールド順(C++の構造体と1対1)
FIELD_NAMES = (
    ["center", "dropBase", "dropCamp", "dropCheck"]
    + [f"push[{i}]" for i in range(16)]
    + ["promote", "kingQuiet", "kingCheck", "kingDist"]
)
PT_NAMES = {1: "PAWN", 2: "LANCE", 3: "KNIGHT", 4: "SILVER", 5: "BISHOP", 6: "ROOK",
            7: "GOLD", 8: "KING", 9: "PRO_PAWN", 10: "PRO_LANCE", 11: "PRO_KNIGHT",
            12: "PRO_SILVER", 13: "HORSE", 14: "DRAGON"}

W_HAND = np.zeros(PF_DIM)
W_HAND[0:4] = [10, -150, 80, -200]
W_HAND[4:20] = [0, 110, 70, 85, 95, 55, 65, 80, -40, 90, 70, 80, 85, 70, 80, 0]
W_HAND[20:24] = [300, -250, 400, 0]

TEMP = 120.0  # Config::policyTemp の既定。実行時の exp(score/temp) に合わせる


def load(paths):
    """decisions: (phi[n_i, PF_DIM], chosen, game_key, kind, chk) のリスト"""
    out = []
    for pi, path in enumerate(paths):
        with open(path) as f:
            for line in f:
                d = json.loads(line)
                phi = np.array(d["phi"], dtype=np.float64)
                out.append((phi, d["c"], (pi, d["g"]), d["k"], d["chk"]))
    return out


def fold_check_features(phi):
    """王手系の特徴を平時側へ畳む = oppCheckPrior=0 の手書きpriorが見る特徴。
    (王手中でも kingQuiet が玉移動に、dropCheck は消える)"""
    q = phi.copy()
    q[:, PF_KING_QUIET] += q[:, PF_KING_CHECK]
    q[:, PF_KING_CHECK] = 0
    q[:, PF_DROP_CHECK] = 0
    return q


def logloss(decisions, w, eps=0.10, fold=False):
    """実行時と同じ softmax(φ·w / TEMP) + ε一様 の log-loss / decision"""
    total = 0.0
    for phi, c, _, _, chk in decisions:
        x = (fold_check_features(phi) if (fold and chk) else phi) @ w / TEMP
        x -= x.max()
        sm = np.exp(x)
        sm /= sm.sum()
        prob = (1 - eps) * sm[c] + eps / len(sm)
        total += -math.log(max(prob, 1e-300))
    return total / len(decisions)


def fit(decisions, l2=1e-3, iters=400):
    """条件付きロジットの最尤推定(全バッチ勾配上昇+モメンタム)。
    β は score/TEMP スケール(= 実行時の softmax の引数)で推定する。"""
    beta = W_HAND / TEMP  # 手書きから出発(悪くない初期値で速く収束)
    vel = np.zeros_like(beta)
    n = len(decisions)
    lr, mom = 0.5, 0.9
    for it in range(iters):
        grad = -2 * l2 * beta
        ll = 0.0
        for phi, c, _, _, _ in decisions:
            x = phi @ beta
            x -= x.max()
            sm = np.exp(x)
            sm /= sm.sum()
            ll += math.log(max(sm[c], 1e-300))
            grad += phi[c] - sm @ phi
        grad /= n
        vel = mom * vel + lr * grad
        beta = beta + vel
        if it % 50 == 0 or it == iters - 1:
            print(f"  iter {it:4d}  train ll/decision = {ll / n:.4f}", file=sys.stderr)
    return beta


def main():
    paths = sys.argv[1:]
    if not paths:
        print(__doc__)
        sys.exit(1)
    decisions = load(paths)
    games = sorted({g for _, _, g, _, _ in decisions})
    rng = np.random.default_rng(20260915)
    rng.shuffle(games)
    heldout_games = set(games[: max(1, len(games) // 5)])
    train = [d for d in decisions if d[2] not in heldout_games]
    test = [d for d in decisions if d[2] in heldout_games]
    kinds = {}
    for _, _, _, k, _ in decisions:
        kinds[k] = kinds.get(k, 0) + 1
    print(f"decisions: total={len(decisions)} train={len(train)} test={len(test)} "
          f"games={len(games)} kinds={kinds}")

    beta = fit(train)
    w_fit = beta * TEMP

    uni = np.zeros(PF_DIM)
    print("\nheld-out log-loss / decision (小さいほど良い):")
    print(f"  uniform  : {logloss(test, uni):.4f}")
    print(f"  hand     : {logloss(test, W_HAND, fold=True):.4f}   (配備時: oppCheckPrior=0)")
    print(f"  hand+chk : {logloss(test, W_HAND):.4f}   (oppCheckPrior=1)")
    print(f"  fit      : {logloss(test, w_fit):.4f}")
    print("\n教師種別ごとの held-out log-loss (fit / hand):")
    for k in kinds:
        sub = [d for d in test if d[3] == k]
        if sub:
            print(f"  {k:10s}: {logloss(sub, w_fit):.4f} / {logloss(sub, W_HAND, fold=True):.4f}"
                  f"  (n={len(sub)})")

    w = np.round(w_fit).astype(int)
    print("\n// tools/fit_policy.py の出力(POLICY_W_FIT に貼る):")
    print("const PolicyWeights POLICY_W_FIT = {")
    print(f"\t/*center*/ {w[0]}, /*dropBase*/ {w[1]}, /*dropCamp*/ {w[2]}, "
          f"/*dropCheck*/ {w[3]},")
    print("\t/*push*/ {")
    print(f"\t\t{w[4]},    // NO_PIECE_TYPE(意図には現れない。値は無意味)")
    for i in range(5, 20):
        name = PT_NAMES.get(i - 4, "?")
        print(f"\t\t{w[i]},  // {name}")
    print("\t},")
    print(f"\t/*promote*/ {w[20]}, /*kingQuiet*/ {w[21]}, /*kingCheck*/ {w[22]},")
    print(f"\t/*kingDist*/ {w[23]},")
    print("};")


if __name__ == "__main__":
    main()
