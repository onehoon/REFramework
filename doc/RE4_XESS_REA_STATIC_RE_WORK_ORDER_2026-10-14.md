# RE4 XeSS — REA Installation, Local Codex MCP Setup, and Targeted Reverse-Engineering Work Order

> **2026-10-14 execution override:** The initial REA 6.4.0 + Windows Ghidra attempt reached full-image auto-analysis and timed out before returning a function. **Do not repeat the original Phase S -> first analyze_function workflow unchanged.** Follow [RE4_XESS_REA_GHIDRA_RECOVERY_WORK_ORDER_2026-10-14.md](RE4_XESS_REA_GHIDRA_RECOVERY_WORK_ORDER_2026-10-14.md) first: PE/RVA bytes -> Ghidra direct -noanalysis persistent project -> one bounded function. The research questions below remain valid; the original execution sequence is superseded until recovery passes.

**Date:** 2026-10-14  
**Repository:** https://github.com/onehoon/REFramework  
**Work-order location / requested branch:** feature/re4-xess / doc  
**Execution owner:** local Codex on the Windows 11 development machine  
**Target:** Resident Evil 4 (2023), native D3D12, REFramework RE4-only XeSS bridge  
**Type:** investigation + local tooling setup + evidence report; **no production code change authorized**  
**Initial REA baseline:** morluto/rea, npm package rea-agents 6.4.0 (released 2026-10-11)

## 0. Mission / required outcome

Install and validate **REA (Reverse Engineer Anything)** locally, connect its official MCP server and bundled skill to local **Codex** where useful, then perform a narrowly targeted, reproducible **static reverse-engineering investigation of the exact RE4 1.5.9.0 x64 executable**. Use our existing engine/source evidence to answer **which RE4 CPU-side render/composite/descriptor-binding path could consume an installed XeSS handoff output**. Reconcile that path with the **actual GPU consumer -> queue submission -> Present -> downstream retirement** evidence gap in REFramework PR #66.

The purpose is not to "reverse engineer all of RE4" or rewrite its renderer. The concrete desired result is an **address- and call-chain-supported set of RE4-owned function sites** suitable for a subsequent, separate, tightly scoped runtime or GPU-capture experiment.

**Success is an auditable finding, including explicit unknowns, not a plausible story.** If static work cannot establish a consumer, report the precise missing edge and the cheapest next discriminating capture. Never promote a static call, a RenderContext equality, a submitted fence signal, or a fullscreen draw into proof that a particular GPU resource was read.

### Mandatory execution order

1. Inspect repository/branch reality and establish the exact artifacts/hashes.
2. Install/test REA + Ghidra and connect official REA MCP to Codex; use CLI fallback if MCP is not working.
3. Execute bounded RE passes, highest priority first.
4. Correlate with earlier RE4 research and PR #66, keeping source identity and dates explicit.
5. Produce complete evidence-backed reports and a minimal follow-up capture plan.
6. Stop. Do not change REF/OptiScaler runtime or open a production implementation PR merely from static hypotheses.

## 1. Source-of-truth and architectural boundaries

Read these before touching local tools:

- https://github.com/onehoon/REFramework/blob/feature/re4-xess/doc/RE4_XESS_BRIDGE_ARCHITECTURE_AND_RE_STATUS_2026-09-26.md
- https://github.com/onehoon/REFramework/blob/feature/re4-xess/doc/RE4_XESS_PRODUCTION_ARCHITECTURE_2026-09-27.md
- https://github.com/onehoon/REFramework/pull/66 — **separate, active diagnostic/implementation branch**, not identical to feature/re4-xess.
- https://github.com/onehoon/REFramework/blob/feature/re4-xess-load-state-accessor-diagnostic/doc/RE4_XESS_GPU_OUTPUT_CONSUMER_IDENTIFICATION_EVIDENCE_2026-10-01.md (verify existence/ref when running; PR #66 can advance).
- Review current PR #66 source for: src/mods/re4_xess/RE4XeSSOutputHandoff.*, RE4XeSSD3D12.*, RE4XeSSWorker.*, RE4XeSSLifetimeTrace.hpp, RE4XeSSWriterChain.hpp, shared/sdk/Renderer.*, and relevant D3D12Hook and renderer callbacks.
- Historic diagnostic branch refactor/re4-temporal-diagnostic and Captures 1–30b are reference evidence only; do not merge them as production code.
- Previously supplied PIX/GPU capture 112 and deep RE notes may exist only in the local RE4 analysis directory. Discover/validate the actual files; do not assume that the GitHub branch includes them.

