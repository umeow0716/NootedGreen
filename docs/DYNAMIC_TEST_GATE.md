# Tahoe VF 動態測試前靜態閘門

Updated: 2026-10-05. 這是 fail-closed 清單，不是開機授權。任何標為
`OPEN` 或 `REVIEWING` 的必要項都禁止啟動 `macos-tahoe-sriov`、部署候選
kext/AuxKC、重綁 PCI 或寫入 SR-IOV sysfs。`CLOSED` 只代表指定的離線證據已
閉合，不代表硬體執行成功。

| ID | 必要閘門 | 狀態 | 目前證據／還缺什麼 |
| --- | --- | --- | --- |
| SG-01 | Tahoe 私有 ABI、兩份 accelerator payload 與 System/Boot KC 身分固定 | CLOSED | UUID/SHA、完整 body、vtable、caller 與 mutation contracts 由 static suite 強制。未知映像 fail closed。 |
| SG-02 | VF/PF 身分、Gen11 virtual-MMIO 與 memory-IRQ 能力分流 | CLOSED | RPL/ADL/TGL 不再錯送 memory-IRQ KLV；MTL/ARL 才使用 memory IRQ。 |
| SG-03 | 已知 legacy/PF-owned GPU producer 隔離 | CLOSED | V284 在 legacy H2G MMIO、GuC DMA、doorbell、native CTB 與 Scheduler5 execlist 五個入口先 fail-stop；modern path 另列 SG-05。 |
| SG-04 | task／PPGTT／PagePool 共同 ownership transaction | CLOSED | V283 已涵蓋 task publish/free、commit/update/release、32/64-bit unmap/shrink、descriptor retirement 與 PagePool reuse/prune/free。這不取代 GPU completion 證明。 |
| SG-05 | modern 外部 producer 在 stop 前可封門、排空，且不阻斷 `finishAllStamps` retirement | CLOSED-STATIC | V296 已關閉 P9：18 個 outer/lifetime roots 共用 receiver-scoped counted gate；native start/stop lock order、DisplaySleep、display notifier、GART、IOSurface finalize/cache selector 3/4 與 KD iterator lifetime 均由雙 KC／雙 payload contracts 固定。Stop 依序 close→drain→同步 one-shot cache finalize→發布 stopping→native stop；低層 retirement bridge 仍保持原生。 |
| SG-06 | reservation → CPU ring writes → tail publication → GuC submit 為一致的 owner/admission transaction | CLOSED-STATIC | V297 已固定 22 個 reservation edges、40 個 transaction owners、79 個 direct writer edges、六組 concrete ring vtable 與全部外層 mutex roots。Outer counted lease 包住整次 native invocation，native accelerator mutex 跨越 reservation/write/submit；final bridge 在同一 H2G queue lock 內重驗 context/ring/backing 並原子發布 LRCA tail 與 CTB tail。這不代表 GPU completion。 |
| SG-07 | GPU completion 與 ring/context/mapping/page-table backing 的最終釋放順序 | CLOSED-STATIC | V299 將 native marker、Scheduler4 invocation 與成功 CTB publication 原子綁定；idle 同時要求 GPU stamp 與 exact HWS head，並排除 termination/reset/fault。Forced GC、slot reuse、active invocation、deregister ACK 與 backing release 順序已有雙 payload/source/mutation contracts。此狀態不是硬體執行證明。 |
| SG-08 | render／depth／CCS／ICB／paging 的 allocation、event collection、partial submit 與錯誤傳遞 | CLOSED-STATIC | V300 固定八份 event-vector grow 的完整 147-call／38-owner 圖，52 個初始要求與 95 個 append growth 都由 postcondition fail-stop 保護；另以三個共同 boolean 邊界涵蓋 `submitBlit`、resource CCS 與 depth 的完整 35-call 清冊。非空 work 的 false、owner/transport 不一致與 later-plane/chunk rejection 都在 CPU 成功狀態可繼續發布前封門並 guest-panic；V281 cache/PTE commit-or-restore 仍為其前提。這是離線 fail-stop 證據，不是 GPU 執行或優雅復原證明。 |
| SG-09 | timer／IRQ／workloop callback 的取消、排空與 owner lifetime | CLOSED-STATIC | V301 固定原生 DPSM、event-machine fallback 與 Scheduler4 passive timer 的 owner/binding/free 邊界；成功 start 在 publication 前驗證三者同屬 accelerator workloop，normal stop 在 base stop 清除 workloop 前同步 detach fallback/periodic source。Start 的 base rollback 與後續 null-provider Intel stop 也各自處理 partial binding。鎖內前後快照、workloop gate 同步 removal 與 IOTimer generation increment 排除 late raw-owner action；原生 owner 稍後負責 release，VF route 不偷取 reference。PagePool callback path 由既有 zero-option admission 排除。這仍是離線證據。 |
| SG-10 | 所有 VF 可達 PF-owned MMIO／DMA／force-wake／reset 的 negative reachability | CLOSED-STATIC | V302 完成 `accelerator+0x1240` 的 131-owner／279-site 精確機械分割，並封閉 legacy construction、SafeRead/Write、Scheduler4 PM、cache/PAT/MOCS、dynamic-offset IRQ helper 與 modern GuC retained path。完整 static `/tmp/ngreen-static.CYth1a` 與 source checkpoint `5529127a03d5b92fcd8ae5ee538b9bad4e136be0` exact-SHA CI `37232739353` 通過。這仍不是硬體執行證據。 |
| SG-11 | baseline 要求的所有程式檔完整審閱與 ledger closure | LOCAL-PASS / CI-PENDING | Exact ledger仍涵蓋1,500 paths及全部program/dependency/payload；V305補上所有可寫qcow2的read-only integrity gate，完整local static `/tmp/ngreen-static.5M8IL1`已通過，仍待clean pushed exact-SHA CI。 |
| SG-12 | 精確候選 commit 的完整 static suite、x86_64 release kext、Metal smoke build 與 artifact provenance | REVALIDATE V305 | V304 checkpoint `8357b2d91729ba55a30eefc539b9544ffe4be1bf` 的exact-SHA CI `37237068929`成功，artifact已核對；V305更改host containment tool，必須在新clean pushed SHA重跑。 |

