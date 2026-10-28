// Tahoe compatibility shim for the TGL Metal UMD.
//
// The UMD was built against an older Metal.framework and reads the private
// descriptor structs (-_descriptorPrivate) with that release's layout. This
// library swizzles the accessors and, for calls coming from the UMD only,
// returns a per-thread copy in the legacy layout. Every other caller sees the
// native struct.
//
// The UMD links @loader_path/impostor.dylib for its load-time constructor only
// (no imported symbols), so this library is installed under that name.
// Layout knowledge follows the WhateverXe-Graphics/Moraea shims; the guards
// check the ABI itself rather than an OS build number, so an update that keeps
// the layout keeps working and one that changes it disables the shim (logged).
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <string.h>

static const char kUMDSuffix[] =
  "/AppleIntelTGLGraphicsMTLDriver.bundle/Contents/MacOS/AppleIntelTGLGraphicsMTLDriver";

static bool fromUMD(const void * address)
{
  Dl_info info;
  if (!dladdr(address, &info) || !info.dli_fname)
    return false;
  const size_t length = strlen(info.dli_fname);
  return length >= sizeof(kUMDSuffix) - 1 &&
    !strcmp(info.dli_fname + length - (sizeof(kUMDSuffix) - 1), kUMDSuffix);
}

typedef const uint8_t * (*Accessor)(id, SEL);
static Accessor nativeRender, nativeCompute, nativeAttachment, nativePass;

static inline uint64_t load64(const uint8_t * p, size_t offset)
{
  uint64_t v;
  memcpy(&v, p + offset, sizeof(v));
  return v;
}

static inline void store64(uint8_t * p, size_t offset, uint64_t v)
{
  memcpy(p + offset, &v, sizeof(v));
}

// MTLRenderPipelineDescriptorPrivate: Tahoe 0x338-byte object, legacy 0x190.
static __thread uint8_t renderLegacy[0x190] __attribute__((aligned(16)));

static const uint8_t * render(id self, SEL _cmd)
{
  const uint8_t * s = nativeRender(self, _cmd);
  if (!fromUMD(__builtin_return_address(0)))
    return s;

  uint8_t * d = renderLegacy;
  memset(d, 0, sizeof(renderLegacy));
  memcpy(d, s, 0x99);
  static const struct { uint16_t dst, src, size; } map[] = {
    {0xa0, 0xb0, 8}, {0xa8, 0xb8, 8}, {0xb0, 0xc0, 8}, {0xb8, 0xc8, 4},
    {0xc8, 0xd8, 8}, {0xd0, 0xe0, 16},
    {0xe0, 0xf0, 40},
    {0x108, 0x198, 8}, {0x110, 0x1a0, 8}, {0x118, 0x1a8, 8}, {0x120, 0x1b8, 8},
    {0x138, 0x1d0, 8}, {0x140, 0x1d8, 1}, {0x144, 0x1dc, 4},
    {0x148, 0x1e0, 32},
    {0x168, 0x210, 8}, {0x170, 0x218, 8}, {0x178, 0x230, 8}, {0x180, 0x238, 8},
    {0x188, 0x240, 2},
  };
  for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); ++i)
    memcpy(d + map[i].dst, s + map[i].src, map[i].size);

  // Flags: Tahoe added two bits (1 and 3) the legacy layout cannot express,
  // and a 3-bit field at 29..31 that it drops.
  const uint64_t q = load64(s, 0xe0);
  if (q & 0xa)
  {
    static bool warned;
    if (!warned)
    {
      warned = true;
      NSLog(@"ngshim: unrepresentable render pipeline flags %#llx", q);
    }
  }
  store64(d, 0xd0, (q & 1) | ((q >> 1) & 2) | ((q >> 2) & 0x7fffffcULL) |
      ((q >> 5) & ~0x7ffffffULL));
  return d;
}

// MTLComputePipelineDescriptorPrivate: Tahoe 0xf0 bytes, legacy 0xa8.
static __thread uint8_t computeLegacy[0xa8] __attribute__((aligned(16)));
static NSDictionary * emptyDictionary;

static const uint8_t * compute(id self, SEL _cmd)
{
  const uint8_t * s = nativeCompute(self, _cmd);
  if (!fromUMD(__builtin_return_address(0)))
    return s;

  uint8_t * d = computeLegacy;
  memset(d, 0, sizeof(computeLegacy));
  static const struct { uint16_t dst, src, size; } map[] = {
    {0x00, 0x00, 16}, {0x10, 0x10, 1}, {0x12, 0x12, 2}, {0x18, 0x18, 16},
    {0x28, 0x30, 32}, {0x48, 0x50, 1}, {0x50, 0x68, 8}, {0x58, 0x70, 1},
    {0x60, 0x78, 16}, {0x70, 0x88, 1}, {0x78, 0x90, 8}, {0x88, 0x98, 16},
    {0x98, 0xa8, 1}, {0xa0, 0xb0, 8},
  };
  for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); ++i)
    memcpy(d + map[i].dst, s + map[i].src, map[i].size);
  d[0x71] = s[0x89] & 3;
  const void * dict = (__bridge const void *)emptyDictionary;
  memcpy(d + 0x80, &dict, sizeof(dict));
  return d;
}

// MTLRenderPipelineAttachmentDescriptorPrivate bitfield: Tahoe inserted bits
// 1 and 36.
static __thread uint64_t attachmentLegacy;

