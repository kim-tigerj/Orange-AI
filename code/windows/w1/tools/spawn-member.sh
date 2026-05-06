#!/usr/bin/env bash
# tools/spawn-member.sh ???뺥??μ씠 ????몄뒪?댁뒪 spawn ?????몄텧?섎뒗 ?꾧뎄.
#
# ?ъ슜:
#   tools/spawn-member.sh <task_id> <task_spec_path>
#   ?? tools/spawn-member.sh task_sidebar tasks/task_sidebar.md
#
# ?숈옉:
#   1) 紐낆꽭 ?뚯씪??寃利앺빀?덈떎 ??`## 紐⑹쟻|諛곌꼍` 쨌 `## 踰붿쐞` 쨌 `## 寃利? ???뱀뀡??紐⑤몢 ?덇퀬
#      蹂몃Ц??200???댁긽?댁뼱??spawn ?듦낵. 誘몃떖?대㈃ 嫄곕? + ?쒓뎅???덈궡 + exit 5.
#   2) 遺紐??뺥??? PID 瑜?ORANGE_CODE_PARENT_PID env 濡?set.
#   3) bin/Release/OrangeCode.exe --role member --task-id <id> --task-spec <path> spawn.
#   4) ?먯떇??源⑥뼱?섎㈃ actor entry `member:<pid>` 濡??먮룞 ?깅줉 + bootstrap ?쇰줈 ?묒뾽 紐낆꽭 ?쎌쓬.
#   5) ?먯떇???묒뾽 ?꾨즺 ??reports/<task_id>.md ??寃곌낵 + MESSAGES.md 濡??꾨즺 ?뚮┝ (?뺥??Β룹삤媛먮룆 ?묒そ).
#
# 紐낆꽭 寃利앹? ?뺥??μ쓽 *踰덉뿭 ??븷* ???쒖뒪?쒖뿉??媛뺤젣?섎뒗 ?μ튂?낅땲??(CLAUDE.md 짠2).
# ?쒓컙????嫄몃젮??紐낆꽭瑜?癒쇱? ?곷뒗 洹쒖쑉???뺥??μ쓽 ?뺤껜?깆쓣 吏耳쒖쨳?덈떎.

set -eu