## 目前主路徑

V305在V304完整CI及root preflight通過後，對上次host crash留下的系統qcow2執行
read-only check，發現一個data-cluster refcount遺失（工具報告兩個相關corruption）。
byte-identical sparse backup保存後，原檔refcount structure已重建且複查零錯誤；preflight
現在必須逐一拒絕任何未通過`qemu-img check`的可寫qcow2。V304根據實際define/re-dump結果修正V303：Q35會把省略的整合iTCO自動補回
`action=reset`，所以第一輪contract要求唯一iTCO `action=none`，並仍以獨立host
deadline作唯一timeout authority。schema validation及完整local static
`/tmp/ngreen-static.kSsbt5`、exact-SHA CI `37237068929`與root current-boot preflight均
已通過；V305完整local static `/tmp/ngreen-static.5M8IL1`亦通過，新checkpoint CI仍待完成。V299 的 completion predicate、V300 的 submission-result boundary、V301 的
同步 callback detach 與 V302 的 negative reachability 都不能將靜態證據誤稱為 GPU
已實際執行；封板文件HEAD的SG-12即時重驗完成前仍禁止動態。以下保留SG-05/SG-06
producer 路徑證據作為 transaction 前提。
Tahoe 25G229 的 `IOAccelCommandQueue::submit_command_buffers`
從 `queue+0x5c0` 取得 accelerator，整批持有 accelerator busy lock，並只在
`canSubmitCommandBuffer == false` 的 pause window 暫時放鎖再重取。其預設與 queue
override `canSubmitCommandBuffer` 都固定回傳 true，所以它本身不是 stop gate。外層
static selector 是目前唯一已確認的直接 member-submit caller；queue stop 也在同一 busy
lock domain 內呼叫 `stopLocked`。

舊式 `IOAccelContext2::submit_data_buffers` 是另一個並列入口，不經上述 command
queue。System KC selector 2 直接指向它；完整 body 在 context `+0x5a8` accelerator 的
busy-lock domain 內呼叫 concrete `processDataBuffers`。它的 can-submit/pause loop 同樣會
暫時放鎖再睡眠，context stop 則在同一 domain 內呼叫 `contextStop`。因此 admission
必須從兩個外層 entry 一起計數，不能只 hook 新式 command queue。

`IOAccel2DContext2` 另外公開 selector `0x100` set-surface、`0x101` finish 與
`0x102` blit；其中 blit 是不經前兩條路徑的 GPU producer。它在 context
`+0x5a8` accelerator 的 mutex/busy domain 內，以 vtable `+0xb40/+0xb48`
分派 Intel `blitCopy`／`blitFill`，且等待分支會先放鎖、之後重新進入。
Intel payload 的 GL、CL、main/media/VEBox legacy processors、command queue
processor 與 2D overrides/factories 已完整固定，所以已知 driver override 不再是
未辨識旁路；base-family clients 的 P5 inventory 也已於下述三組閉合。

Base `IOAccelSurface` 的 19 個 selector、`IOAccelDevice2` 的 10 個 selector 與
`IOAccelSharedUserClient2` 的 21 個 selector 已固定。Surface 特殊 selector 會進入
set-id、legacy flush、set-shape 與 shared-event dispatch；`surface_read`、shape/
displayable、legacy swap/copy/update 的 mutex/busy scopes 與 vtable producer edges
也已固定。Intel factory 確認建立 `IGAccelSurface`，其 copy DMA、swap flush 與
copy-forward 進入 `submitBlit`，swap-copy 則經 accelerator `+0x9a8`。Shared selector
11 的 dirty ring 只處理 CPU resource-state virtuals，未找到新的 GPU submit edge。
這關閉 Surface dispatch inventory，但不是 admission、completion 或 drain 證明。

