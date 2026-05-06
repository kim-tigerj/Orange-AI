#!/usr/bin/env bash
# tools/list-actors.sh ??actor ?곹깭 ??以??붿빟. spawn ???ㅻⅨ actor ?몄? 鍮좊Ⅴ寃?
#
# 異쒕젰 ??以??뺤떇:
#   <actor_id>  <age>s  <吏꾪뻾?곹깭>  claim=<N>  | <intent 泥?80??
#
# 留?spawn 留덈떎 留ㅻ쾲 `for f in ...; do cat $f; done` 湲멸쾶 ?곷뜕 ?먮━ ?⑥텞.
# ?뺥??μ쓽 媛곸꽦 ?덉감(CLAUDE.md 짠1) ??actors 湲濡??④퀎瑜???紐낅졊?쇰줈.

set -eu

ACTOR_DIR="${APPDATA:-$HOME/AppData/Roaming}/OrangeCode/actors"
if [ ! -d "$ACTOR_DIR" ]; then
    echo "(no actors dir: $ACTOR_DIR)"
    exit 0
fi

ACTOR_DIR="$ACTOR_DIR" PYTHONIOENCODING=utf-8 python -c '
import json, os, glob, sys, time

# Windows git-bash ??湲곕낯 肄섏넄??cp949 ???쒓뎅??intent 媛 ? 濡??⑥뼱吏???媛뺤젣 UTF-8.
try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

base = os.environ["ACTOR_DIR"]
files = sorted(
    glob.glob(os.path.join(base, "*.json")),
    key=os.path.getmtime,
    reverse=True,
)
if not files:
    print("(no actors)")
    sys.exit(0)

now = time.time()
for f in files:
    try:
        with open(f, encoding="utf-8") as fh:
            d = json.load(fh)
    except Exception:
        continue
    age = int(now - os.path.getmtime(f))
    intent = (d.get("current_intent") or "").replace("\n", " ").replace("\r", " ")
    if len(intent) > 80:
        intent = intent[:79] + "??
    claim = d.get("claim_files") or []
    turn = d.get("turn") or {}
    inprog = "吏꾪뻾以? if turn.get("in_progress") else " ?湲?
    aid = d.get("actor_id", "?")
    # stale (age > 1h) ??二쎌뿀嫄곕굹 ?먮━ 鍮꾩슫 actor ???몄? 臾닿쾶 以꾩씠湲? ?④? X (?ъ슜??*??蹂몃떎*).
    stale = age > 3600
    prefix = "[stale] " if stale else "        "
    # ?쒓뎅?????뺣젹? ?섍꼍留덈떎 ?щ씪 ?⑥닚 ljust 留????꾧뎄??鍮좊Ⅸ ?뺤씤?⑹씠???덉슜 ?ㅼ감.
    print(f"{prefix}{aid:<22} {age:>5}s {inprog} claim={len(claim):<2} | {intent}")
'
