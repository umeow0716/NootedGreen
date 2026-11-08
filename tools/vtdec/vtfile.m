// vtfile: decode an Annex B HEVC file or an IVF VP9 file with the hardware
// decoder required and write the frames (decode order), cropped, as NV12 or
// P010 (VTFILE_P010) to the output file.
//   vtfile in.265|in.ivf out.yuv
#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>

extern const CFStringRef kVTVideoDecoderSpecification_AllowVP9Decode; // VideoToolbox, private

static FILE *out;
static int decoded, errors;

static void decodedCb(void *ref, void *src, OSStatus st, VTDecodeInfoFlags f, CVImageBufferRef img, CMTime pts, CMTime dur)
{
  if (st || !img) { errors++; printf("decode error %d\n", (int)st); return; }
  CVPixelBufferLockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
  const size_t bytes = CVPixelBufferGetPixelFormatType(img) == kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange ? 2 : 1;
  for (size_t p = 0; p < 2; p++) {
    const uint8_t *base = CVPixelBufferGetBaseAddressOfPlane(img, p);
    const size_t pitch = CVPixelBufferGetBytesPerRowOfPlane(img, p), h = CVPixelBufferGetHeightOfPlane(img, p);
    const size_t w = CVPixelBufferGetWidthOfPlane(img, p) * (p ? 2 : 1) * bytes;
    for (size_t r = 0; r < h; r++) fwrite(base + r * pitch, 1, w, out);
  }
  CVPixelBufferUnlockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
  decoded++;
}

int main(int argc, char **argv)
{
  setvbuf(stdout, NULL, _IONBF, 0);
  if (argc < 3) { printf("usage: vtfile in.265 out.yuv\n"); return 1; }
  @autoreleasepool {
    NSData *file = [NSData dataWithContentsOfFile:@(argv[1])];
    const uint8_t *d = file.bytes;
    const size_t n = file.length;
    NSMutableArray<NSMutableData *> *frames = [NSMutableArray array];
    CMFormatDescriptionRef fmt = NULL;
    OSStatus st;
    if (n > 32 && !memcmp(d, "DKIF", 4)) {
      // IVF: 32-byte header (fourcc +8, width +12, height +14), frames of
      // {u32 size, u64 pts, data}.
      const int w = d[12] | d[13] << 8, h = d[14] | d[15] << 8;
      for (size_t o = d[6] | d[7] << 8; o + 12 <= n;) {
        const uint32_t size = d[o] | d[o + 1] << 8 | d[o + 2] << 16 | (uint32_t)d[o + 3] << 24;
        if (o + 12 + size > n) break;
        [frames addObject:[NSMutableData dataWithBytes:d + o + 12 length:size]];
        o += 12 + size;
      }
      // VP Codec Configuration Box: profile, level, bit depth / 4:2:0 / range, BT.709.
      const bool tenBit = getenv("VTFILE_P010") != NULL;
      const uint8_t vpcc[12] = {1, 0, 0, 0, tenBit ? 2 : 0, 41, (uint8_t)((tenBit ? 10 : 8) << 4 | 1 << 1), 1, 1, 1, 0, 0};
      VTRegisterSupplementalVideoDecoderIfAvailable(kCMVideoCodecType_VP9);
      printf("VP9 hardware decode supported: %d\n", VTIsHardwareDecodeSupported(kCMVideoCodecType_VP9));
      NSDictionary *ext = @{(id)kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms: @{@"vpcC": [NSData dataWithBytes:vpcc length:12]}};
      st = CMVideoFormatDescriptionCreate(NULL, kCMVideoCodecType_VP9, w, h, (__bridge CFDictionaryRef)ext, &fmt);
      printf("ivf %dx%d, %lu frames, format %d\n", w, h, (unsigned long)frames.count, (int)st);
    } else {
    // NAL units between start codes.
    NSMutableArray<NSValue *> *nals = [NSMutableArray array];
    for (size_t i = 0, start = 0; i + 3 <= n; i++) {
      if (d[i] || d[i + 1] || d[i + 2] != 1) { if (i + 3 == n && start) [nals addObject:[NSValue valueWithRange:NSMakeRange(start, n - start)]]; continue; }
      if (start) {
        size_t end = i;
        while (end > start && !d[end - 1]) end--;
        [nals addObject:[NSValue valueWithRange:NSMakeRange(start, end - start)]];
      }
      start = i + 3;
      i += 2;
    }
    const uint8_t *ps[3] = {0}; size_t psLen[3] = {0};
    for (NSValue *v in nals) {
      const NSRange r = v.rangeValue;
      const int type = d[r.location] >> 1 & 0x3f;
      if (type >= 32 && type <= 34) { if (!ps[type - 32]) { ps[type - 32] = d + r.location; psLen[type - 32] = r.length; } continue; }
      if (type > 31) continue;
      if (d[r.location + 2] & 0x80 || !frames.count) [frames addObject:[NSMutableData data]];
      const uint32_t len = CFSwapInt32HostToBig((uint32_t)r.length);
      [frames.lastObject appendBytes:&len length:4];
      [frames.lastObject appendBytes:d + r.location length:r.length];
    }
    st = CMVideoFormatDescriptionCreateFromHEVCParameterSets(NULL, 3, ps, psLen, 4, NULL, &fmt);
    printf("%lu nals, %lu frames, format %d\n", (unsigned long)nals.count, (unsigned long)frames.count, (int)st);
    }
    if (st) return 1;
    const bool tenBit = getenv("VTFILE_P010") != NULL;
    VTDecompressionOutputCallbackRecord cb = {decodedCb, NULL};
    VTDecompressionSessionRef dec;
    st = VTDecompressionSessionCreate(NULL, fmt,
      (__bridge CFDictionaryRef)(getenv("VTFILE_SW")
        ? @{(id)kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder: @NO}
        : @{(id)kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder: @YES,
            (id)kVTVideoDecoderSpecification_AllowVP9Decode: @YES}),
      (__bridge CFDictionaryRef)@{(id)kCVPixelBufferPixelFormatTypeKey:
        @(tenBit ? kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange)},
      &cb, &dec);
    printf("decoder create %d\n", (int)st);
    if (st) return 1;
    out = fopen(argv[2], "wb");
    int i = 0;
    for (NSMutableData *frame in frames) {
      CMBlockBufferRef block = NULL;
      CMBlockBufferCreateWithMemoryBlock(NULL, NULL, frame.length, NULL, NULL, 0, frame.length, 0, &block);
      CMBlockBufferReplaceDataBytes(frame.bytes, block, 0, frame.length);
      CMSampleTimingInfo timing = {CMTimeMake(1, 30), CMTimeMake(i, 30), kCMTimeInvalid};
      size_t size = frame.length;
      CMSampleBufferRef sample = NULL;
      CMSampleBufferCreateReady(NULL, block, fmt, 1, 1, &timing, 1, &size, &sample);
      VTDecompressionSessionDecodeFrame(dec, sample, 0, NULL, NULL);
      VTDecompressionSessionWaitForAsynchronousFrames(dec);
      CFRelease(sample);
      CFRelease(block);
      i++;
    }
    fclose(out);
    printf("decoded %d errors %d\n", decoded, errors);
    return decoded == (int)frames.count && !errors ? 0 : 2;
  }
}