GLContext `0x100..0x105`、GLDrawable 六項與 SurfaceMTL 十九項 selector 表也已
完整固定。GLContext selector `0x105` read-buffer 是獨立 producer root：它在
accelerator mutex/busy domain 及 enabled wait 後，經 surface `+0x960` 進入既有
Intel copy/DMA/`submitBlit` 鏈，另有 CPU fallback。GL `processSwap` 由既有 P2
data-buffer processor 的 `+0xb70` virtual 進入，不另算外部 root；其 surface-copy、
flip 與 legacy present 選擇已固定。GLDrawable 與 SurfaceMTL 只找到 shape、IOSurface
與 shared-event fence 設定；完整 body、特殊 dispatch、外層 lock scope 及 fence edges
沒有顯示新的 GPU-submit root。這關閉 P5b inventory，仍不等於 admission 或 completion
證明。

Device 十項 selector 的完整 body 與三個 busy-lock 讀取範圍已固定，沒有新的 GPU
producer。Shared 二十一項 member body、selector 0/17 的可變結構特殊 dispatch 及其
busy-lock 範圍也已固定；selector 2 `page_off_resource` 與 selector 18
`get_resource_offset` 都可進入 paging producer，後者經 `getPhysicalOffset -> prepare`；
new-resource 的錯誤清理由 `unload` 也可能 page-off。selector 2 會經
`IOAccelResource2::pageoffIfNeeded` 的 concrete virtual 進入
`IGAccelResource::pageoff`。Intel payload 另固定 resource initialize/alloc、page-on、
page-off 的完整區域與 vtable：page-on 有三次、page-off 有兩次 `submitBlit`，五個 AL
結果皆未被消費。MemoryInfo 的三項 selector、argument contract、2 秒 bounded lock、
gather 與 purge/unwire 路徑也已固定；V293 證明 purge 的 unwire 可再到
`unload_dirty_resources -> unload -> pageoffIfNeeded`。這關閉 selector/body inventory，
但新增的 paging root 必須由 P8/P9 與 SG-08 一起解決。

DisplayPipeUserClient 的十四項 selector、完整 argument table、wrapper/member bodies、
accelerator mutex/busy scopes 及 pipe selection 已固定。selector 8
`transactionEnd` 會進入既有 transaction queue／flip path；selector 12
`copySurface` 會進入 pipe copy 並經 accelerator `+0x9a8` 到
`IntelAccelerator::submitSwapCopy`，兩者都是 P8/P9 必須涵蓋的 external roots。
Intel `newDisplayMachine`／`newDisplayPipe` factories、concrete vtable，以及
base→legacy display-machine start、`IOFramebuffer` enumeration、
`found_framebuffer`、`createDisplayPipe` 鏈亦已固定。拒絕 Intel physical
framebuffer 並不足以證明無 pipe：其他 registry `IOFramebuffer` 仍可能被掃描。
因此 P6 是 `CLOSED-INVENTORY`，不是 unreachable 或安全執行證明。

非 user-triggered/internal producer 的直接呼叫清單也已在兩份 Intel payload
逐一固定。V292 已把 PAVP command callback 在 VF 改為不碰硬體且回傳
`kIOReturnUnsupported`，也把會 tail-call telemetry sample 的 `recognizeFlip` 改為 VF
no-op；兩者不再進入 counted admission。V292 當時只剩 DisplaySleep callback；V293
證明這個 direct-call 結論漏掉 System KC 的 transitive resource paging，包括 display
mode/power/WSAA、gart/cache control、linear page-off 與 KD first-flush roots。原生
`startGraphicsEngine` 內的 PAVP 與 stamp
提交在 classified VF
上因整個入口被替換而不可達；reset replay 與舊 DPSM kick 則留在已隔離的
IGGuC/Scheduler5 路徑後方。其餘 telemetry、sync-event、context stamp 與 Blit2D
initialize 呼叫都是已列 external roots 的後裔。

GC/GuC/scheduler/DPSM timer 與 `finishAllStamps` 另以完整 body 及 negative direct-edge
契約證明沒有直接進入已知 Intel ring producer；但 gart collector 可經 inherited
`try_unload_dirty_resources -> unload -> pageoffIfNeeded` 間接到 Intel page-off。
event/channel finish、transaction idle、mapping/unwire/freeAllGPUMappings 也使用同一
lower bridge，屬必須保留的 retirement／teardown 路徑。這表示 low-level hook 不安全，
也不是 owner-lifetime 或 no-late-callback 證明；P8 不得把 retirement 一併封死，P9 與
SG-09 仍須證明 cancel/drain/lifetime。V294 已將全部 104 個 slot call sites
分割成 57/5/11/31 四類，並固定 display notification、GART collector、
IOSurface device-cache control 與 KD first-flush 四個非 user-client control roots
的註冊點和 receiver ownership。P7 因此關閉為 inventory；callback
drain/lifetime 仍由 SG-09 處理。

Display notification 另會透過 concrete `IGAccelDisplayMachine` 在 mode-change
前後直接呼叫 routed engine stop/start。這不會重建已存在的 GuC/CTB：Tahoe
`IGScheduler::initFirmware` 會先檢查 scheduler `+0x20` loaded byte，只有首次
成功前才 dispatch `+0x220 loadFirmware`，成功後的 display resume 是明確的
idempotent fast path。兩份 payload 的 callback bodies、vtable slots 及 stop/start edges
已固定；這只解除重複 firmware-init 疑慮，不代替 P8/P9。

