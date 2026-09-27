//! Copyright © 2022-2023 ChefKiss Inc. Licensed under the Thou Shalt Not Profit License version 1.5.
//! See LICENSE for details.

#pragma once
#include <Headers/kern_patcher.hpp>

struct LookupPatchPlus : KernelPatcher::LookupPatch {
	const UInt8 *findMask {nullptr}, *replaceMask {nullptr};
	const size_t skip {0};

	LookupPatchPlus(KernelPatcher::KextInfo *kext, const UInt8 *find, const UInt8 *replace, size_t size, size_t count,
		size_t skip = 0)
		: KernelPatcher::LookupPatch {kext, find, replace, size, count}, skip {skip} {}

	LookupPatchPlus(KernelPatcher::KextInfo *kext, const UInt8 *find, const UInt8 *findMask, const UInt8 *replace,
		size_t size, size_t count, size_t skip = 0)
		: KernelPatcher::LookupPatch {kext, find, replace, size, count}, findMask {findMask}, skip {skip} {}

	LookupPatchPlus(KernelPatcher::KextInfo *kext, const UInt8 *find, const UInt8 *findMask, const UInt8 *replace,
		const UInt8 *replaceMask, size_t size, size_t count, size_t skip = 0)
		: KernelPatcher::LookupPatch {kext, find, replace, size, count}, findMask {findMask}, replaceMask {replaceMask},
		  skip {skip} {}

	// Keep array overloads one argument shorter than their raw-pointer forms.
	// Otherwise a mutable replacement array makes a five-argument
	// (find, replace, size, count) call prefer this template and reinterpret
	// size as count plus count as skip. Callers that intentionally need skip
	// must use the explicit raw-pointer (size, count, skip) form.
	template<size_t N>
	LookupPatchPlus(KernelPatcher::KextInfo *kext, const UInt8 (&find)[N], const UInt8 (&replace)[N], size_t count)
		: LookupPatchPlus {kext, find, replace, N, count, 0} {}

	template<size_t N>
	LookupPatchPlus(KernelPatcher::KextInfo *kext, const UInt8 (&find)[N], const UInt8 (&findMask)[N],
		const UInt8 (&replace)[N], size_t count)
		: LookupPatchPlus {kext, find, findMask, replace, N, count, 0} {}

	template<size_t N>
	LookupPatchPlus(KernelPatcher::KextInfo *kext, const UInt8 (&find)[N], const UInt8 (&findMask)[N],
		const UInt8 (&replace)[N], const UInt8 (&replaceMask)[N], size_t count)
		: LookupPatchPlus {kext, find, findMask, replace, replaceMask, N, count, 0} {}

	bool apply(KernelPatcher &patcher, mach_vm_address_t address, size_t maxSize) const;

	static bool applyAll(KernelPatcher &patcher, const LookupPatchPlus *patches, size_t count,
		mach_vm_address_t address, size_t maxSize);

	template<size_t N>
	static bool applyAll(KernelPatcher &patcher, const LookupPatchPlus (&patches)[N], mach_vm_address_t address,
		size_t maxSize) {
		return applyAll(patcher, patches, N, address, maxSize);
	}

private:
	bool preflight(mach_vm_address_t address, size_t maxSize) const;
	bool applyPrepared(mach_vm_address_t address, size_t maxSize) const;
};
