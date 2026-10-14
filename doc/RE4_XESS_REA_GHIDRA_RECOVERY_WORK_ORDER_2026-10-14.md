# RE4 XeSS — REA / Ghidra Large-PE Analysis Recovery Work Order

**Date:** 2026-10-14
**Branch:** feature/re4-xess
**Owner:** Local Codex, Windows 11
**Relation:** Supersedes initial-analysis execution order in RE4_XESS_REA_STATIC_RE_WORK_ORDER_2026-10-14.md until the recovery gates pass.
**Scope:** Offline analysis only. Do not modify REFramework, PR #66, OptiScaler, RE4 binaries, D3D12 runtime hooks or game behavior.

## 1. What failed and why

Reported results: exact RE4 SHA-256 A1082B154105FAC7CA22668FFC1C99A8BFC9F1B945439276C556EDEA7597A5B7 and x64 PE recognized; REA 6.4.0/Codex MCP/Ghidra 12.1.3 session bound. First function analysis entered Ghidra "ANALYZING all memory and code", did not return a function result within the Codex 300-second wait, and cleanup deleted the REA temporary project/logs. No executable runtime test was performed. These are reported local findings, not independent new experiments.

Verified in REA 6.4.0 upstream code: src/ghidra/GhidraLauncher.ts includes -import, -readOnly, -deleteProject, -postScript, but NOT -noanalysis. REA's Ghidra provider does not serve its bridge until default **whole-program** auto-analysis finishes. That holds even if the requested procedure is just one address.

- Increasing REA_GHIDRA_STARTUP_TIMEOUT_MS or invoking REA CLI can change deadlines but cannot reduce analysis scope.
- REA snapshot stores eligible **completed query results**, not a reusable Ghidra project or a way to import an independently analyzed Ghidra database.
- REA_GHIDRA_SEED_FILE adds code/function seeds before default auto-analysis, not an option to disable global analysis.
- Ghidra 12.1.4 may contain fixes but simply upgrading from the observed 12.1.3 is not a validated solution.

**Do not repeat the same REA MCP open_binary -> analyze_function or whole-file CLI call. Keep the working official MCP configuration. Do not build another MCP server or fork REA at this stage.**

Sources:
- https://github.com/morluto/rea/blob/rea-agents-6.4.0/src/ghidra/GhidraLauncher.ts
- https://github.com/morluto/rea/blob/rea-agents-6.4.0/docs/ghidra-provider.md
- https://github.com/morluto/rea/blob/rea-agents-6.4.0/docs/cli.md
- https://github.com/NationalSecurityAgency/ghidra/blob/master/Ghidra/RuntimeScripts/support/analyzeHeadlessREADME.md
- https://github.com/NationalSecurityAgency/ghidra/blob/master/Ghidra/Features/Decompiler/src/main/java/ghidra/app/decompiler/DecompInterface.java

## 2. Gate H0 — prove mapped bytes and bounded native instructions first

Do not require REA or Ghidra automatic analysis for H0.

1. Locate an immutable, non-cloud, ordinary local NTFS copy of the exact RE4 EXE. Recheck its SHA-256 and length. Do not analyze or modify the live Steam executable.
2. Inspect PE header ImageBase, sections and raw/virtual file mappings with pefile or equivalent. Record file offset, RVA, preferred image VA and runtime VA as separate coordinates. For every probe ensure the RVA belongs to actual mapped executable code.
3. Use Capstone x64 or equivalent to decode only a **64-256 byte range** from a verified offset, not full program analysis. Save the raw bytes, offsets, instructions, direct relative calls/branches and unknown indirect targets to local Evidence.
4. Starting anchors:
   - RVA 0x4470470: historical RE4 RTV resolver function/thunk; independently validate.
   - RVA 0x44AF030: historical TargetState writer method; inspect instruction/store region 0x44AF179 to 0x44AF180.
   - Candidate preferred VA 0x140AC6820 was requested previously for SceneView.get_Size. Its actual method identity/function entry is **not validated**. Derive its RVA from the measured PE ImageBase; do not assume it is SceneView merely because that name appears in REFramework reflection.
5. A decoded interior instruction is not proof of a function entry. Avoid interpreting nearby arbitrary bytes as function prologues without supporting flow evidence.

H0 PASS: at least one historical anchor with verified executable SHA, ImageBase, section, file offset, RVA, canonical VA, exact bytes and bounded x64 instructions. It need not be decompiled yet.

## 3. Gate H1 — independently use Ghidra Headless with -noanalysis

REA does not offer persistent Ghidra project reuse as a shortcut in 6.4.0. **Use Ghidra's official launcher directly**, importing the unchanged copy into a new permanent project with no default auto-analysis.

Command patterns (PowerShell; replace discovered paths, and ensure output directories exist before running):