V295 已實作「VF accelerator receiver identity + 外層 counted admission」：16 個
System-KC roots 與 Intel `DisplaySleepCallback` 共用同一個 close/count contract。
GLContext 只在 `0x100..0x105` 取得 outer lease；inherited selector 2 由
`submit_data_buffers` 自己取得，避免同一交易雙重計數。PF／非目標 receiver 原樣呼叫
native trampoline；resource prepare/load/unload/page-on/page-off、`submitBlit` 與其他
retirement bridge 不被 route。protocol fault 僅原子封門，避免在自己的 lease／IRQ
callback 內等待；final accelerator stop 才 bounded drain，之後發布
`gVfDeviceStopping` 並進入 native `finishAllStamps`。這關閉 P8 的實作與 receiver
mapping，但 callback owner lifetime、stop caller 鎖狀態及完整 cancel/drain 仍屬 P9／
SG-09；不能因此進入動態。

V296 關閉 P9。兩份 Intel payload 的 start/stop 鎖序與 Tahoe System/Boot KC 的
workloop/notifier API 已交叉固定；DisplaySleep、display notifier 與 GART source 都先
同步撤銷再清除 receiver。IOSurface 的 finalize source、跨 KC import、全 cache 掃描及
selector 3/4 已完整固定，並修正原本 gate 會拒絕退役 callback 的缺口：finalize handler
本身取得 counted lease，selector 3/4 只能在 retirement scope 中略過巢狀 lease，stop
則在一般 producer drain 後同步執行原生 atomic one-shot finalize。KD callback 雖永久
註冊，但只使用每次新建且持有 backing OSSet 的 registry iterator，沒有保存 accelerator
receiver。這些證據只關閉 SG-05；SG-06 至 SG-11 仍阻擋動態測試。

V297 關閉 SG-06。兩份 Intel payload 的 22 個 direct `waitForSpace` edges、全部
79 個 direct `writeDWord`/`writeBuffer`/`alignRing`/`submitCommands`/
`submitStamp` edges 與 40 個不重複 transaction owners 已精確分割；`writeQWord`
沒有 direct caller，`submitToRing` 只由六組已固定 concrete ring vtable 到達。這 40 個
owners 全部沒有 `_IOLockUnlock`/`unlock_busy` direct edge，而 System KC 的
GART、finalize、device-cache、first-flush、display callback 與 Intel DisplaySleep 的完整
accelerator-lock 進出口亦已固定。因此 SG-05 的 counted outer lease 保持整次 native
invocation 存活，Tahoe accelerator mutex 使 reservation/write/final dispatch 串行。
`vfSubmitWorkItem` 再以 context-operation gate 與 H2G queue lock 重驗 descriptor/task/context/
ring backing/geometry/tail；`vfSendCtbFastAction` 只在 CTB space 與 credits 都預留成功後，
於同一 queue lock 內先發布 LRCA tail，再發布 H2G descriptor tail 及 interrupt。失敗
enqueue 不寫 tail；stop 先 drain outer producers，後續 engine stop 才 close context gate。
Freestanding `NGVfSubmissionCoverage::Tracker` 沒有被誤當成生產環境 lease；它與 GPU
completion/stamp 覆蓋仍屬 SG-07。Targeted dual-payload、paired-KC contracts 與 full static
`/tmp/ngreen-static.H0Uwp3` 均通過；V297 clean checkpoint
`6f8a325d9f45bc183be8845c3f8966ee0bacfbf4` 的 exact-SHA CI `37221608262` 亦完整通過，
SG-07 至 SG-11 仍禁止動態。

## 靜態轉動態的交接條件

只有 SG-01 至 SG-11 全部為 `CLOSED`，且 SG-12 在同一個 clean/pushed commit 上完成
即時重驗，才可建立一次性候選
artifact。之後仍必須另外通過 `HOST_CONTAINMENT_PLAN.md` 的 current-boot root journal、
destroy-only lifecycle XML、唯一且停用動作的iTCO watchdog、獨立 host watcher、固定 deadline 與 cooldown 檢查；這些是
動態 preflight，不可由本表的離線結果代替。第一輪只允許最短、單次、可強制摧毀的
contained boot，不是效能、Metal completion、媒體或 Looking Glass 測試。

## SG-05 子閘門

