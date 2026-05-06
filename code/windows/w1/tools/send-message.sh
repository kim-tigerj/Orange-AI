#!/usr/bin/env bash
# tools/send-message.sh ??MESSAGES.md ????硫붿떆吏 ??ぉ prepend (理쒖떊????.
#
# ?묒そ actor (?뺥???/ ?ㅺ컧?? 媛 留?spawn ??MESSAGES.md 瑜??쎈룄濡?CLAUDE.md 짠1 ??# 諛뺥??덈떎. ???꾧뎄??洹?梨꾨꼸????ぉ??*?뺤떇 留욎떠* ?먮룞?쇰줈 異붽? ???곗샂??escape /
# ?쒓컖 / 諛쒖떊???먮룞 寃곗젙 ???몃씪???몄쭛??遺???쒓굅.
#
# ?ъ슜:
#   tools/send-message.sh <to_actor_id> <subject> path/to/body.txt
#   echo "蹂몃Ц" | tools/send-message.sh <to_actor_id> <subject>
#   tools/send-message.sh <to_actor_id> <subject>     # stdin 吏곸젒 ?낅젰 ??EOF
#
# 諛쒖떊??from) ???먮룞:
#   - ORANGE_CODE_PID set ????"manager:<pid>"   (?뺥????몄뒪?댁뒪 ?덉뿉???몄텧)
#   - ?꾨땲硫?             ??"supervisor:host" (?ㅺ컧???몄뒪???몄뀡?먯꽌 ?몄텧)

set -eu

if [ $# -lt 2 ]; then
    echo "usage: $0 <to_actor_id> <subject> [body_file_or_stdin]" >&2
    exit 2
fi

TO="$1"
SUBJECT="$2"
if [ $# -ge 3 ] && [ -f "$3" ]; then
    BODY=$(cat "$3")
else
    BODY=$(cat)  # stdin
fi

if [ -n "${ORANGE_CODE_PID:-}" ]; then
    FROM="manager:${ORANGE_CODE_PID}"
else
    FROM="supervisor:host"
fi

ROOT="${ORANGE_CODE_ROOT:-$(pwd)}"
MSG_FILE="$ROOT/MESSAGES.md"

NOW=$(python -c 'import datetime; print(datetime.datetime.now().strftime("%Y-%m-%d %H:%M"))')

FROM="$FROM" TO="$TO" SUBJECT="$SUBJECT" BODY="$BODY" NOW="$NOW" MSG_FILE="$MSG_FILE" \
PYTHONIOENCODING=utf-8 python -c '
import os, sys, pathlib

try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

now  = os.environ["NOW"]
fr   = os.environ["FROM"]
to   = os.environ["TO"]
sub  = os.environ["SUBJECT"]
body = os.environ["BODY"]

p = pathlib.Path(os.environ["MSG_FILE"])
existing = p.read_text(encoding="utf-8") if p.exists() else ""

sep = "---"
if sep in existing:
    head, _, tail = existing.partition(sep)
    head_with_sep = head + sep + "\n\n"
    rest = tail.lstrip("\n")
else:
    head_with_sep = existing + ("\n\n" if existing and not existing.endswith("\n") else "")
    rest = ""

entry = f"## {now} ??{fr} ??{to} ??{sub}\n\n{body}\n\n"
out = head_with_sep + entry + rest

tmp = p.with_suffix(p.suffix + ".tmp")
tmp.write_text(out, encoding="utf-8")
os.replace(tmp, p)
print(f"Prepended to {p}: {fr} -> {to} - {sub}")
'