if [ $# -lt 2 ]; then
    echo "usage: $0 <task_id> <task_spec_path>" >&2
    echo "example: $0 task_sidebar tasks/task_sidebar.md" >&2
    exit 2
fi

TASK_ID="$1"
TASK_SPEC="$2"

ROOT="${ORANGE_CODE_ROOT:-$(pwd)}"
EXE="$ROOT/bin/Release/OrangeCode.exe"

if [ ! -x "$EXE" ]; then
    echo "OrangeCode.exe not found: $EXE" >&2
    exit 3
fi

# 紐낆꽭 ?뚯씪 寃쎈줈 ?뺣━ ???곷?/?덈? ?묒そ ?쒕룄.
SPEC_PATH=""
if [ -f "$ROOT/$TASK_SPEC" ]; then
    SPEC_PATH="$ROOT/$TASK_SPEC"
elif [ -f "$TASK_SPEC" ]; then
    SPEC_PATH="$TASK_SPEC"
else
    echo "task spec not found: $TASK_SPEC (relative to $ROOT or cwd)" >&2
    exit 4
fi

# ?? 紐낆꽭 寃利???
# 媛숈? 洹쒖튃??C++ 痢?(TaskSpecValidator.h) ??怨듭쑀?⑸땲?? ??吏꾩엯??紐⑤몢 媛숈? 寃?
PYTHONIOENCODING=utf-8 python - "$SPEC_PATH" <<'PY' || exit 5
import io, re, sys

path = sys.argv[1]
try:
    with io.open(path, encoding="utf-8") as f:
        text = f.read()
except OSError as e:
    print(f"紐낆꽭 ?뚯씪???????놁뒿?덈떎: {path} ({e})", file=sys.stderr)
    sys.exit(5)

# ?뱀뀡 ?ㅻ뜑 寃????以??쒖옉??"## " ?ㅼ쓬 ?쒓뎅???⑥뼱.
has_purpose = bool(re.search(r"^##\s+(紐⑹쟻|諛곌꼍)(\s|$|[.,:/(\[\-])", text, re.MULTILINE))
has_scope   = bool(re.search(r"^##\s+踰붿쐞(\s|$|[.,:/(\[\-])",       text, re.MULTILINE))
has_verify  = bool(re.search(r"^##\s+寃利?\s|$|[.,:/(\[\-])",       text, re.MULTILINE))

missing = []
if not has_purpose: missing.append("`## 紐⑹쟻` ?먮뒗 `## 諛곌꼍`")
if not has_scope:   missing.append("`## 踰붿쐞`")
if not has_verify:  missing.append("`## 寃利?")

if missing:
    print("紐낆꽭 寃利??ㅽ뙣: ?ㅼ쓬 ?뱀뀡??鍮좎죱?듬땲????" + ", ".join(missing) + ".", file=sys.stderr)
    print("?묒뾽 紐낆꽭?먮뒗 `## 紐⑹쟻` (?먮뒗 `## 諛곌꼍`) 쨌 `## 踰붿쐞` 쨌 `## 寃利? ???뱀뀡??紐⑤몢 ?덉뼱???⑸땲??", file=sys.stderr)
    print("tasks/_template.md 瑜?李멸퀬?섏꽭??", file=sys.stderr)
    sys.exit(5)

# 蹂몃Ц 湲몄씠 ???ㅻ뜑 ?쇱씤 (#??쨌怨듬갚留??쒖쇅?섍퀬 肄붾뱶?ъ씤?몃줈 ??
body_chars = 0
for line in text.splitlines():
    stripped = line.lstrip()
    if stripped.startswith("#"):
        continue
    body_chars += len(stripped)

MIN_CHARS = 200
if body_chars < MIN_CHARS:
    print(f"紐낆꽭 寃利??ㅽ뙣: 蹂몃Ц???덈Т 吏㏃뒿?덈떎 ({body_chars}?? 理쒖냼 {MIN_CHARS}???꾩슂).", file=sys.stderr)
    print("??먯씠 ?묒뾽???댄빐?섎젮硫?紐⑹쟻쨌踰붿쐞쨌寃利앹쓣 異⑸텇??????곸뼱 二쇱꽭??", file=sys.stderr)
    print("tasks/_template.md 瑜?李멸퀬?섏꽭??", file=sys.stderr)
    sys.exit(5)
PY

# tasks/ + reports/ ?붾젆?좊━ 蹂댁옣.
mkdir -p "$ROOT/tasks" "$ROOT/reports"

# 遺紐?(?뺥????먮뒗 ?ㅺ컧?? ??PID 瑜??먯떇 env 濡??멸퀎.
# ?먯떇???먭린媛 ?꾧뎄????먯씤吏 ?몄? (MESSAGES.md ?꾨즺 ?뚮┝ ??manager:<parent_pid> 濡?蹂대깂).
export ORANGE_CODE_PARENT_PID="${ORANGE_CODE_PID:-$$}"

# Windows ShellExecute 濡?spawn ??諛깃렇?쇱슫???ㅽ뻾, ?먯떇 PID 諛섑솚 X.
# bash ?먯꽌??吏곸젒 exec ?섎㈃ 媛숈? 肄섏넄??臾띠씠誘濡?start (cmd) 濡?遺꾨━.
# git-bash ??forward slash 媛 cmd /c start ?먯꽌 源⑥?誘濡?backslash 濡?蹂??
EXE_WIN=$(echo "$EXE" | sed 's|/|\\|g')

# GUI ??Windows subsystem)?대씪 cmd 媛 利됱떆 return ??start 遺꾨━ spawn ?놁씠 吏곸젒 ?몄텧?????덉쟾.
# start ??泥??곗샂???쒕ぉ) 泥섎━?먯꽌 path 遺꾧린 源⑥????먮━ ?뚰뵾.
"$EXE" --role member --task-id "$TASK_ID" --task-spec "$TASK_SPEC" \
    --bootstrap "???spawn. CLAUDE.md 짠1 留?spawn 泥??됰룞 ?곕씪媛?? ?묒뾽 紐낆꽭??\$ORANGE_CODE_ROOT/$TASK_SPEC. ?쎄퀬 ?섑뻾 ??寃곌낵瑜?reports/$TASK_ID.md ??諛뺤? ??MESSAGES.md ??supervisor:host ? manager:\$ORANGE_CODE_PARENT_PID ?묒そ?쇰줈 ?꾨즺 ?뚮┝ (4異??먭린 ?됯? 媛숈씠)." &

echo "spawned member: task_id=$TASK_ID, spec=$TASK_SPEC, parent_pid=$ORANGE_CODE_PARENT_PID"