| 子項 | 狀態 | 證據／剩餘工作 |
| --- | --- | --- |
| P1 command queue selector 1 | CLOSED-INVENTORY | 外層 static/member/per-buffer、pause 放鎖/重取、stop busy domain 與 Intel process override 已固定。 |
| P2 legacy context selector 2 | CLOSED-INVENTORY | 外層 submit 與 GL/CL/main/media/VEBox concrete `processDataBuffers` 已固定。 |
| P3 2D selectors `0x100..0x102` | CLOSED-INVENTORY | 完整 method table/body、busy-lock scopes 與 Intel blitCopy/blitFill slots 已固定。 |
| P4 Intel SharedUserClient selectors `20..28` | CLOSED-INVENTORY | 完整九項表已固定；20/22/27 分別是 depth/color/ICB producers。 |
| P5a Base/Legacy/Intel Surface | CLOSED-INVENTORY | 19-selector 表、特殊 dispatch、完整 producer bodies/lock scopes、legacy 與 Intel vtable/factory、copy/swap/flush/blit edges 已固定；不代表已實作 admission。 |
| P5b GLContext／GLDrawable／SurfaceMTL | CLOSED-INVENTORY | 三組完整 selector/argument tables、dynamic/static/special dispatch、完整 member bodies、vtable、mutex/busy/wait scopes 與 producer/fence edges 已固定。GL selector `0x105` read-buffer 是獨立 copy/DMA root；processSwap 屬 P2 家族；另兩類未找到新 submit root。 |
| P5c Device／Shared／MemoryInfo clients | CLOSED-INVENTORY | Device 10、Shared 21 與 MemoryInfo 3 項 selector/argument contracts、完整 member/wrapper bodies、特殊 dispatch、busy/timeout-lock scopes 與 unwire edges 已固定。Shared selector 2 經 `pageoffIfNeeded` 進入 Intel page-off；page-on/page-off 共五次 `submitBlit` 的未消費 AL 已列入 SG-08。 |
| P6 display／flip reachability | CLOSED-INVENTORY | 14-selector DisplayPipeUserClient、鎖域、pipe selection、transaction/copy producer、Intel factories/vtables 與 base→legacy framebuffer enumeration/create-pipe 鏈已固定。無法證明不可達，故 selector 8/12 與 downstream flip/copy 必須納入 P8/P9。 |
| P7 非 user-triggered／內部 producers | CLOSED-INVENTORY | V294 將五組 slot 的 104 個 executable call sites 精確分成 admitted/control 57、retirement/teardown 5、shared bridge 11、unrelated receiver 31；display/GART/device-cache/KD 四個 control roots 的註冊與 receiver 亦已固定。Display mode stop/start 由 native scheduler loaded-byte 保證 firmware init 冪等。 |
| P8 counted admission 實作 | CLOSED-STATIC | 18 個 outer/lifetime roots 已 route；12 組 object→accelerator offsets、五個 direct accelerator callbacks 與 DisplaySleep ABI/route 均由 pinned binaries/source contracts 固定。PF／非目標 receiver pass-through，GL inherited selector 2 不重複 lease，低層 retirement bridge 保留。V301 clean baseline 為 148 routes；V302 working tree 加入 17 個 SG-10 isolation routes 後為 165（141 accelerator、3 framebuffer、21 System KC）。 |
| P9 close→drain→native stop 鎖序 | CLOSED-STATIC | Native start/stop 的 accelerator-lock／busy-lock 次序與失敗 stop edge 已固定；DisplaySleep 先撤銷 callback table，display notifier 的 `remove()` 與 GART/finalize source 的 workloop removal 均同步。IOSurface gather 只接納 retain-count 1 的 orphan cache，selector 3 最後 release 同步巢狀 selector 4；production 在 drain 後同步呼叫原生 one-shot finalize，再發布 `gVfDeviceStopping`。永久 KD callback 每次建立 retaining matching-services iterator，不保存 receiver，且 receiver wrapper 提供 late-entry gate。完整 static 與 paired-KC contracts 通過。 |

## SG-06 子閘門

| 子項 | 狀態 | 證據／剩餘工作 |
| --- | --- | --- |
| S6.1 reservation budget/postcondition | CLOSED-STATIC | 22 個 native reservation edges 完整分割；VF wrapper 對 caller overhead、TLB/AUX trailer、ring mask/cursor 與回傳 available capacity fail closed。 |
| S6.2 writer/dispatch inventory | CLOSED-STATIC | 40 個 owners、79 個 direct writer/helper edges、六組 concrete ring vtable 及 `submitToRing -> Scheduler4::push -> submitWorkItem` receiver chain 均已固定。 |
| S6.3 outer owner lifetime | CLOSED-STATIC | SG-05 的 18 個 counted roots 保持整次 native invocation；stop close/drain 在 native `finishAllStamps` 與 engine teardown 前完成。 |
| S6.4 native writer serialization | CLOSED-STATIC | 40 個 transaction owners 均無 direct unlock edge；Intel DisplaySleep 及 System-KC control roots 的完整 accelerator mutex 進出口已固定。 |
| S6.5 final identity/publication | CLOSED-STATIC | Context-operation gate + H2G queue lock 下重驗 descriptor/task/context/ring backing/geometry/tail；只在 CTB/credit reservation 後依序發布 LRCA tail、H2G tail 與 interrupt。 |
| S6.6 failure/stop ordering | CLOSED-STATIC | Failed enqueue 不寫 LRCA tail；false submit 走 Tahoe fatal work-queue path。Outer producer drain 早於 context gate close，不會在已接納 transaction 中途釋放 backing。GPU completion 及最終釋放仍屬 SG-07。 |

## SG-07 子閘門