Project invariants:

- The production module is RE4-only, built inside REFramework; no separate ATSBridge/ASI DLL, no PDPerfPlugin dependency.
- The game-facing producer calls the **public XeSS D3D12 API**, optionally intercepted by **stock OptiScaler**. No private REF-to-OptiScaler ABI.
- The existing XeFG/OptiScaler compatibility subsystem is protected. XeFG stays **OFF** in research runtime comparisons unless a subsequent experiment is explicitly authorized.
- HUDless HDR scene color -> XeSS SR -> native RE4 overlay/UI/final presentation is the intended architecture. Direct copying the XeSS output to swapchain is **not** an authorized replacement.
- Respect original RE Engine/REFramework semantics outside the RE4 XeSS compatibility surface; never "fix" general REFramework behavior differently from upstream as a by-product of the investigation.
- Do not weaken third TargetState ownership/quarantine, output lifetime, marker-pending, fence, reset-history, resize, or non-RE4 safety rules to make a trace look successful.
- All activity under this work order is **read-only against the game executable**. Do not patch protected binaries, defeat DRM/anti-tamper, dump secrets, or implement bypasses.

## 2. Known facts and non-facts to preserve

### 2.1 Binary identity (verify locally)

| Property | Investigated value |
| --- | --- |
| Game | Resident Evil 4 (2023) / re4.exe |
| Version | 1.5.9.0 |
| Steam App ID | 2050650 |
| Historical BuildID | 22377325 |
| D3D renderer | x64 D3D12 |
| Size in canonical 2026-09-26 research | 233,689,576 bytes |
| SHA-256 | A1082B154105FAC7CA22668FFC1C99A8BFC9F1B945439276C556EDEA7597A5B7 |
| Historical game directory | E:\SteamLibrary\steamapps\common\RESIDENT EVIL 4  BIOHAZARD RE4 |

Do not use a known RVA against a mismatched binary. Record current exe file length, timestamp, SHA-256, PE image base, sections, machine, checksum (if present), import table, and REA/Ghidra analysis profile. **Distinguish file offset, RVA, preferred-image VA, and runtime ASLR-adjusted VA**. The historical game-directory spelling is not an installation guarantee: locate the actual file.

### 2.2 Established/previous candidate anchors

| Anchor | Prior meaning / required handling |
| --- | --- |
| RVA 0x4470470 | RE4-specific create-render-target-view resolver target / thunk reported for RE4 1.5.9.0; verify bytes and true function ownership before using. |
| RVA 0x44af030 | Previous PR #66 TargetState-writer investigation method RVA; inspect function/callers and writer sites in exact image. |
| RVA 0x44af179 -> 0x44af180 | Previously identified narrow writer store/instruction interval; independently confirm the disassembly/semantics, not just the label. |
| Capture 30b | Candidate final fullscreen draw: DrawInstanced(3,1,0,0), in final color-readable / swapchain-RT window, last engine DIRECT submission before Present; no exact SRV/resource proof. |
| PR #66 | Successful XeSS execution/output installs have been observed in previous runs; **GPU output consumer and reader-to-Present ownership are NOT PROVEN**. Verified writer-chain acceptance does not close the output-consumer gate. |
| Local PIX 112 notes | Prior investigation discussed obj#7 = RE4 XeSS Handoff Output (R11G11B10_FLOAT 1920x1080), GID 6039 writes #4782, GID 6047 copies #4782 to backbuffer; **obj#7 -> #4782 was not established**. Confirm identity and event references from actual local capture before citing as evidence. |

The 2026-10-01 PR #66 output consumer report records an output install and a plausible downstream draw/Present window, but **not** the bound SRV resource, actual consumer command list, or safe last-read-to-fence lifetime edge. More recent captures and the local PIX 112 result may supersede some details; report exactly which dated evidence comes from which build.

An older Pix capture's "Gather Accessed Resources" success does not imply that the resource's read was recorded. Missing PIX events do not by themselves prove absence of an RE4 reader.

## 3. Official REA sources, support boundaries, and permissions

Pin initial investigation behavior to official **rea-agents 6.4.0**, then only move versions after documenting why:

- Source: https://github.com/morluto/rea
- Release: https://github.com/morluto/rea/releases/tag/rea-agents-6.4.0
- Install: https://github.com/morluto/rea/blob/rea-agents-6.4.0/docs/installation.md
- CLI: https://github.com/morluto/rea/blob/rea-agents-6.4.0/docs/cli.md
- Windows Ghidra: https://github.com/morluto/rea/blob/rea-agents-6.4.0/docs/windows-ghidra-p0.md
- Ghidra semantics: https://github.com/morluto/rea/blob/rea-agents-6.4.0/docs/ghidra-provider.md
- MCP: https://github.com/morluto/rea/blob/rea-agents-6.4.0/docs/mcp-contracts.md
- Native primitives: https://github.com/morluto/rea/blob/rea-agents-6.4.0/docs/native-investigation.md
- Open roadmap for general live native debugger: https://github.com/morluto/rea/issues/1267

REA/Ghidra on Windows is **experimental x64, read-only P0**. Native x64 Windows PE applications and DLLs are supported, but this is not a full game debugger or arbitrary in-process native instrumentation service. Do not assume Windows Hopper-equivalent functionality, native runtime breakpoints, general Windows attach, PE load-image attestation, or GPU event observation. A decompiler output is a hypothesis with machine-code provenance, **not original source code**.

REQ: use **the official shipped REA MCP server** rather than writing a new MCP provider from scratch. An auxiliary local adapter is only considered under the conditional rules in section 4.4.

## 4. Phase S — local installation and Codex MCP commissioning

### 4.1 Preflight and secure preparation

On the Windows 11 development host:

- Run PowerShell under the normal user identity. Record Windows version, CPU/RAM, available local **NTFS** disk space, installed Node/npm, Java, Ghidra and Codex version/paths. Avoid running tools as Administrator unless separately required and justified.
- REA requires Node.js **22.x >=22.19, 24.x >=24.11, or 26+**. Check the actual release's constraints and select a supported version.
- For Windows Ghidra P0, install a legitimate **Ghidra 12.1.x** distribution (12.1.4 verified by upstream) and a full **64-bit JDK >=21** in Ghidra's declared Java range. REA does **not** install these prerequisites for you.
- Verify official distribution/integrity/source. Preserve existing tools and Codex configuration; do not overwrite unrelated MCP servers or skill registrations.
- Work with an unchanged copy of re4.exe stored in a fixed, ordinary local NTFS directory, e.g. C:\RE4-RE\input\re4.exe. Do not use a SUBST drive, symlink/junction/reparse component, network UNC path, alternate data stream, unstable or live-mutating target, cloud-sync folder, or working Steam executable as the REA input. This avoids Windows P0 admission failures and accidental changes to a running game. Rehash both input and original after copying.
- Keep large decompilation/Evidence JSON, caches, executable copies and raw PIX exports **outside Git** and outside tracked doc paths; place only reviewed reports, bounded excerpts, metadata/hashes and scripts where permitted.
- Do not upload the copyrighted RE4 executable, game assets, binary snapshots, proprietary OptiScaler DLLs, user data, or raw memory dumps to GitHub/third-party services.

Suggested staging layout (not prescriptive if local host differs):

~~~text
C:\RE4-RE\
  input\re4.exe                 # copied, immutable, hash-checked
  evidence\                    # local-only raw REA JSON, Ghidra outputs
  pix\                         # local-only capture identifiers/exports
  notes\                       # local-only scratchpad
  reports\                     # reviewed Markdown and small structured tables
~~~

### 4.2 Install and test official REA

The following are **verified command forms from upstream CLI/Windows documentation**. Adapt paths after discovering the installed distributions.

~~~powershell
node --version
npm --version
npm view rea-agents@6.4.0 version

npm install --global rea-agents@6.4.0
rea --version
rea --help

$env:GHIDRA_INSTALL_DIR = "C:\tools\ghidra_12.1.4_PUBLIC"
$env:JAVA_HOME = "C:\tools\jdk-21"
$env:REA_ANALYSIS_PROVIDER = "ghidra"

rea doctor --provider ghidra --json
rea providers --json
rea capabilities --json
~~~

Values under C:\tools above are examples only. Check actual installation directory and JDK home. Run provider diagnostics **before** configuring the MCP client. A global rea install is optional if the approved official pinned package-runner registration works, but use the exact same version for repeatability.

For this large 233 MB native target, start with the default Ghidra startup ceiling and monitor memory. An optional environment setting such as GHIDRA_HEADLESS_MAXMEM=2G or 4G can be evaluated based on **observed** host capacity and import memory, not blindly forced. This heap does not cap total RSS. The REA Ghidra provider defaults to 330,000 ms for initialization; REA_GHIDRA_STARTUP_TIMEOUT_MS controls provider startup, **not the Codex MCP tool-call timeout**.

