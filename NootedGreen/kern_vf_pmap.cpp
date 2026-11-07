//  Copyright © 2026 Stezza @ inc. Licensed under the Thou Shalt Not Profit License version 1.0. See LICENSE for
//  details.
#include "kern_vf_internal.hpp"

namespace V373 {
struct Pv {
	Pv *next, *prev;
	uint64_t vaFlags;
	void *pmap;
	uint32_t ppn;
	Pv *nexth;
};
static mach_vm_address_t orgRemove;
static Pv **pvHead;
static char **pvLock;
static Pv ***hashTable;
static char **hashLock;
static uint32_t *hashMask;
static uint32_t *lastManaged;
static void **kernelPmap;
static uint64_t (*physRead)(uint64_t);
static void (*noPreempt)();
static void (*preempt)();
static uint32_t repairs;
constexpr uint64_t sharedBase = 0x7FF800000000ULL, sharedEnd = 0x800000000000ULL;
constexpr uint64_t frame = 0x000FFFFFFFFFF000ULL, nested = 1ULL << 53;
constexpr uint32_t chunk = 256;

static void bitLock(uint32_t bit, char *table) {
	asm volatile("jmp 1f\n0: btl %0, %1\njb 0b\n1: lock; btsl %0, %1\njb 0b" : : "r"(bit), "m"(*reinterpret_cast<volatile int *>(table)) : "memory");
}

static void bitUnlock(uint32_t bit, char *table) {
	asm volatile("lock; btrl %0, %1" : : "r"(bit), "m"(*reinterpret_cast<volatile int *>(table)) : "memory");
}

static uint32_t hashIndex(void *pmap, uint64_t va) {
	return (static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pmap)) ^ static_cast<uint32_t>(va >> 12)) & *hashMask;
}

// Physical address of the PT page covering va, or 0.
static uint64_t ptPage(void *pmap, uint64_t va) {
	auto *pml4 = *reinterpret_cast<uint64_t **>(static_cast<uint8_t *>(pmap) + 0x50);
	if (!pml4)
		return 0;
	const uint64_t pml4e = pml4[(va >> 39) & 511];
	if (!(pml4e & 1))
		return 0;
	const uint64_t pdpte = physRead((pml4e & frame) + ((va >> 30) & 511) * 8);
	if (!(pdpte & 1) || (pdpte & (nested | 0x80)))
		return 0;
	const uint64_t pde = physRead((pdpte & frame) + ((va >> 21) & 511) * 8);
	if (!(pde & 1) || (pde & (nested | 0x80)))
		return 0;
	return pde & frame;
}

static void unlinkHashed(Pv *e) {
	const uint32_t idx = hashIndex(e->pmap, e->vaFlags & ~0xFFFULL);
	bitLock(idx, *hashLock);
	Pv **prev = &(*hashTable)[idx];
	while (*prev && *prev != e)
		prev = &(*prev)->nexth;
	if (*prev)
		*prev = e->nexth;
	e->prev->next = e->next;
	e->next->prev = e->prev;
	bitUnlock(idx, *hashLock);
}

// Called after the removal: drop a PV entry of (pmap, va) on pa if no PTE maps it.
static void repair(void *pmap, uint64_t va, uint64_t pa, int options, void *caller) {
	const uint32_t pai = static_cast<uint32_t>(pa >> 12);
	if (pai > *lastManaged)
		return;
	noPreempt();
	bitLock(pai, *pvLock);
	// pv_head_table holds 32-byte rooted entries (no ppn/nexth).
	Pv *root = reinterpret_cast<Pv *>(reinterpret_cast<uint8_t *>(*pvHead) + static_cast<uint64_t>(pai) * 32);
	Pv *stale = nullptr;
	if (root->pmap) {
		Pv *e = root;
		do {
			if (e->pmap == pmap && (e->vaFlags & ~0xFFFULL) == va) {
				stale = e;
				break;
			}
			e = e->next;
		} while (e && e != root);
	}
	if (stale) {
		const uint64_t pt = ptPage(pmap, va);
		const uint64_t pte = pt ? physRead(pt + ((va >> 12) & 511) * 8) : 0;
		if ((pte & 1) && (pte & frame) == pa) {
			stale = nullptr;
		} else if (stale != root) {
			unlinkHashed(stale);
		} else if (root->next != root) {
			Pv *h = root->next;
			unlinkHashed(h);
			root->pmap = h->pmap;
			root->vaFlags = h->vaFlags;
		} else {
			root->pmap = nullptr;
		}
	}
	bitUnlock(pai, *pvLock);
	preempt();
	if (stale && repairs++ < 64)
		SYSLOG("ngreen", "V373: unlinked stale PV pmap=%p va=0x%llx pa=0x%llx options=0x%x caller=%p", pmap, va, pa, options, caller);
}

