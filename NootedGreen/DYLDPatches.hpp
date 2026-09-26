//! Copyright © 2022-2023 ChefKiss Inc. Licensed under the Thou Shalt Not Profit License version 1.5.
//! See LICENSE for details.

#pragma once
#include <Headers/kern_patcher.hpp>
#include <Headers/kern_util.hpp>

class DYLDPatch {
    const void *find {nullptr}, *findMask {nullptr};
    const void *replace {nullptr}, *replaceMask {nullptr};
    const size_t size {0};
    const char *comment {nullptr};

    public:
    DYLDPatch(const void *find, const void *replace, size_t size, const char *comment)
        : find {find}, replace {replace}, size {size}, comment {comment} {}

    DYLDPatch(const void *find, const void *findMask, const void *replace, const void *replaceMask, size_t size,
        const char *comment)
        : find {find}, findMask {findMask}, replace {replace}, replaceMask {replaceMask}, size {size},
          comment {comment} {}

    DYLDPatch(const void *find, const void *findMask, const void *replace, size_t size, const char *comment)
        : find {find}, findMask {findMask}, replace {replace}, size {size}, comment {comment} {}

    template<typename T, size_t N>
    DYLDPatch(const T (&find)[N], const T (&replace)[N], const char *comment)
        : DYLDPatch(find, replace, N * sizeof(T), comment) {}

    template<typename T, size_t N>
    DYLDPatch(const T (&find)[N], const T (&findMask)[N], const T (&replace)[N], const T (&replaceMask)[N],
        const char *comment)
        : DYLDPatch(find, findMask, replace, replaceMask, N * sizeof(T), comment) {}

    template<typename T, size_t N>
    DYLDPatch(const T (&find)[N], const T (&findMask)[N], const T (&replace)[N], const char *comment)
        : DYLDPatch(find, findMask, replace, N * sizeof(T), comment) {}

	inline bool apply(void *data, size_t size) const {
		if (!data || !size || !this->find || !this->replace || !this->size)
			return false;
		const bool applied = KernelPatcher::findAndReplaceWithMask(data, size, this->find, this->size,
			this->findMask, this->findMask ? this->size : 0, this->replace, this->size,
			this->replaceMask, this->replaceMask ? this->size : 0);
		if (UNLIKELY(applied))
			DBGLOG("DYLD", "Applied '%s' patch", this->comment);
		return applied;
    }

    static inline void applyAll(const DYLDPatch *patches, size_t count, void *data, size_t size) {
        for (size_t i = 0; i < count; i++) { patches[i].apply(data, size); }
    }

    template<size_t N>
    static inline void applyAll(const DYLDPatch (&patches)[N], void *data, size_t size) {
        applyAll(patches, N, data, size);
    }
};

class DYLDPatches {
    public:
    static DYLDPatches *callback;

    void init();
    void processPatcher(KernelPatcher &patcher);

    private:
    mach_vm_address_t orgCsValidatePage {0};
    static void wrapCsValidatePage(vnode *vp, memory_object_t pager, memory_object_offset_t page_offset,
        const void *data, int *validated_p, int *tainted_p, int *nx_p);
};

//! VideoToolbox DRM model check
static const char kVideoToolboxDRMModelOriginal[] = "MacPro5,1\0MacPro6,1\0IOService";

static const char kHwGvaId[] = "Mac-7BA5B2D9E42DDD94";

//! AppleGVA model check
static const char kAGVABoardIdOriginal[] = "board-id\0hw.model";
static const char kAGVABoardIdPatched[] = "hwgva-id\0hw.model";

//! AppleGVAHEVCEncoder model check
static const char kHEVCEncBoardIdOriginal[] = "vendor8bit\0IOService\0board-id";
static const char kHEVCEncBoardIdPatched[] = "vendor8bit\0IOService\0hwgva-id";
