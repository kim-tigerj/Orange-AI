# Orange Architecture Blueprint V1: Autonomous Multi-Agent Factory

## 1. 由ъ꽌移?寃곌낵 ?붿빟 (Research Synthesis)

### 1.1. ?꾩씠??Aider) ?듭떖 湲곗닠 ?대?
- **吏?ν삎 ?덊룷 留?(Repo Map)**: `universal-ctags`? `tree-sitter`瑜??듯빐 ?뚯뒪肄붾뱶???щ낵(?⑥닔, ?대옒???? 愿怨꾨? 異붿텧?섍퀬, ?대? **PageRank ?뚭퀬由ъ쬁**?쇰줈 媛怨듯븯??媛??以묒슂??1024 ?좏겙 遺꾨웾??吏?꾨? ?앹꽦?⑸땲??
- **?섏닠???몄쭛 ?꾨줈?좎퐳 (Surgical Edit)**: `SEARCH/REPLACE` 釉붾줉 諛⑹떇???ъ슜?섏뿬 LLM???좎씪??臾몄옄??留ㅼ묶???듯빐 ?뚯씪???섏젙?섎룄濡?媛뺤젣?⑸땲?? ?대뒗 ?쇱씤 踰덊샇???섏〈?섎뒗 Unified Diff蹂대떎 LLM ?섍꼍?먯꽌 ?⑥뵮 寃ш퀬?⑸땲??

### 1.2. ?ㅼ쨷 ?먯씠?꾪듃 ?묒뾽 踰ㅼ튂留덊궧
- **MetaGPT (SOP 湲곕컲)**: 'Code = SOP(Team)' 泥좏븰 ?섏뿉 媛??먯씠?꾪듃??異쒕젰???뺥삎?붾맂 ?곗텧臾?PRD, Design, Code)濡?洹쒓꺽?뷀븯怨?Pub/Sub 紐⑤뜽濡?怨듭쑀?⑸땲??
- **Devin (?먯쑉 寃利?**: 怨꾪쉷 ?섎┰ -> ?ㅽ뻾 -> ?먮윭 愿痢?-> ?먯쑉 ?섏젙??猷⑦봽瑜??낅┰??VM ?섍꼍?먯꽌 ?섑뻾?섏뿬 ?좊ː?깆쓣 ?뺣낫?⑸땲??

## 2. Orange Code 珥덇꺽李??ㅺ퀎 (The "Orange" Advantage)

### 2.1. 蹂묐젹 ?쒓컙 ?뺤텞 (Parallelism via SQLite)
- ?꾩씠?ㅼ? ?щ━, ?곕━??以묒븰 DB瑜?'Global Message Pool' 諛?'Job Queue'濡??ъ슜?섏뿬 **N媛쒖쓽 Worker瑜??숈떆 媛??*?⑸땲??
- DB?????⑥쐞 ?좉툑(Row-level Lock)怨??섏〈??愿由?`depends_on`)瑜??듯빐 異⑸룎 ?녿뒗 蹂묐젹 ?섏닠??蹂댁옣?⑸땲??

### 2.2. 4異?由щ럭 ?먮룞??(Strict Quality Gate)
- Worker媛 ?섏닠???꾨즺?섎㈃, DB ?대깽?몃? ?듯빐 利됱떆 ?ㅻⅨ ?몄뒪?댁뒪(Supervisor)媛 諛곗젙?⑸땲??
- Supervisor??DB?먯꽌 ?섏젙 ?꾨Ц怨?鍮뚮뱶 濡쒓렇瑜??쎌뼱 4異?湲곗?(湲곕뒫/援ы쁽/愿怨??쒖뒪???쇰줈 ?낅┰ 寃利앹쓣 ?섑뻾?⑸땲??

### 2.3. ?ㅽ뙣???먯궛??(Database-driven Intelligence)
- 紐⑤뱺 鍮뚮뱶 ?먮윭 ?⑦꽩怨?由щ럭 諛섎젮 ?ъ쑀瑜?DB??'吏???먯궛'?쇰줈 ??ν빀?덈떎.
- ?뺥???Manager)? ?덈줈???묒뾽??Worker?먭쾶 ?좊떦???? ?대떦 ?뚯씪??怨쇨굅 ?ㅽ뙣 ?щ?瑜?荑쇰━?섏뿬 **'Negative Prompt(?ㅽ뙣 ?덈갑 吏移?'**瑜??먮룞?쇰줈 ?앹꽦???ы븿?⑸땲??

## 3. 援ы쁽 濡쒖쭅 (Implementation Logic)

### Step 1: SQLite 湲곕컲 Repo Map ?붿쭊
- `tree-sitter` (C API)瑜??꾨줈?앺듃???뺤쟻 留곹겕?섏뿬 AST 湲곕컲 ?щ낵 異붿텧湲곕? 援ы쁽?⑸땲??
- 異붿텧???щ낵怨??몄텧 愿怨꾨? `CodeSymbols`, `CodeReferences` ?뚯씠釉붿뿉 ?몃뜳?깊븯怨? SQL???ш? 荑쇰━(CTE)濡?PageRank ?쒖쐞瑜?怨꾩궛?⑸땲??