static const uint8_t * attachment(id self, SEL _cmd)
{
  const uint8_t * s = nativeAttachment(self, _cmd);
  if (!fromUMD(__builtin_return_address(0)))
    return s;

  const uint64_t q = load64(s, 0);
  attachmentLegacy = (q & 1) | ((q >> 1) & 0x7fffffeULL) |
    (((q >> 32) & 0xf) << 27) | ((q >> 37) << 31);
  return (const uint8_t *)&attachmentLegacy;
}

// MTLRenderPassDescriptorPrivate: Tahoe added a char at 0x29.
static __thread uint8_t passLegacy[0xc0] __attribute__((aligned(16)));

static const uint8_t * renderPass(id self, SEL _cmd)
{
  const uint8_t * s = nativePass(self, _cmd);
  if (!fromUMD(__builtin_return_address(0)))
    return s;

  memcpy(passLegacy, s, sizeof(passLegacy));
  passLegacy[0x29] = s[0x2a];
  passLegacy[0x2a] = s[0x2b];
  passLegacy[0x2b] = 0;
  return passLegacy;
}

// Binary archives need a target architecture the legacy driver can't name;
// fail cleanly instead of letting Metal build an archive the UMD can't use.
typedef id (*NewArchive)(id, SEL, id, NSError **);
static NewArchive nativeArchive;

static id newArchive(id self, SEL _cmd, id descriptor, NSError ** error)
{
  Class cls = object_getClass(self);
  const char * image = class_getImageName(cls);
  const size_t length = image ? strlen(image) : 0;
  const bool umd = length >= sizeof(kUMDSuffix) - 1 &&
    !strcmp(image + length - (sizeof(kUMDSuffix) - 1), kUMDSuffix);
  SEL target = sel_registerName("targetDeviceArchitecture");
  if (umd && (![self respondsToSelector:target] ||
        !((id (*)(id, SEL))objc_msgSend)(self, target)))
  {
    if (error)
      *error = [NSError errorWithDomain:MTLBinaryArchiveDomain code:4 userInfo:@{
        NSLocalizedDescriptionKey: @"This legacy driver has no target architecture for binary archives"}];
    return nil;
  }
  return nativeArchive(self, _cmd, descriptor, error);
}

// The native accessors are `lea rax, [rdi + offset]; ret`.
static bool accessorAt(Method m, uint8_t offset)
{
  const uint8_t expected[] = {0x55, 0x48, 0x89, 0xe5, 0x48, 0x8d, 0x47, offset, 0x5d, 0xc3};
  return m && !memcmp((const void *)method_getImplementation(m), expected, sizeof(expected));
}

static Method accessor(const char * cls, size_t size, uint8_t offset, const char * encoding)
{
  Class c = objc_getClass(cls);
  [c class]; // realize it: sizes and method lists are lazy
  Method m = c ? class_getInstanceMethod(c, sel_registerName("_descriptorPrivate")) : NULL;
  if (!m || class_getInstanceSize(c) != size || !accessorAt(m, offset) ||
      strcmp(method_getTypeEncoding(m), encoding))
  {
    NSLog(@"ngshim: %s layout changed, not translating it", cls);
    return NULL;
  }
  return m;
}

__attribute__((constructor)) static void install(void)
{
  @autoreleasepool
  {
    emptyDictionary = [NSDictionary new];

    Method r = accessor("MTLRenderPipelineDescriptorInternal", 0x338, 0x30,
      "r^{MTLRenderPipelineDescriptorPrivate=@[8Q]QQQQcQQQQQcqqq(?=QQ)Q(?=If)QQ(?=[2I]"
      "{?=b2b2b1b2b1b1b1b1b1b1b1b1b1b1b8b3b1b4b1b1b1b1b1b1b6})IIQ@@@@@@{?=QQQ}{?=QQQ}"
      "{?=QQQ}Q@@QQ@@@@@^v^v@cI@Q@@@@@@@@QQccQQcc@IQ{?=QQQ}{?=QQQ}@qQQQQQ}16@0:8");
    Method c = accessor("MTLComputePipelineDescriptorInternal", 0x100, 0x10,
      "r^{MTLComputePipelineDescriptorPrivate=@@cS@@@@@(?=@@)@cqqqcQQ@cb1b1b1b29@@QcQ@Q"
      "{?=QQQ}@}16@0:8");
    Method a = accessor("MTLRenderPipelineColorAttachmentDescriptorInternal", 0x10, 0x08,
      "r^{MTLRenderPipelineAttachmentDescriptorPrivate=(?={?=b2b3b3b5b5b5b5b5b1b4b22}"
      "{?=Q})}16@0:8");
    Method p = accessor("MTLRenderPassDescriptorInternal", 0x100, 0x30,
      "r^{MTLRenderPassDescriptorPrivate=@@QQQccccQQQ(?=QQ)QQ[8{?=ff}]Q@@cqc}16@0:8");

    // The pipeline flags and the attachment bits only make sense together.
    if (r && a)
    {
      nativeRender = (Accessor)method_setImplementation(r, (IMP)render);
      nativeAttachment = (Accessor)method_setImplementation(a, (IMP)attachment);
    }
    if (c)
      nativeCompute = (Accessor)method_setImplementation(c, (IMP)compute);
    if (p)
      nativePass = (Accessor)method_setImplementation(p, (IMP)renderPass);

    Method archive = class_getInstanceMethod(objc_getClass("_MTLDevice"),
      @selector(newBinaryArchiveWithDescriptor:error:));
    if (archive && !strcmp(method_getTypeEncoding(archive), "@32@0:8@16^@24"))
      nativeArchive = (NewArchive)method_setImplementation(archive, (IMP)newArchive);
  }
}