~~~powershell
$ghidra = "C:\tools\ghidra_12.1.3_PUBLIC\support\analyzeHeadless.bat"
$projectRoot = "C:\RE4-RE\ghidra-projects"
$inputExe = "C:\RE4-RE\input\re4.exe"
$evidence = "C:\RE4-RE\evidence"
New-Item -ItemType Directory -Force $projectRoot, $evidence | Out-Null

# FIRST IMPORT ONLY — project remains on disk
& $ghidra $projectRoot "RE4Focused" -import $inputExe -noanalysis -log "$evidence\ghidra-import.log" -scriptlog "$evidence\ghidra-import-scripts.log"

# LATER RUN — reuse existing project, selected program
& $ghidra $projectRoot "RE4Focused" -process "re4.exe" -noanalysis -scriptPath "C:\RE4-RE\scripts" -postScript "RE4RvaProbe.java" "0x4470470" "0x44AF030" "0x44AF179" -log "$evidence\ghidra-probe.log" -scriptlog "$evidence\ghidra-probe-scripts.log"
~~~

Required local RE4RvaProbe.java implementation:
- A small ordinary GhidraScript (consult exact Ghidra 12.1.3 APIs as necessary), stored in C:\RE4-RE\scripts, not a REA bridge.
- Read currentProgram.getImageBase() and target memory, map only its specified RVA arguments; independently cross-check with H0 image mapping.
- For each RVA check memory membership and read at most 64-256 bytes; print mapped VA, bytes, any *already known* function/instruction entries and explicit absent metadata. Do not silently create inferred code just to make the output look complete.
- Avoid analyzeAll, analyzeChanges, global inventory, whole-program string/symbol searches, or recursive blanket processing; instrument the first run to show it did not enter whole-image auto-analysis.
- Capture Ghidra -log, -scriptlog, stdout, stderr, process exit code and surviving project directory outside REA disposable runtime.
- Do not pass -deleteProject, -readOnly, -overwrite or accidentally re-import with each subsequent probe. Verify reopening with -process succeeds.
- In first pass do **not** ask for the full call graph; with -noanalysis global xrefs and inferred functions will be incomplete by design.

H1 PASS: one -noanalysis import, permanent project preserved, successful independent -process reload, matching anchor bytes and persistent external logs. If import fails for size/resources, report exact cause, continue H0 only, and DO NOT retry the same expensive import unchanged.

## 4. Gate H2 — selectively identify and decompile *one* real function

Only after H0 and H1:
1. Validate RVA 0x44AF030 entry from direct instructions/control flow, inspect the 0x44AF179 store and related instructions, then RVA 0x4470470. Confirm observed bytes against previous PR #66 evidence.
2. In a second, specifically bounded GhidraScript, disassemble and create only a verified function or small local branch region. Verify extent and prevent unintended traversal beyond the desired range.
3. Run Ghidra DecompInterface for that validated function with an explicit per-function timeout. Capture decompileCompleted/error message, actual entry and body extent; dispose decompiler. An incomplete decompilation or type graph is an explicit limitation.
4. Expand one or two directly supported caller/callee relationships using exact callsite machine bytes. Indirect COM vtable sites can remain UNKNOWN until supporting interface/slot evidence appears.
5. Test the actual identity of candidate VA 0x140AC6820 before attributing it to via.SceneView.get_Size.
6. Return to the original REA work order's final compositor/descriptor/SRV chain **after** one valid targeted function is recovered. Do not claim a GPU consumer merely from static CPU code.

H2 PASS: at least one exact, reproducible RE4 function identity with mapped bytes and small function body/callsite evidence; completed per-function decompilation **or** informative precise failure. Never report decompiler failure as complete.

## 5. GPU evidence separation and prohibited changes

Historical PIX 112 mentions obj#7 = XeSS Handoff Output, GID 6039 writing #4782, and GID 6047 copying #4782 to backbuffer. These are useful candidate identifiers only until the original local PIX artifact is inspected and its source recorded. None proves obj#7 -> #4782 or exact downstream last reader, Present association, queue/fence eligibility, or safe output reuse.

Leave PR #66 draft/open and untouched. No runtime game launch, game-process attach, DRM/anti-tamper bypass, broad D3D12 hooks, new XeSS output rings, lifetime/marker modifications, custom REA MCP redesign or PE modifications under this recovery task.

If headless direct path fails, the permitted fallback is H0-only pefile/Capstone disassembly with a very narrow explicit branch/call ledger; alternatively, in Ghidra GUI, decline the initial full auto-analysis and analyze only a selected region, saving the independent project. Record unknown edges.

## 6. Required output from local Codex

Write doc/RE4_XESS_REA_GHIDRA_RECOVERY_REPORT_2026-10-14.md after investigation. Include H0/H1/H2 PASS/BLOCKED/FAIL, exact target SHA/PE image base, command transcripts, preserved external logs, verified mapped addresses and bytes, project persistence, decompiler status, what was learned about writer/RTV anchors, the *next smallest* final compositor evidence step and all missing GPU proof edges.

**Stop after the first independently reproducible bounded RVA/function result. The next milestone is evidence, not whole-game decompilation.**
