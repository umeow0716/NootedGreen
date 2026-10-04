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
| SG-05 | modern 外部 producer 在 stop 前可封門、排空，且不阻斷 `finishAllStamps` retirement | REVIEWING | `IOAccelCommandQueue` selector 1 與 legacy `IOAccelContext2` selector 2 的 submit、pause 放鎖/重取及 stop scope 已完整固定；SharedUserClient depth/color/ICB 三個獨立 roots 亦已辨識。尚須閉合 driver overrides、display/flip 與所有非 user-triggered producers，實作 receiver-scoped counted admission，並證明 close/drain 鎖序。 |
| SG-06 | reservation → CPU ring writes → tail publication → GuC submit 為一致的 owner/admission transaction | OPEN | reservation postcondition、ring geometry、retained backing 與 final submit validation 已有；但 native writer 在 final routed submit 前的跨呼叫區間仍沒有完整 lease。 |
| SG-07 | GPU completion 與 ring/context/mapping/page-table backing 的最終釋放順序 | OPEN | GuC context deregistration與 heavy TLB ACK 已覆蓋選定 teardown；尚未證明所有完成事件、stamp、pool reuse 與 producer owner 都在釋放前退休。 |
| SG-08 | render／depth／CCS／ICB 的 allocation、event collection、partial submit 與錯誤傳遞 | OPEN | 已修補選定 CCS null rectangle 與兩個 event-vector capacity failure；多個 callers 仍忽略 result 或容許 partial progress。SharedUserClient ICB 的兩次 `submitBlit` 也都不檢查 AL，不能宣稱 fail closed。 |
| SG-09 | timer／IRQ／workloop callback 的取消、排空與 owner lifetime | OPEN | IRQ callback counted gate 已有；DPSM、event-machine、passive timer、workloop removal 的完整 no-late-callback／無反向鎖序證明尚未閉合。 |
| SG-10 | 所有 VF 可達 PF-owned MMIO／DMA／force-wake／reset 的 negative reachability | OPEN | 已隔離多批具名入口；仍須以完整 symbol/vtable/function-pointer inventory 證明沒有 retained native bypass。 |
| SG-11 | baseline 要求的所有程式檔完整審閱與 ledger closure | OPEN | `SOURCE_REVIEW_COVERAGE.md` 仍明確標記 incomplete；新增／修改檔案也必須納入。CI 成功不能替代此項。 |
| SG-12 | 精確候選 commit 的完整 static suite、x86_64 release kext、Metal smoke build 與 artifact provenance | REVALIDATE PER CANDIDATE | 最後已知綠燈產品樹是 V284 `f0c76d5`（GitHub Actions `37196931380`）；V285 未改產品碼，完整本機 static suite 已通過（`/tmp/ngreen-static.mpbxos`），但仍須在決定動態候選時核對該 clean/pushed SHA 的 CI 與 artifact。任何產品碼或 payload 變更立即重開本項。 |

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

因此一般 command queue 可以使用「VF accelerator receiver identity + 外層 counted
admission」封門，但尚不能把它當成全域 producer gate。SharedUserClient
depth/color/ICB 有獨立 external roots；native display/flip、command-buffer pool、driver
override 與非 user-triggered producer 的旁路或
不可達性仍要逐一閉合。不得只在 `vfSubmitWorkItem`、`submitToRing` 或
`waitForSpace` 單點拒絕：前兩者太晚才涵蓋 CPU ring writes，後者無法以一般 RAII 跨越
後續 writer 與 submission，且部分 callers 不檢查 reservation Boolean。

## 靜態轉動態的交接條件

只有 SG-01 至 SG-11 全部為 `CLOSED`，且 SG-12 在同一個 clean/pushed commit 上完成
即時重驗，才可建立一次性候選
artifact。之後仍必須另外通過 `HOST_CONTAINMENT_PLAN.md` 的 current-boot root journal、
destroy-only XML/watchdog、獨立 host watcher、固定 deadline 與 cooldown 檢查；這些是
動態 preflight，不可由本表的離線結果代替。第一輪只允許最短、單次、可強制摧毀的
contained boot，不是效能、Metal completion、媒體或 Looking Glass 測試。
