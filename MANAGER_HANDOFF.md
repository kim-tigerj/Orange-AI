# Manager Handoff

Current date: 2026-05-05

## State

- OrangeCode is the body of Jeong Team Lead.
- The required role name is `manager`.
- Only `manager` should appear as Jeong Team Lead's role in docs, prompts, code, and issue text.
- Cost control is mandatory, but the priority remains becoming a practical Claude Code replacement.
- Oh-Council, also called **오감독 3자 협업체계** or **Oh Supervisor 3-Way Collaboration System**, is the official collaboration protocol for Claude Code + Gemini + Codex. Read `OH_COUNCIL.md` before planning or verifying multi-model work.
- CLI work also follows Oh-Council from now on: use the protocol for important direction, architecture, UX, backend integration, or review decisions; keep routine edits local to avoid unnecessary Claude/Gemini/Codex quota use.

## CLI Oh-Council Operating Rule

- Do not spend three real LLM calls just to acknowledge the protocol.
- For small implementation fixes, 오감독 may execute locally and record the result.
- For important planning/review work, run Council Phase with Claude Code, Gemini, and Codex using the prompt shape in `OH_COUNCIL.md`.
- Council Phase is read-only: no participant edits files during discussion.
- Execution has one owner and explicit file scope.
- Tri-Review uses all three only when the result affects product direction, backend contracts, persistence, user-visible workflows, or crash risk.
- Gemini model selection is delegated to Gemini CLI unless the user explicitly configures `gemini.model`.

## Completed In This Loop

- Added shared `ManagerEnvironment` module.
- Backend child calls now receive:
  - `ORANGE_CODE_ROOT`
  - `ORANGE_CODE_ROLE=manager`
  - `ORANGE_CODE_PID`
  - `ORANGE_CODE_SESSION_KEY`
- Added `--diagnose-manager-env <path>` for zero-cost environment verification.
- Added shared manager prompt wrapping for Claude and Gemini calls.
- Confirmed Gemini model selection is delegated to Gemini CLI when `gemini.model` is empty.
- Repaired core docs to remove stale role naming.
- Fixed code-block cards so code backgrounds stay inside the assistant card.
- Added a local work card between the user request and backend response.
- Added a distinct `Mock` response source so offline tests do not look like Gemini calls.
- Added a verification card after backend completion.
- Increased spacing after labeled assistant cards so verification cards do not overlap.
- Added `ScrollLatestIntoView()` and call it after backend completion so the latest verification card is visible.
- Improved table rendering by removing visible pipe separators and expanding cell background padding.
- Upgraded table drawing to row/column grid backgrounds using `TableInfo`.
- Verification cards now show the current executable build timestamp and capture target.
- Fixed markdown link hit-testing by computing the same text origin used for rendering.
- Added multi-link mock response fixture for link alignment checks.
- Added local `/build` and `빌드` commands that run `tools/build.sh` without calling an LLM.
- Build command results are appended as work cards with exit code, elapsed time, executable timestamp, and output tail.
- Added local `/capture` and `캡처` commands that save the current OrangeCode window as PNG without calling an LLM.
- Capture command results are appended as work cards with HRESULT, output path, file size, and elapsed time.
- Added local `/verify` and `검증` command that runs build first and capture after a successful build.
- `/verify` stops with an error card if the build fails.
- Added `AttachmentStore` for `%APPDATA%\OrangeCode\attachments\<session>\manifest.json`.
- Manager prompts now include the attachment manifest path when present.
- Added zero-cost diagnostics: `--diagnose-attachments` and `--diagnose-manager-prompt`.

## Completed On 2026-05-04

- Codex global config now treats `CLAUDE.md` as a fallback project instruction file:
  - `%USERPROFILE%\.codex\config.toml`
  - `project_doc_fallback_filenames = ["CLAUDE.md"]`
  - This avoids creating per-repo `AGENTS.md` files for Claude Code-compatible projects.
- Removed the temporary root `AGENTS.md` wrapper that only pointed to `CLAUDE.md`.
- Replaced the custom-drawn input attachment glyph with the Windows system icon font:
  - `code/windows/w1/OrangeInput.h`
  - Uses `Segoe MDL2 Assets` glyph `\xE723` (`Attach`) instead of hand-drawn Direct2D lines.
- Removed hardcoded Gemini model selection from Jeong Team Lead's Gemini backend:
  - Old fixed model: `gemini-2.5-flash-lite`
  - New behavior: omit `-m` when no model is configured, letting Gemini CLI choose from its own defaults/settings.
- Added Gemini model configuration through `%APPDATA%\OrangeCode\settings.json`:
  - `code/windows/w1/AppSettings.h` loads `gemini.model`.
  - `code/windows/w1/GeminiPrintBackend.cpp` appends `-m "<model>"` only when `gemini.model` is non-empty.
  - Current local default is `"model": ""`.

