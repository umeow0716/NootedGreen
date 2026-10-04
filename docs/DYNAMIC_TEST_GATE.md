# Tahoe VF 動態測試前靜態閘門

Updated: 2026-10-04. 這是 fail-closed 清單，不是開機授權。任何標為
`OPEN` 或 `REVIEWING` 的必要項都禁止啟動 `macos-tahoe-sriov`、部署候選
kext/AuxKC、重綁 PCI 或寫入 SR-IOV sysfs。`CLOSED` 只代表指定的離線證據已
閉合，不代表硬體執行成功。

| ID | 必要閘門 | 狀態 | 目前證據／還缺什麼 |
| --- | --- | --- | --- |
| SG-01 | Tahoe 私有 ABI、兩份 accelerator payload 與 System/Boot KC 身分固定 | CLOSED | UUID/SHA、完整 body、vtable、caller 與 mutation contracts 由 static suite 強制。未知映像 fail closed。 |
| SG-02 | VF/PF 身分、Gen11 virtual-MMIO 與 memory-IRQ 能力分流 | CLOSED | RPL/ADL/TGL 不再錯送 memory-IRQ KLV；MTL/ARL 才使用 memory IRQ。 |
| SG-03 | 已知 legacy/PF-owned GPU producer 隔離 | CLOSED | V284 在 legacy H2G MMIO、GuC DMA、doorbell、native CTB 與 Scheduler5 execlist 五個入口先 fail-stop；modern path 另列 SG-05。 |
| SG-04 | task／PPGTT／PagePool 共同 ownership transaction | CLOSED | V283 已涵蓋 task publish/free、commit/update/release、32/64-bit unmap/shrink、descriptor retirement 與 PagePool reuse/prune/free。這不取代 GPU completion 證明。 |
| SG-05 | modern 外部 producer 在 stop 前可封門、排空，且不阻斷 `finishAllStamps` retirement | REVIEWING | V295 已關閉 P8：17 個 outer roots 使用同一 receiver-scoped counted gate，PF／非目標 receiver 與 low-level retirement bridge 保持原生；protocol fault 只封門，final stop 先做 5 秒 bounded drain 再發布 stopping。P9 仍須封閉 stop caller、callback cancel/lifetime 與鎖序證明。 |
| SG-06 | reservation → CPU ring writes → tail publication → GuC submit 為一致的 owner/admission transaction | OPEN | reservation postcondition、ring geometry、retained backing 與 final submit validation 已有；但 native writer 在 final routed submit 前的跨呼叫區間仍沒有完整 lease。 |
| SG-07 | GPU completion 與 ring/context/mapping/page-table backing 的最終釋放順序 | OPEN | GuC context deregistration與 heavy TLB ACK 已覆蓋選定 teardown；尚未證明所有完成事件、stamp、pool reuse 與 producer owner 都在釋放前退休。 |
| SG-08 | render／depth／CCS／ICB／paging 的 allocation、event collection、partial submit 與錯誤傳遞 | OPEN | 已修補選定 CCS null rectangle 與兩個 event-vector capacity failure；多個 callers 仍忽略 result 或容許 partial progress。SharedUserClient ICB 的兩次，以及 `IGAccelResource::pageon/pageoff` 的三次／兩次 `submitBlit` 都不檢查 AL，不能宣稱 fail closed。 |
| SG-09 | timer／IRQ／workloop callback 的取消、排空與 owner lifetime | OPEN | IRQ callback counted gate 已有；DPSM、event-machine、passive timer、workloop removal 的完整 no-late-callback／無反向鎖序證明尚未閉合。 |
| SG-10 | 所有 VF 可達 PF-owned MMIO／DMA／force-wake／reset 的 negative reachability | OPEN | V292 已隔離 PAVP callback 的 force-wake/PF MMIO 與 `recognizeFlip` telemetry submission；仍須以完整 symbol/vtable/function-pointer inventory 證明沒有 retained native bypass。 |
| SG-11 | baseline 要求的所有程式檔完整審閱與 ledger closure | OPEN | `SOURCE_REVIEW_COVERAGE.md` 仍明確標記 incomplete；新增／修改檔案也必須納入。CI 成功不能替代此項。 |
| SG-12 | 精確候選 commit 的完整 static suite、x86_64 release kext、Metal smoke build 與 artifact provenance | REVALIDATE PER CANDIDATE | V294 `fb1a961` 的 exact-SHA push CI `37211776358` 已通過，兩份 artifacts 均未過期；目前 V295 worktree 的 full static `/tmp/ngreen-static.6zGGEk` 已通過，但仍待 clean commit/push 與新 SHA 的 CI/artifacts。這不解除 SG-05 至 SG-11。 |

## 目前主路徑

先關閉 SG-05。Tahoe 25G229 的 `IOAccelCommandQueue::submit_command_buffers`
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

## 靜態轉動態的交接條件

只有 SG-01 至 SG-11 全部為 `CLOSED`，且 SG-12 在同一個 clean/pushed commit 上完成
即時重驗，才可建立一次性候選
artifact。之後仍必須另外通過 `HOST_CONTAINMENT_PLAN.md` 的 current-boot root journal、
destroy-only XML/watchdog、獨立 host watcher、固定 deadline 與 cooldown 檢查；這些是
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
| P8 counted admission 實作 | CLOSED-STATIC | 17 個 outer roots 已 route；12 組 object→accelerator offsets、四個 direct accelerator callbacks 與 DisplaySleep ABI/route 均由 pinned binaries/source contracts 固定。PF／非目標 receiver pass-through，GL inherited selector 2 不重複 lease，低層 retirement bridge 保留。143-route 清冊與 8 個 source mutations 通過。 |
| P9 close→drain→native stop 鎖序 | REVIEWING | production 已是 close→最多 5 秒 drain→`gVfDeviceStopping`→native stop，並禁止使用 Tahoe 永遠回 true 的 `isLockedByCurrentThread` 作 owner 證明。仍須完成 stop caller／失敗回滾入口的鎖狀態，以及 display/GART/cache/KD/DisplaySleep callback cancel、late-entry 與 owner lifetime 證明；`finishAllStamps` 不受低層 gate 阻斷已由 route 排除固定。 |