Preflight acceptance:
- REA launches and reports its exact package version.
- Native Windows control bundle, path admission, Ghidra installation and JDK checks pass for the selected provider.
- A tiny *non-proprietary* x64 PE (existing suitable fixture or compiled test program) completes read-only overview and decompile without execution.
- Failure includes exact error code, attempted remedy and final status; do not replace the official implementation solely because of a missing prerequisite.

### 4.3 Install the official Codex MCP server and skill

First use the official **setup** flow. The configuration mutation must be reviewed and applied **only for Codex**, not all detected clients.

~~~powershell
rea setup --client codex --dry-run
# Review proposed Codex MCP and skill paths/other edits.
rea setup --client codex
# Approve the preview when prompted.

rea doctor --client codex --json
rea doctor --skill --json
~~~

When the setup cannot operate interactively, inspect the exact REA 6.4.0 behavior first, then use its supported explicit scoped --client codex and --yes form; never use --all-detected accidentally. Setup should preserve other Codex MCP servers and settings. Restart local Codex after registration.

**Verify in an actual local Codex session**, not just from a healthy CLI:
1. Official REA MCP server appears and negotiates tools.
2. Invoke binary_session({}) to inspect selected provider availability; it is a discovery operation, not proof of a loaded target.
3. Open a small known native x64 PE with open_binary({path, provider_id:"ghidra"}) using the live advertised schema.
4. Invoke binary_overview (or an equivalently advertised read-only provider query), then a function inspection. Verify correct returned file identity.
5. Close with close_binary and check reported resource cleanup. Do not silently keep orphan Ghidra processes.
6. Repeat the cold-start readiness against the **copied** RE4 binary if time/resources permit. A target-open reply alone is not Ghidra auto-analysis completion: that typically happens on the first actual provider query.

The standard Codex MCP tool timeout may be shorter than the Ghidra cold import. Diagnose this explicitly. REA docs distinguish **MCP initialization**, **REA Ghidra startup (default 330 s)** and **individual MCP request deadlines (SDK default often 60 s)**. If local Codex exposes a per-tool request timeout, extend appropriately based on measured import; do not confuse Codex server startup_timeout_sec with the tool-call deadline. Prefer CLI for a large first import if Codex cannot sustain the call. If interrupted, inspect binary_session, close_binary, verify cleanup, reopen, then retry; do not run multiple identical heavy imports concurrently.

### 4.4 MCP implementation is conditional — no redundant custom REA server

If setup succeeded, **no new MCP project/code is needed**. Keep using shipped official REA functions and the version-matched bundled skill.

If setup failed:
1. Inspect REA's documented direct-registration example for Windows, identify the actual installed node.exe and REA scripts/rea.mjs, and configure **only the existing Codex MCP entry** with the same provider variables; preserve the rest of config.
2. Verify process/stdio contract, tool discovery, permission constraints and exact error. Fix install/paths/client compatibility before writing software.
3. Only if a demonstrable Codex integration limitation remains, a minimal local-only shim may be developed to call **official REA CLI/MCP** and expose the few required read-only tasks. It must not reimplement a native decompiler, binary parser, GPU tracer, x64 debugger, or REA's provider; it must not become an REFramework production dependency.
4. Record its necessity, precise limitation, inputs/outputs, process cleanup, version pin, and tested Codex call. Store optional helper **outside production runtime paths**, reviewed source only; no long-running network service, remote upload, persistent privileged daemon or automatic REA fork.
5. If the shim is still blocked, continue the investigation with documented official REA CLI and report that MCP was unavailable; do not let infrastructure redesign consume the RE budget.

For reference, REA's Windows documentation describes **Node** launching installed scripts/rea.mjs with the mcp argument and GHIDRA_INSTALL_DIR/JAVA_HOME/REA_ANALYSIS_PROVIDER in the environment. Do not blindly paste example absolute paths or unverified TOML structure; use the registered format actually accepted by the installed Codex version and verify tool invocation.

### 4.5 Bounded first RE4 analysis / CLI fallback

The upstream documented native commands are:

~~~powershell
$exe = "C:\RE4-RE\input\re4.exe"
Get-FileHash $exe -Algorithm SHA256

rea analyze   $exe --provider ghidra --json
rea search    $exe "CreateRenderTargetView" --provider ghidra --json
rea function  $exe "FUNCTION_NAME_OR_CANONICAL_VA" --provider ghidra --json
rea decompile $exe 0xKNOWN_CANONICAL_VA --provider ghidra --json
rea xrefs     $exe 0xKNOWN_CANONICAL_VA --provider ghidra --json
rea trace     $exe "SEARCH_TEXT" --provider ghidra --json
~~~