| 子項 | 狀態 | 證據／剩餘工作 |
| --- | --- | --- |
| S7.1 direct context／ring／stamp／scratch backing lifetime | CLOSED-STATIC | Attach 在 firmware registration publication 前獨立 retain 四份 DMA backing；只有匹配 `DEREGISTER_DONE`、最後 native reference 與無 protocol fault 時才清除 identity 並在鎖外 release。Slot reuse 不會略過 tombstone ownership。 |
| S7.2 device-wide shutdown DMA boundary | CLOSED-STATIC | Context operation gate 先 close/drain；每個 context 完成 disable+deregister 後依序等待 heavy Engines 與 GuC TLB matching ACK，最後才發布 `gVfDmaQuiesced`。Pre-CTB rollback 另要求完整 context table unowned。 |
| S7.3 live GGTT／PPGTT／PagePool／task retirement | CLOSED-STATIC | V280–V283 已固定 GGTT post-write GuC invalidation、32-bit unmap 後與 64-bit shrink 前的 Engines invalidation、final task free 的 context 排除／retirement，以及共同 recursive task/table/PagePool transaction；shared descriptor 在 ACK 前保持額外 reference。 |
| S7.4 exact normal-submit marker association | CLOSED-STATIC | VF-only Scheduler4 `push` wrapper 在 context-operation gate 內固定 descriptor/task/context/ring/stamp-index、thread owner、sequence/tail 與 final ring topology；內層 `vfSubmitWorkItem` 必須 claim 同一 token，且只在 CTB action publication 後 publish coverage。Native 結果與 publication 不一致即 fail-stop，後續 un-stamped submit 使舊 coverage 失效。 |
| S7.5 GPU-written completion observation／true idle | CLOSED-STATIC | Retained stamp slot `+0` 以 signed wrap-safe comparison驗證，retained context/HWSP `+0x10` 必須等於 marker transaction 的 final published tail；ring 最後只能是 `MI_REPORT_HEAD` 或 `MI_REPORT_HEAD, MI_NOOP`。10-byte CPU-address getter、ring init/refresh、stamp encoders 與 marker topology均 whole-body/anchor pinned。無法證明時只會 false-busy。 |
| S7.6 termination／restart／fault／forced-GC／reuse | CLOSED-STATIC | Snapshot 前後雙重檢查 accelerator `+0xdc8`，並拒絕 submission stop、protocol fault、device stop 與 shutdown。Scheduler4 reset callback為 pinned no-op；timeout/replay/physical-reset roots fail closed。Forced collect/drain 即使略過 idle，context free 仍經 routed GuC detach，四份 backing 留到 matching deregister ACK。Table lifetime-long 不換址；monotonic non-wrapping serial、active-owner reuse/release拒絕與 operation-gate close/drain 排除 ABA/UAF。 |

## SG-08 子閘門

| 子項 | 狀態 | 證據／剩餘工作 |
| --- | --- | --- |
| S8.1 command／scratch／rectangle allocation | CLOSED-STATIC | Command-pool getter與建構/partial unwind、Blit3D scratch 及 CCS rectangle-null bounded patches 均有 source、binary-anchor、sanitizer／emulation contracts；無法取得完整 backing 時不會把不完整 command 當成功發布。 |
| S8.2 dependency event-vector completeness | CLOSED-STATIC | 八份 duplicate grow targets 全部 exact-address route；雙 payload 完整清冊為 147 calls／38 owners，其中 52 個初始 request 與 95 個 append growth 均受 VF state/postcondition 保護，無 direct tail/address-taken 旁路。Native typed event-pair 若無法建立會在 resource 初始化、任何 GPU publication 之前停止，不能形成靜默遺漏。 |
| S8.3 blit／ICB／paging result propagation | CLOSED-STATIC | `IntelAccelerator::submitBlit` 的完整 26-call 清冊進入同一 VF wrapper；空 vector 保留 native no-op，非空工作只有 transport、task 與 2D/3D FIFO 完整且 native AL=true 才可返回。ICB 兩次、page-on 三次與 page-off 兩次的 ignored AL 不再能靜默繼續。 |
| S8.4 CCS／depth multi-plane/chunk result | CLOSED-STATIC | Resource CCS/depth 的 4+5 個 direct calls 無 tail/address-taken 旁路；兩個 exact x86_64 ABI wrapper 對 owner/transport 或 native false 均先標記 protocol fault 再 panic。若先前 plane/chunk 已發布，fail-stop 保留 unresolved state並禁止後續 CPU success/free，而不是宣稱 rollback 已發生。 |
| S8.5 cache/PTE publication consistency | CLOSED-STATIC | V281 的共同 resource cache route 在 installed mapping 更新失敗時還原 resource flags 與 mapping type、重播舊 type，完成 Engines+GuC retirement 後 fail-stop；10 mutations 與 8,192-state commit-or-restore model 通過。 |
| S8.6 integrated regression | CLOSED-STATIC | 新增 8 組 submission-result mutations、精確 35-call inventory、compiled ABI symbols/source forwarding contract；V300 route inventory 為 147（124/3/20）。完整離線 suite `/tmp/ngreen-static.a0ic6Y` 與 exact-SHA CI `37227125652` 均通過。 |

## SG-09 子閘門