## Verified On 2026-05-05

- Confirmed all three CLIs are installed and callable from PATH:
  - Claude Code: `claude.cmd --version` -> `2.1.123 (Claude Code)`
  - Gemini CLI: `gemini.cmd --version` -> `0.40.1`
  - Codex CLI: `codex.cmd --version` -> `codex-cli 0.128.0`
- Confirmed OrangeCode can call each backend individually:
  - `--test-backend claude` returned `CLAUDE_OK`, capture `tools/three-call-claude.png`
  - `--test-backend gemini` returned `GEMINI_OK`, capture `tools/three-call-gemini.png`
  - `--test-backend codex` returned `CODEX_OK`, capture `tools/three-call-codex.png`
- Confirmed OrangeCode can call all three backends from one prompt:
  - `--test-backend all` returned `Codex_COUNCIL_OK`, `Claude_COUNCIL_OK`, and `Gemini_COUNCIL_OK`
  - capture `tools/three-call-all.png`
  - screen log also recorded the three roles: `assistant-codex`, `assistant-claude`, `assistant-gemini`
- Note: the `all` test process did not self-exit after capture in one run; the test PID was manually stopped. Existing user OrangeCode process was left untouched.

## Settings Contract

OrangeCode user settings live at:

```text
%APPDATA%\OrangeCode\settings.json
```

Gemini model selection:

```json
{
  "gemini": {
    "model": ""
  }
}
```

- Empty or missing `gemini.model`: do not pass `-m`; Gemini CLI chooses the model.
- Non-empty `gemini.model`: pass `-m "<value>"` to Gemini CLI.

## Verification

```powershell
cd O:\Work\OrangeLabs\Orange\Orange-AI\code\windows\w1
& "C:\Program Files\Git\bin\bash.exe" tools/build.sh
.\bin\Release\OrangeCode.exe --diagnose-manager-env .\tools\manager-env.txt
.\bin\Release\OrangeCode.exe --test-backend mock --test-prompt "manager smoke test" --test-response-file tools\mock-code-response.md --test-capture tools\code-card-contained.png
.\bin\Release\OrangeCode.exe --test-backend mock --test-prompt-file tools\mock-prompt.txt --test-response-file tools\mock-code-response.md --test-capture tools\work-card-verification-spacing.png
.\bin\Release\OrangeCode.exe --test-backend mock --test-prompt-file tools\mock-prompt.txt --test-response-file tools\mock-table-response.md --test-capture tools\table-render-cleaner.png
.\bin\Release\OrangeCode.exe --test-backend mock --test-prompt-file tools\mock-prompt.txt --test-response-file tools\mock-table-response.md --test-capture tools\verification-real-values.png
.\bin\Release\OrangeCode.exe --test-backend mock --test-prompt-file tools\mock-prompt.txt --test-response-file tools\mock-links-response.md --test-capture tools\link-hit-alignment.png
.\bin\Release\OrangeCode.exe --test-prompt /build --test-capture tools\build-command-card.png --test-capture-ms 9000 --test-exit-ms 12000
.\bin\Release\OrangeCode.exe --test-prompt /capture --test-capture tools\capture-command-card.png --test-capture-ms 2500 --test-exit-ms 4200
.\bin\Release\OrangeCode.exe --test-prompt /verify --test-capture tools\verify-command-card.png --test-capture-ms 11000 --test-exit-ms 14000
.\bin\Release\OrangeCode.exe --diagnose-attachments tools\attachment-diagnostic.txt
.\bin\Release\OrangeCode.exe --diagnose-manager-prompt tools\manager-prompt-diagnostic.txt
```

Most recent build verification:

```powershell
cd O:\Work\OrangeLabs\Orange\Orange-AI\code\windows\w1
& "C:\Program Files\Git\bin\bash.exe" tools/build.sh
```

Result: Release build passed after the attachment icon and Gemini settings changes.

Observed diagnostic:

```text
ORANGE_CODE_ROOT=O:\Work\OrangeLabs\Orange\Orange-AI\code\windows\w1
ORANGE_CODE_ROLE=manager
ORANGE_CODE_SESSION_KEY=default
```

## Next

1. Add drag-and-drop file ingestion and copy files into `AttachmentStore`.
2. Generate WIC image thumbnails and render attachment cards.
3. Auto-register `/capture` output as an attachment.
4. Add a UI/settings surface for editing `gemini.model` if users need it; for now edit `settings.json` directly.
5. Run one real backend call after the manager prompt wrapper when quota use is approved.
6. Keep UI tests on mock backend by default.