**FUNCTION_NAME_OR_CANONICAL_VA / 0xKNOWN_CANONICAL_VA / SEARCH_TEXT are placeholders, not runnable candidate addresses.** First obtain the program image base and translate verified historical RVAs into Ghidra canonical addresses; use the actual CLI --help and provider responses. Do not run a guessed address as an assertion.

CLI commands each begin a separate process. For repeated queries, use REA's documented --snapshot mechanism to preserve compatible exact query results; snapshot is **not** a reusable live Ghidra project. On MCP, use one persistent open_binary session and selectively inspect analyzed functions. For giant machine-readable responses, prefer targeted function queries, finite excerpts and Evidence summaries. Capture JSON only to local files, avoid pasting entire decompilation corpora into the model context or Git.

If the REA session refuses the game image due to genuine host/path/resource constraints, record the precise constraint and assess a limited stand-alone **Ghidra headless** pass as a fallback; do not claim that REA completed the analysis. Never weaken REA's native path, DACL or process ownership security checks to make a blocked file load.

## 5. RE research request — questions, method, and proof obligations

**Work only on the verified exact binary.** Treat names such as SceneView, TargetState and Overlay in our REF SDK as semantic hints, not proof of identical Ghidra function names. Every proposed RE4 function identity must cite entry VA/RVA, key machine instructions or bytes, callers/callees, and at least one independent reason it implements the claimed behavior.

### Pass R0 — build identity and REA function map

Questions:
- Is this exactly the game image for the historical resolver and writer RVAs?
- What are the PE sections/image base, import thunks, D3D12/DXGI imported calls and likely renderer utility wrappers?
- Which of the known historical anchor RVAs fall into well-formed Ghidra functions? Are they thunks, stubs, code, data or false-positive matches?

Method:
- SHA-256 and copy integrity preflight. Open via REA/Ghidra.
- Query binary overview, function dossier, instructions/decompile, xrefs/call targets around exact known RVAs.
- Preserve each coordinate system and note any unresolved dynamic/virtual call.
- Build a compact named analyst map with anonymous labels **in the report**; do not pretend they are debug symbols.

Deliverables:
- binary_identity.json (small metadata; no binary), analysis environment and address conversion note;
- initial anchor table with exact instruction, call/reference provenance, confidence and limitations.

### Pass R1 — RE Engine render stages and final-screen CPU path (highest priority)

Questions:
1. After the pre-Overlay XeSS handoff is installed, which RE4 engine functions create and record the later Overlay, post-effect/final compositor and present-related D3D12 commands?
2. Where does Overlay::get_main_target_state() or an equivalent field travel into a native ID3D12Resource / SRV descriptor?
3. Does a later compositor fetch the state live, use a cached view/descriptor, or select an entirely different source?
4. Is the final fullscreen-triangle candidate from Capture 30b represented in a unique RE4-owned **draw-recording path**? How many similar draws exist and what discriminates them?
5. Where does RenderContext's current TargetState differ from the Overlay's main TargetState? Does the C++ method use a virtual dispatch or wrapper whose static target is unresolved?

Method:
- Start from *validated* renderer/TargetState helper anchors and field accesses, then follow **both callers and callees** through pre-Overlay, post-Overlay, prepare-output and output-present families.
- Search for D3D12 command-recording interfaces and call patterns (DrawInstanced, Dispatch, CopyResource/CopyTextureRegion, ExecuteCommandLists, OMSetRenderTargets, SetGraphicsRootDescriptorTable, SetDescriptorHeaps, CreateShaderResourceView, CopyDescriptors*, ResourceBarrier / ID3D12GraphicsCommandList7::Barrier where present).
- D3D12 COM methods can be **vtable indirect calls** rather than ordinary PE imports. Use interface vtable slot identity and argument setup when possible; do not infer a concrete API from slot number without interface/version evidence.
- Identify shader-resource binding preparation upstream of draw recording: CPU descriptor handle creation, copy/staging paths, GPU descriptor handles/root descriptor tables, descriptor heap allocation and per-frame reuse; record alias/overwrite possibilities.
- Follow target-to-native-texture methods through enough code to map the **candidate** texture pointer/descriptor identity. Distinguish buffer pointer identity from descriptor/value identity and the resulting physical GPU resource.
- Search only narrow function neighborhoods after the initial traversal; 1,704 generic indirect-call candidates in the historical brute-force scan were not a useful verified final path. No unguided global hooking.