| 子項 | 狀態 | 證據／剩餘工作 |
| --- | --- | --- |
| S9.1 raw-owner callback inventory | CLOSED-STATIC | 雙 payload 固定 Scheduler4 `+0x448` passive timer、event-machine `+0xd30` fallback 與 accelerator `+0x1460` DPSM timer 的建構、綁定、callback、free/stop 邊界。PagePool 唯一 factory 與 VF admission 固定 options=0、`pool+0x64=0`，callback-backed pool 在目標路徑不可達。 |
| S9.2 successful-start admission | CLOSED-STATIC | 在 `registerService` 前要求 event-machine／Scheduler4／DPSM owner 完整，三個 source 綁到同一 `accelerator+0xf0` workloop；event/periodic collection 由 scheduler mutex 下快照為空。任何 incomplete binding 直接 fail-stop。 |
| S9.3 normal-stop drain | CLOSED-STATIC | Intel stop 先 finish stamps、停 engine並移除/release/清空 DPSM；新 System-KC base-stop route 在 inherited stop 清除 `+0xf0` 前，且只在 external producer gate 已 close+drain 時，同步 disable/remove fallback 與 cancel/remove periodic timer。Detach 前後均在 native mutex 下驗證 collection/count 為空；source reference 留給原生 owner free。 |
| S9.4 start-failure rollback | CLOSED-STATIC | Exact start body固定兩種非對稱路徑：base start 可在 Intel attachment 前自行呼叫 base stop；較晚失敗則以 null provider 呼叫 Intel stop而跳過 base stop。兩者都只接受 zero-use partial binding，並在 workloop 尚存時同步 detach；DPSM allocation 的特殊失敗仍由既有 V252 engine rollback 處理。 |
| S9.5 no-late-callback／lock order | CLOSED-STATIC | Boot KC 固定 `removeEventSource` 經 command gate進入 maintenance op=1，在 workloop gate內呼叫 `setWorkLoop(nullptr)`；timer disable 先增加 generation，再 cancel/cancel-wait。已進 gate 的 action 在 remove 返回前完成，queued passive callout因 generation mismatch 不再呼叫 raw owner。Production 不持有 scheduler mutex進入 workloop gate，並於同步 detach 後二次鎖內驗證。 |
| S9.6 integrated regression | CLOSED-STATIC | 15 個 callback-owner mutations、30 種 teardown ordering、4 種 active/queued generation 狀態、雙 payload lifecycle contract、Tahoe 25G229 paired-KC contract與完整 suite `/tmp/ngreen-static.uzHEFL` 均通過。Route inventory為148（124/3/21）；精確候選 CI 留在 SG-12。 |

## SG-10 子閘門（動態前必須全部關閉）

| 子項 | 狀態 | 證據／剩餘工作 |
| --- | --- | --- |
| S10.1 raw BAR0 `+0x1240` owner inventory | CLOSED-STATIC | 雙 payload 的完整 `__text` disp32 memory-operand scanner固定 131 個 owner／279 個實際 sites，並分為 telemetry/diagnostic 37/70、legacy construction 31/85、physical roots 24/74、modern GuC 21/27、object layout/publication 4/4、IRQ 14/19；另兩個 `mov $0x1240,%edx` 只是 Blit kernel copy 長度而不誤列。精確 owner→site map、owner alias、新 site、分類或數量改變皆 fail closed。 |
| S10.2 legacy IGGuC／Scheduler5／CommandStreamer5 | CLOSED-STATIC | V302 已在兩個 scheduler factories／initializers 與 CommandStreamer5 factory／initializer 建立硬拒絕；SafeRead32/64/Write32 最終 sinks 亦 fail-stop。原始 factory caller、body hash、direct／LEA／loaded-pointer inventory 與 route mapping已由雙 payload contract 固定。 |
| S10.3 physical engine start／cache／PM／fence／reset／diagnostics | CLOSED-STATIC | VF engine start不進原始 raw start；五個 cache/PAT/MOCS virtuals、Scheduler4 PM與 SafeRead/Write 已直接隔離。Fence factory、eDRAM、async-slice admission、timeout/hang、reset/replay與 PAVP 既有 contracts保持 fail closed。 |
| S10.4 modern GuC／CTB／WorkQueue retained path | CLOSED-STATIC | 21 個 raw owner已逐項對齊：15 個公開入口 VF-route；WOPCM checker只由已整體替換的 native `loadGuCBinary` 呼叫；MDRB read/write與 CS4 frequency helper固定零 reference；CS4 error helper由能力分支前一層封鎖；`releaseUkContext`及四個仍執行的 WorkQueue/CTB body以相鄰 symbol bounds套用 exact `0xCEE8` patches。body hash、direct/tail、RIP-LEA、loaded pointer、route mapping、patch bounds與 retained bootstrap edges均由雙 payload/source contracts固定。 |
| S10.5 interrupt transport allowlist | CLOSED-STATIC | 完整 14-owner／19-site `IGInterruptBridge` BAR0集合已固定。TGL/ADL/RPL只保留 12 個固定-offset bridge bodies，其每個 `0x190000` offset均在 i915 VF allowlist；caller-selected offset的 `clearQueuedHeirarchichalIntrBits`／`readIIR` 具 whole-body與零 branch/LEA/pointer證明，並在 VF入口直接拒絕。MTL/ARL capability branch則替換 filter/read/bridge/scheduler；virtual-MMIO branch只封鎖實體 per-engine error helper。 |
| S10.6 telemetry／OA／debug sysctl／PAVP | CLOSED-STATIC | 既有 end-to-end contracts固定 pre-engine manager、usage、trace、OA user-client、55 OIDs及 hardware descendants；VF routes保留必要 object lifetime但不進 force-wake/PF MMIO。PAVP callback與 telemetry flip root亦已隔離。 |
| S10.7 raw helpers with no reachable owner | CLOSED-STATIC | `clearEventWait`、`clearSemaphoreWait`、CommandStreamer4 `setGTFrequencyMMIO`、GuC `readMDRBRegister`／`writeMDRBRegister` 均以 exact whole-body及零 direct/tail、RIP-LEA、loaded-pointer reference固定；任何未來 address-taken或 caller新增即測試失敗。 |
| S10.8 SG-10 integrated regression | CLOSED-STATIC | Targeted dual-payload lifecycle、165-route contract、`git diff --check`及完整 static suite `/tmp/ngreen-static.CYth1a` 均通過；clean source checkpoint `5529127a03d5b92fcd8ae5ee538b9bad4e136be0` 已 push，exact-SHA CI `37232739353` 成功並產出 release kext與 Metal smoke artifacts。SG-11仍禁止啟動 VM或操作 VF/PF。 |

