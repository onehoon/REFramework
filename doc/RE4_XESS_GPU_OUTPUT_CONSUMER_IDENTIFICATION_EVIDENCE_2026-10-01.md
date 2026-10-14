# RE4 XeSS GPU Output Consumer Identification — Evidence Report

**Date:** 2026-10-01
**PR:** [onehoon/REFramework#66](https://github.com/onehoon/REFramework/pull/66) — keep Draft/Open
**Result:** actual GPU reader and reader-to-Present ownership remain **NOT PROVEN**. This was a read-only evidence review; no production behavior or lifetime policy was changed.

## Artifact identity and method

| Artifact | Observed identity |
| --- | --- |
| RE4 executable | `1.5.9.0`; SHA-256 `A1082B154105FAC7CA22668FFC1C99A8BFC9F1B945439276C556EDEA7597A5B7` |
| REF runtime log | `E:\SteamLibrary\steamapps\common\RESIDENT EVIL 4  BIOHAZARD RE4\re2_framework_log.txt`; SHA-256 `0A1A2FA9D3D598F64FEF4D8EDB72FCFDF8C1B5D6E1EF10901ED6DC8CC7D3EEFF`; log stamp `a5e36fca710042fb366a4833e14c3a9c0c15eb87`, branch `feature/re4-xess-load-state-accessor-diagnostic` |
| REF DLL on disk when inspected | `dinput8.dll`, file version `0.1.0.66`, SHA-256 `9D6CCCA4CD53A98FC97DAF6BDB34A4B7265C30F1B266A0687AF9F5D732457A2A`; last-write time 2026-10-01 20:32. The log does not include the loaded module's SHA, so the on-disk hash is recorded separately and is not claimed as a cryptographic runtime match. |
| OptiScaler | Runtime log reports `v0.9.5-pre4` / `a556a639` / `20260929_135800`; installed `dxgi.dll` SHA-256 `66759C05F936347006321D9ECA7D6F982012FF8FB1A9CFC4BB46A0784BB822A5` |
| Runtime logs | REF SHA-256 `0A1A2FA9D3D598F64FEF4D8EDB72FCFDF8C1B5D6E1EF10901ED6DC8CC7D3EEFF`; OptiScaler SHA-256 `09DF1DA8503AB7EF690EC1D605FEF31FA1C5BB9193F42E3A6ED4355D11350E9C` |

The installed `re4.exe` exactly matches the executable identity recorded by historical Capture 30b. That capture is useful as a candidate locator, but it is **not** a GPU capture of the 2026-10-01 run and did not record the candidate draw's bound SRV/descriptor resource.

No `.wpix`, `.pix3`, or `.rdc` capture was present under the game directory or `C:\GoogleDrive\ETS2ATS\RE4`. PIX and RenderDoc executables were not found in the available application/command locations. RE4 was not launched or attached to for this review.

The bounded offline check used the exact-hash executable and searched executable sections for validated generic x64 `call [reg+0x60]` instructions. It returned **1,704 candidate sites**. Capture 30b did not record a CPU return address for its draw, and no validated descriptor/binding path narrows those sites to the observed event. This broad static result cannot identify an owner RVA or justify an address-based hook; no RVA was promoted and no hook was added.

## Same-run software trace: install, candidate marker, next-frame skip

The REF log records this sequence around 2026-10-01 20:38:08:

| Edge | Trace evidence | Status |
| --- | --- | --- |
| Successful XeSS submit and OutputInstall | `frame=8192`, `trace=7779`, `install=1165`, `outputUseToken=1165`, `consumerEvidence=reader-not-observed`, `apiOk=true`, `queueSubmitted=true`; output `0x218f70e4940`; queue `0x21856e31be0` (type `0`, valid); submit/writer fence value `463` | **PROVEN** that REF submitted and installed this output; no reader is implied |
| Capture 30b final draw candidate | `DrawInstanced(3,1,0,0)` occurred in the Color-readable / swapchain-RT window on the last engine DIRECT submission before Present in 124/124 historical stable samples | **PROVEN candidate command pattern**, not proven to read this output |
| Bound SRV/descriptor -> installed output | Neither the runtime trace nor Capture 30b records descriptor contents/resource identity for that draw | **UNKNOWN** |
| Actual reader command list / queue submission | The candidate list ordinal is known historically; no command-list submission is tied to an actual read of `0x218f70e4940` | **UNKNOWN** |
| Present and marker | Post-Present callback reports `present=8191`; the subsequent marker carries `install=1165`, fence `463`, and `mappingState=inferred-candidate` / `marker-queued-different-present-ordinal` | Marker was **PROVEN queued**; ownership by the actual reader/Present is **CANDIDATE only** |
| Fence completion | A later event reports `actualCompleted=463`, `actualValid=true` | Completion of the signaled fence value is **PROVEN**; that it postdates the output's last GPU read is **NOT PROVEN** |
| Adjacent skipped frame | `frame=8193` reports `output-handoff-marker-pending` and no new install | **PROVEN** skip; it does not identify which image/UI target was ultimately presented |

The log therefore confirms the temporal stall and candidate software correlation, not GPU consumption. `output_use_token` remains at `reader-not-observed` on installation and is not propagated by a verified reader. A successful queue `Signal` and later completed value cannot establish that the unknown reader submitted before that signal.

## HUD / ESC composition conclusion

**Unknown.** These logs contain no GPU descriptor/resource binding or pixel-layer capture that shows whether the HUD, REFramework menu, or ESC menu is composed from the installed XeSS output, another engine target, or a later overlay. The reduced SceneView plus marker-pending skip remains a plausible frame-coherency contributor, not proven visual causality.

## Decision and minimal capture needed

Do **not** implement an output ring, alter marker timing/fence policy, promote the candidate draw, or change history/quarantine behavior from this evidence. The single next step is a narrowly filtered GPU frame capture that resolves the actual output resource through descriptor/SRV binding into its reader.

Operator capture recipe:

1. Keep XeFG **OFF**. Record the exact RE4 executable and both installed DLL SHA-256 values for the test session; enable the existing REF debug trace and keep the paired REF/OptiScaler logs.
2. In stable gameplay with XeSS active, capture one GPU frame containing a successful `OutputInstall`; record its `frame`, `trace`, `install`, and `outputUseToken` from the paired REF log. In the PIX/RenderDoc event list, inspect the Capture-30b `DrawInstanced(3,1,0,0)` candidate, but accept it only if the bound SRV/descriptor resolves to that exact installed output resource. If another draw/dispatch/copy reads it, record that actual command instead.
3. Export the reader event's descriptor/view and resource details, owning command list, queue submission/order and queue identity/type. Capture the adjacent marker-pending skip frame separately if the tool captures one GPU frame at a time; preserve both captures and the matching logs from the same run.
4. For each frame, record the exact Present/Present1 event, marker `Signal` queue/value, and a later observed `GetCompletedValue`; distinguish a queued signal from completion. If available, show HUD and ESC menu in the capture and identify their actual composition targets. If descriptor identity or the UI target is unavailable, leave that edge **UNKNOWN**.

Until that artifact exists, the evidence-backed next design choice is **no production change**. Keep PR #66 Draft/Open and request the minimal GPU capture above; do not infer a ring requirement from marker counts alone.
