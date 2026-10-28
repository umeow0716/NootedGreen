#include <mach/mach_types.h>

extern kern_return_t NGXe_kern_start(kmod_info_t *, void *);
extern kern_return_t NGXe_kern_stop(kmod_info_t *, void *);

KMOD_EXPLICIT_DECL(io.umeow.NGXe, "1.0.0",
    NGXe_kern_start, NGXe_kern_stop)
__private_extern__ kmod_start_func_t *_realmain = 0;
__private_extern__ kmod_stop_func_t *_antimain = 0;
__private_extern__ int _kext_apple_cc = __APPLE_CC__;