### Step 2: ?뺥??μ쓽 '紐낅졊 踰덉뿭湲? (Instruction Generator)
- ?뺥???紐⑤뜽?먭쾶 ?꾩슜 ?쒖뒪???꾨＼?꾪듃瑜?遺?ы븯???ъ슜?먯쓽 ?섎룄瑜?`Target File` + `SEARCH/REPLACE Context`濡?蹂?섑빀?덈떎.
- DB??`Tasks` ?뚯씠釉붿뿉 ???뺥삎?붾맂 紐낅졊 ?⑦궎吏瑜???ν빀?덈떎.

### Step 3: ?먯쑉 ?쇰뱶諛?猷⑦봽 (Self-Correction)
- Worker???묒뾽 以?鍮뚮뱶 ?먮윭 諛쒖깮 ?? ?먮윭 ?꾨Ц??DB??湲곕줉?섍퀬 ?ㅼ뒪濡?`Diagnostic Turn`???섑뻾?섏뿬 ?섏닠??蹂댁셿?⑸땲??
- 理쒖쥌 ?깃났 ?쒖뿉留??뺥??μ뿉寃??꾨즺 ?좏샇瑜?蹂대깄?덈떎.

## Decision 2026-05-05: Oh-Council Direction Check For Jeong Team Lead

Claude Code, Gemini, and Codex were asked in read-only Council Phase to verify
the development direction for OrangeCode / 정팀장.

Verdict: continue the current direction. 정팀장은 a Windows native coding
workbench, not a general chat app. Chat remains the input surface, while diffs,
logs, task state, build output, capture evidence, attachments, worker reports,
and persistent handoff become first-class product objects.

Council consensus for the next phase:

1. Stabilize the persistent work-state model: goals, projects, chats, tasks,
   worker reports, build results, capture artifacts, and attachments.
2. Complete a mock-first manager loop: request, context summary, task split,
   bounded worker instruction, result collection, build/test/capture, final
   report, and durable handoff.
3. Keep evidence visible in the UI instead of burying code, diffs, logs, and
   captures inside plain chat text.
4. Finish attachment correctness: original files/images must be preserved
   separately from display thumbnails.
5. Isolate backend failures and enforce cost control: Claude, Gemini, and Codex
   adapters must not let one CLI failure break the common execution loop.

Execution assignment:

- Default execution owner: Claude Code for Windows app/body implementation.
- Codex review focus: schema, backend abstraction, build/test evidence,
  regression risk, and documentation.
- Gemini review focus: product direction, UX flow, evidence presentation, and
  broad integration risks. Gemini backend stability remains a separate bounded
  task.

Reopen Oh-Council if any of these occur:

- `chief` or another role name reappears for 정팀장 instead of `manager`.
- The mock manager loop cannot complete end to end.
- SQLite recovery cannot restore trustworthy work state after restart.
- Build/capture evidence can be skipped while still reporting completion.
- Gemini, Claude, or Codex backend errors leak into the shared manager loop.
- Real LLM calls grow without an explicit budget reason.
- The product drifts back into a general chat UI where task state and evidence
  are secondary.

## Decision 2026-05-05: Left Sidebar Direction

Claude Code, Gemini, and Codex were asked in read-only Council Phase to verify
the in-progress left sidebar work.

Verdict: continue the current `CSidebar` direction. The sidebar should benchmark
Claude app navigation shape, but its product meaning is different: root
`정팀장` is the global coordination area, with global chats under it. Goal and
project groups appear only when real goal/project data exists.

The next implementation step is not more visual polish. The next step is to
bind `CSidebar` to real SQLite/goals/projects/chats data and prove behavior
parity with the old LISTBOX path before removing the old path.

Now the sidebar should show:

- root `정팀장`
- real global chats from SQLite
- `새 대화` as a draft action, not a persisted empty session
- current chat accent
- last active time
- real Goal/Project groups only when their data exists

Later, after the work model is stable, add:

- goal/project progress
- build/capture/evidence status
- worker report and supervisor review status
- failure/recovery indicators
- attachment summaries

Review focus:

- no mock marketing tree or fake Goal/Project/Task rows
- global chats and goal chats are separated correctly
- empty draft chats do not accumulate in SQLite
- double-click session switching remains stable
- right-click context menu targets the correct row
- Korean text, scroll hit-testing, tooltip text, and DPI behavior are verified
- Release build and capture pass before the old LISTBOX/GDI path is deleted

Reopen Oh-Council if the sidebar UI diverges from SQLite state, mock rows appear,
session switching becomes unstable, draft sessions persist incorrectly, or
visual polish starts before real navigation behavior is proven.