Mandatory output: a **CPU-side producer-to-consumer graph** with function VAs/RVAs and source/reference observations; mark all edges as CONFIRMED_STATIC, INFERRED_STATIC, or UNKNOWN. A CPU binding candidate by itself does not demonstrate the actual GPU read on the captured frame.

### Pass R2 — TargetState ownership and quality-switch writer reconciliation

Questions:
- What does the exact RVA 0x44af030 function do? What are the semantics of the 0x44af179–0x44af180 store interval in the **exact matched PE**?
- What owner allocates/clones/disposes or refcounts TargetState and its native resource? Which functions can replace the Overlay slot during quality/resize/view transitions?
- Where are the equivalent intrusive reference increments/decrements or engine lifetime protections? Does this affect the identity used by the final compositor?
- Can the accepted PR #66 handoff -> A -> B writer-chain be related to a concrete static engine call chain, **without** invalidating PR #66 runtime-established writer acceptance?

Method:
- Start at those RVAs and verified TargetState/RTV thunk; compare assembly with the REFramework hooks and verified runtime stores.
- Reverse-reference the field store/copy/call chain and cross-check write semantics with function inputs/output and memory offsets.
- Produce object ownership/lifetime table, explicitly separating engine object refcount, native D3D12 COM resource lifetime and the downstream GPU last read.
- Treat any inferred third writer as a candidate until there is a runtime observed address/owner. Do not restore a foreign TargetState or change quarantine code.

Priority: lower than R1 if CPU consumer gap remains wide; stop when marginal work is no longer likely to distinguish the consumer.

### Pass R3 — D3D12 producer/reader and PIX object correlation