static void wrapRemove(void *pmap, uint64_t start, uint64_t end, int options) {
	const auto org = FunctionCast(wrapRemove, orgRemove);
	if (pmap == *kernelPmap || end <= sharedBase || start >= sharedEnd) {
		org(pmap, start, end, options);
		return;
	}
	if (start < sharedBase) {
		org(pmap, start, sharedBase, options);
		start = sharedBase;
	}
	const uint64_t stop = end < sharedEnd ? end : sharedEnd;
	struct { uint64_t va, pa; } seen[chunk];
	while (start < stop) {
		// Note up to `chunk` present PTEs, then remove exactly that span.
		uint32_t n = 0;
		uint64_t va = start, cut = stop;
		while (va < stop) {
			const uint64_t pt = ptPage(pmap, va);
			const uint64_t pdEnd = (va + 0x200000) & ~0x1FFFFFULL;
			const uint64_t to = pdEnd < stop ? pdEnd : stop;
			for (; pt && va < to; va += 0x1000) {
				const uint64_t pte = physRead(pt + ((va >> 12) & 511) * 8);
				if (!(pte & frame))
					continue;
				if (n == chunk) {
					cut = va;
					break;
				}
				seen[n++] = {va, pte & frame};
			}
			if (cut != stop)
				break;
			va = to;
		}
		org(pmap, start, cut, options);
		for (uint32_t i = 0; i < n; i++)
			repair(pmap, seen[i].va, seen[i].pa, options, __builtin_return_address(0));
		start = cut;
	}
	if (end > sharedEnd)
		org(pmap, sharedEnd, end, options);
}

void install(KernelPatcher &patcher) {
	pvHead = reinterpret_cast<Pv **>(patcher.solveSymbol(KernelPatcher::KernelID, "_pv_head_table"));
	pvLock = reinterpret_cast<char **>(patcher.solveSymbol(KernelPatcher::KernelID, "_pv_lock_table"));
	hashTable = reinterpret_cast<Pv ***>(patcher.solveSymbol(KernelPatcher::KernelID, "_pv_hash_table"));
	hashLock = reinterpret_cast<char **>(patcher.solveSymbol(KernelPatcher::KernelID, "_pv_hash_lock_table"));
	hashMask = reinterpret_cast<uint32_t *>(patcher.solveSymbol(KernelPatcher::KernelID, "_npvhashmask"));
	lastManaged = reinterpret_cast<uint32_t *>(patcher.solveSymbol(KernelPatcher::KernelID, "_last_managed_page"));
	kernelPmap = reinterpret_cast<void **>(patcher.solveSymbol(KernelPatcher::KernelID, "_kernel_pmap"));
	physRead = reinterpret_cast<decltype(physRead)>(patcher.solveSymbol(KernelPatcher::KernelID, "_ml_phys_read_double_64"));
	noPreempt = reinterpret_cast<decltype(noPreempt)>(patcher.solveSymbol(KernelPatcher::KernelID, "__disable_preemption"));
	preempt = reinterpret_cast<decltype(preempt)>(patcher.solveSymbol(KernelPatcher::KernelID, "__enable_preemption"));
	if (!pvHead || !pvLock || !hashTable || !hashLock || !hashMask || !lastManaged || !kernelPmap || !physRead || !noPreempt || !preempt) {
		SYSLOG("ngreen", "V373: pmap symbols missing, stale PV repair off");
		patcher.clearError();
		return;
	}
	KernelPatcher::RouteRequest request("_pmap_remove_options", wrapRemove, orgRemove);
	if (patcher.routeMultiple(KernelPatcher::KernelID, &request, 1))
		SYSLOG("ngreen", "V373: stale PV repair routed on pmap_remove_options");
	else
		SYSLOG("ngreen", "V373: cannot route pmap_remove_options");
	patcher.clearError();
}
}