## SG-11 子閘門（所有程式檔 ledger）

| 子項 | 狀態 | 證據／剩餘工作 |
| --- | --- | --- |
| S11.1 exact tracked inventory／scope partition | CLOSED-STATIC | `source_review_inventory_test.py`固定目前1,500 paths與top-level partition：NootedGreen 42、tools 68、MacKernelSDK 1,227、Lilu.kext 40、sle_Internal 109、Xcode 4、workflow 1，另含docs/root metadata；新增、刪除、改名、symlink escape均fail closed。舊1,305-file快照只保留歷史用途。 |
| S11.2 production source semantic review | CLOSED-STATIC | 六個`.cpp`、35個`.hpp`與Info.plist均在Xcode/compiler closure；`kern_gen11.cpp/.hpp`已逐段讀完，完整165-route與protocol contracts仍固定。Compiler/analyzer/cppcheck/clang-tidy未找到新的material defect；161個Gen11方法及95個route/original fields均有consumer。 |
| S11.3 host tools／tests semantic review | CLOSED-STATIC | 68個tools paths精確分為50個static/workflow direct programs、12個明列的indirect/external programs與6個data/fixtures；28個Python AST、3個shell syntax、全部C++ model/static contracts均受測。V304 preflight明確要求唯一Q35 iTCO且`action=none`，host deadline是唯一timeout authority；不存在的`destroy` action與會被Q35補回`reset`的零watchdog合約均已排除。Pinned System/Boot KC contract及兩個Unicorn command-pool tests亦已離線通過。 |
| S11.4 vendored dependency closure | CLOSED-STATIC | 實際compiler closure固定為369 paths：41 product、15 Lilu與313 MacKernelSDK。Production的直接vendor surface另固定26個headers及68個external imports，其中16個由exact Lilu binary實際export、52個是kernel ABI；因此transitive include不再被誤列為逐項執行consumer。Lilu bundle/binary/plugin-start、`libkmod.a`及其兩個精確members/source identities均固定；Xcode contract固定唯一archive link input與plugin-start source。其餘vendored paths仍保留在1,500-path清冊，但不宣稱為active build implementation。 |
| S11.5 payload／metadata／build closure | CLOSED-STATIC | 九個bundle／109 paths的content identity與topology已固定；43個plist/CodeResources可解析，15個MacOS payload全為x86_64 Mach-O。四個kernel binaries保留per-route review，11個userspace binaries只列opaque identity。`AppleIntelGraphicsShared.bundle`沿用受審12.5 resource-only layout，其plist所列但不存在的`AppleIntelGraphicsSharedIL`是唯一精確例外，新增任何缺 executable皆fail closed。Xcode與workflow contracts另固定6 sources、35 headers、3 configurations及artifact consumers。 |
| S11.6 obsolete／duplicate／unowned code elimination | CLOSED-STATIC | 六個production translation units通過unused-function/private-field/internal-declaration硬錯誤；product無TODO/FIXME/XXX，161個Gen11 definitions與95個route/original fields皆有owner。62個tool programs及6個fixtures無未歸屬項；保留的legacy程式均由現行PF/VF隔離或轉譯contracts明確擁有，未找到可安全刪除而不改ABI的重複production path。 |
| S11.7 integrated all-file regression | LOCAL-PASS / CI-PENDING | V305完整local static suite `/tmp/ngreen-static.5M8IL1`通過，包含qcow2 integrity source contract與全部既有檢查。尚缺clean commit/push及exact-SHA CI；完成前禁止啟動VM。 |