Questions:
- Which CPU D3D12 recorder submits the final draw/copy associated with historical Capture 30b and the local PIX object/event notes?
- Is there a **proven static assignment/descriptor chain** allowing RE4 to sample the installed handoff output, or only a plausible candidate?
- Can the static code distinguish a direct handoff-sampled composite, an intermediate-copy/render pass (candidate #4782), and a path that ignores the handoff?
- What observation would uniquely distinguish the competing theories in **one** targeted GPU frame?

Method:
- Build an edge ledger: handoff TargetState -> native ID3D12Resource -> CPU SRV descriptor write -> copy to GPU-visible heap / root binding -> draw/dispatch/copy -> intermediate output -> backbuffer -> Present.
- Every edge carries: input/output resource/descriptor identities where known, function entry and call site, frame/queue evidence if available, and evidence classification.
- For local PIX 112, open the *actual original capture/event export* if available and record: file hash/name, capture application/version, queue GID/event IDs, resource descriptions and actual accessed-resource panels. Do not convert notes into firsthand verified events.
- If resource obj#7 lacks recorded read events, mark the reader **UNKNOWN**, not "never read". If GID 6039 writes #4782 and GID 6047 copies it to the backbuffer, those facts alone do not establish #7 as upstream of #4782.
- If PIX or RenderDoc has a concrete target draw, determine the nearest exact D3D12 command-list recorder / descriptor setup; explain how an exported shader bind/root signature/source resource could be correlated to the REA function.

**Critical proof gate:** A verified actual read of the **exact installed XeSS resource on an identified GPU command list**, and that list's queue/Present/retirement ordering, still requires runtime/GPU evidence. Static REA cannot close this gate alone.

### Pass R4 — queue, marker, fence and lifetime classification

Questions:
- What RE4 queue/list recording/submission path does the candidate consumer follow, relative to the REF XeSS writer submit and post-Present marker?
- Can the producer's writer completion fence be incorrectly confused with the downstream consumer's final read/retirement?
- What minimal capture would prove the consumer list is submitted before an eligible retirement signal and its completion?

Method:
- Map static ExecuteCommandLists / signal / wait and command-allocator ownership paths for the narrow identified engine renderer. Use PR #66's true callback/queue observations, preserving thread/frame differences.
- Distinguish API call sequence, queue submission, fence signal enqueued, actual GPU completed value, and last GPU read. They are different facts.
- Report whether same-queue ordering is *supported*, *proven for a concrete submission*, or *unknown*. Do not infer from adjacent callbacks, a queue pointer, frame number or a signaled marker alone.
- Recommend **no output ring, forced wait, synthetic signal, early Release, or marker timing change** unless a later validated GPU-lifetime design authorizes it.

### Pass R5 — SceneView and HUD/ESC flicker attribution (secondary)

Questions:
- Which validated primary SceneView determines internal render extent and which views are secondary/loading/menu?
- Is a reduced-resolution and jittered SceneView frame possible when later XeSS OutputHandoff fails/skips for marker-pending? Which HUD/ESC layers are composited before or after the installed output?
- Is there an actual native-size fallback decision before the rendering work, or merely an after-the-fact failure?

Method:
- From SDK callback and RE4 method accesses, map real read/write/control flow around SceneView.get_Size, camera jitter, Overlay compositing and menu draw dispatch. Distinguish primary and secondary view identities.
- Use previous REFramework logs/PIX only for the frames they actually measured. Runtime UI flicker is user-observed, but its exact layer/compositor cause is not proved.
- Explain whether observations affect render-size admission policy, downstream resource selection, or UI presentation, **without coding a fix**.

Secondary work must not delay R1-R3's consumer result. If no valid evidence exists, return the explicit unknown and required visual/frame capture.

## 6. Required evidence discipline

For each claimed function and graph edge, include:

| Field | Required contents |
| --- | --- |
| Artifact identity | executable SHA-256, exact game build; provider name/version and REA analysis profile |
| Location | file offset (where applicable), RVA, preferred VA, runtime VA only if actually measured |
| Function | analyst label, entry and range, thunk/external/vtable classification |
| Evidence | instruction bytes/disassembly, pseudocode snippet and/or xref/call edge, citing exact local Evidence JSON file+key |
| Meaning | bounded behavioral inference, not invented symbol certainty |
| Confidence | CONFIRMED_STATIC / INFERRED_STATIC / OBSERVED_RUNTIME / UNKNOWN / CONTRADICTED |
| Rival explanation | other resource, cached descriptor, indirect thunk, different render pass, etc. |
| Next observation | precise datum that would falsify/confirm the edge |

Definitions:
- **CONFIRMED_STATIC**: reproducibly true machine-code fact or validated static call/reference, with source coordinate. Does **not** mean the branch runs every frame.
- **INFERRED_STATIC**: semantic inference from decompilation/call graph. Show caveat/alternative.
- **OBSERVED_RUNTIME**: directly measured trace or GPU artifact, named session/date/file/build; say whether CPU, GPU, or UI observation.
- **UNKNOWN**: evidence insufficient or tool cannot observe it.
- **CONTRADICTED**: exact proof that a previously proposed relation cannot hold under the verified scope.

Do not claim "proven reader", "HUDless", "safe retire", or "correct Present" from pseudocode, pointer matches, or log adjacency. Do not turn Ghidra's failure to recover an indirect edge into proof it does not exist. Do not extrapolate from a different version of RE4, an older PR #66 DLL hash, or a different OptiScaler backend.

## 7. Investigation mechanics, costs, and safety

- Begin each area with an explicit hypothesis and smallest set of candidate functions; capture a short result, then choose the next call. Avoid whole-image decompile exports or indiscriminate P-code scans.
- Keep a query ledger: tool/method, exact parameters, target hash, start/end status, Evidence location, findings, negative findings, next action. Record unsuccessful attempts as well.
- The first Ghidra import is expensive: permit one import and bounded function traversal at a time. Do not launch another uncontrolled import on client timeout.
- If a native analyzer reports truncated function dossier/P-code, note truncation; inspect narrower instructions or subfunctions instead of treating omitted edges as absent.
- Preserve vendor cache/evidence but store huge outputs locally; report only limited pseudocode/assembly excerpts that justify a specific inference.
- Local static investigation does not require the game to run. **Do not launch/attach RE4** under this order. No x64dbg/Frida/WinDbg attach, REFramework instrumentation patch, GPU capture, or debugger data-watchpoint in this phase. Those are a proposed follow-up if and only if the current research can nominate a narrow, reproducible point and the user approves a new runtime test.
- No broad D3D12 global hooks; no antitamper/DRM bypass or live injection changes; no changes to disabled diagnostic writer hooks, game exe or OptiScaler.
- Do not add REA/npm/Ghidra or its setup environment to REFramework production build, CI, or shipping runtime.

## 8. Required deliverables from local Codex

Place reviewed, standalone textual reports under **feature/re4-xess/doc** only after checking branch state and protecting concurrent changes. The initial investigation can stage all generated material locally before a reviewed documentation-only commit. Suggested files:

1. **doc/RE4_XESS_REA_RE_REPORT_2026-10-14.md** — executive result; versions/hashes; exact questions; what was proven; unknowns; decision on next experiment.
2. **doc/RE4_XESS_REA_FUNCTION_AND_RESOURCE_GRAPH_2026-10-14.md** — small call-graph and resource-edge ledger, RVA/VA instruction anchors, descriptor/SRV path, final draw candidate, alternative hypotheses and confidence labels.
3. **doc/RE4_XESS_REA_PIX_CORRELATION_AND_NEXT_CAPTURE_2026-10-14.md** — distinguish actual PIX artifacts from historical notes; object IDs only from verified capture; capture recipe for exact reader/queue/Present/fence; negative/unknown findings.
4. **doc/RE4_XESS_REA_SETUP_AND_REPRODUCIBILITY_2026-10-14.md** — Node/REA/Ghidra/JDK/Codex versions, MCP/skill readiness and first query, configuration *shape* (no secrets), commands, hash checks, environment limits, setup errors/mitigations, Evidence artifact paths relative to local staging.
5. Small ancillary .tsv/.json **only when genuinely useful** and safe to publish. No giant decompilation dumps, executable bytes, private tokens, Steam paths revealing secrets, or copyrighted raw captures.

If there is insufficient evidence for a fully resolved function, still deliver a complete negative finding: attempted queries, exact candidate set, observed obstacles, and one narrowly scoped follow-up. Mark any files not produced as NOT PRODUCED and why; do not fabricate results.

### Report header example

~~~text
Target: RE4 1.5.9.0 / SHA-256 [...]
REA: rea-agents 6.4.0 / official Codex MCP [working|not working]
Ghidra: 12.1.x / JDK: ...
Exact binary mapping: image base ... / RVA -> VA convention ...
Source branch: feature/re4-xess / baseline commit ...
PR #66 reference: head commit ... (read-only)
Observed result:
  Engine final compositor function: [RVA, evidence or UNKNOWN]
  Candidate XeSS SRV binding: [evidence or UNKNOWN]
  Actual GPU reader of installed output: [not proven unless capture]
  Reader submission -> Present -> marker: [not proven unless capture]
  Safe output reuse: [not authorized by static analysis]
Next minimal experiment: [one read-only capture/probe at proven site]
Production code touched: NO
~~~

## 9. Acceptance criteria and stop condition

**Setup gate:**
- Exact REA provider and Windows prerequisites installed or a fully documented blocker.
- Official Codex REA MCP + bundled skill configured and verified with an actual query, or official REA CLI fallback functioning and MCP blocker documented.
- Exact RE4 copy hash and PE image-base validation succeed before historical RVA analysis.

**Static RE gate:**
- At least the known RTV/TargetState writer anchors independently inspected against the exact binary, with address conversion and xrefs.
- One concrete, bounded final-compositor/draw recording candidate either established with instruction/call-site evidence **or** explicitly rejected with supporting observations.
- Descriptor/SRV/resource chain listed edge-by-edge with no "proof" inferred from missing data.
- Actual PIX notes, if used, tied to an independently inspected artifact; missing artifacts clearly marked.
- A prioritized next runtime/PIX observation and precise expected discriminating results is specified. No unsubstantiated output ring or fence policy recommendations.

**Engineering safety gate:**
- No production RE4 code, upstream REF behavior, XeFG hooks, OptiScaler fork, or game binary modified.
- No game runtime attach and no DRM/antitamper workaround.
- Raw evidence is reproducible locally, protected/unchanged, and source hashes/versions recorded.
- Document commit, if made, changes only reviewed investigation docs (and justified non-runtime helpers separately after approval). PR #66 is **not** merged/rewritten as part of this task.

Once the output reader remains unknown despite validated targeted REA investigation, **stop** and report the missing proof edge. Do not invent increasingly broad hooks or speculative architecture to force closure.

## 10. Handoff — next production decision, NOT authorized here

Only after the above report can a separate work order choose between:
- a narrowly filtered PIX / RenderDoc GPU frame with verified resource descriptors and submission identities;
- a read-only RE4-owned descriptor-binding-site instrumented capture in a separate diagnostic branch, after function bytes/callsite are independently validated;
- an independent visual HUD/ESC render-layer capture, if GPU consumer is already proven.

The subsequent design must still demonstrate actual GPU read -> owning queue -> eligible Present -> post-consumer signal and observed completion **before** modifying OutputHandoff lifetime/reuse/retirement behavior. An REA static call graph is a way to design that proof, not a substitute for it.

**Start now with the exact binary integrity, REA 6.4.0/Ghidra 12.1.x installation, official Codex MCP configuration, and Pass R0/R1. Do not ask to redesign the whole bridge before returning evidence.**
