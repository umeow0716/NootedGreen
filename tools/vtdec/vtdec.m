#include <unistd.h>
// vtdec: software-encode N frames of H.264, then decode them with the
// hardware decoder required. Prints per-stage status and checks pixels.
#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>

static NSMutableArray *samples;
static NSMutableArray *sources;
static double psnrMin = 999;
static int decoded, decodeErrors, mismatches, base = 16;

static void encoded(void *ref, void *src, OSStatus st, VTEncodeInfoFlags f, CMSampleBufferRef sb)
{
  if (st || !sb) { printf("encode error %d\n", (int)st); return; }
  [samples addObject:(__bridge id)sb];
}

static void decodedCb(void *ref, void *src, OSStatus st, VTDecodeInfoFlags f,
  CVImageBufferRef img, CMTime pts, CMTime dur)
{
  if (st || !img) { decodeErrors++; printf("decode error %d\n", (int)st); return; }
  // Frame i was filled with luma 16 + 8*i (mod); check the centre pixel.
  int i = (int)pts.value;
  CVPixelBufferLockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
  uint8_t *y = CVPixelBufferGetBaseAddressOfPlane(img, 0);
  size_t pitch = CVPixelBufferGetBytesPerRowOfPlane(img, 0);
  int v = y[pitch * (CVPixelBufferGetHeight(img) / 2) + CVPixelBufferGetWidth(img) / 2], want = base + (8 * i) % 200;
  if (i < (int)sources.count) {
    // Luma PSNR against the source frame.
    NSData *src = sources[i]; const uint8_t *sp = src.bytes;
    size_t w = CVPixelBufferGetWidth(img), h = CVPixelBufferGetHeight(img), sw = src.length / h;
    double se = 0;
    for (size_t r = 0; r < h; r++) for (size_t c = 0; c < w; c++) { int d = y[r * pitch + c] - sp[r * sw + c]; se += d * d; }
    double psnr = se ? 10 * log10(255.0 * 255 * w * h / se) : 99;
    if (psnr < psnrMin) psnrMin = psnr;
    if (psnr < 30 && i < 6) printf("frame %d psnr %.1f\n", i, psnr);
    if (psnr < 30 && i < 6 && getenv("VTDEC_RECHECK")) {
      // Did the decode land late (early completion) or never?
      CVPixelBufferUnlockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
      usleep(500000);
      CVPixelBufferLockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
      y = CVPixelBufferGetBaseAddressOfPlane(img, 0);
      if (getenv("VTDEC_CLFLUSH")) {
        // Stale CPU cache lines over a non-snooped GPU write?
        for (size_t o = 0; o < pitch * h; o += 64) __builtin_ia32_clflush(y + o);
        __builtin_ia32_mfence();
      }
      double se2 = 0;
      for (size_t r = 0; r < h; r++) for (size_t c = 0; c < w; c++) { int d = y[r * pitch + c] - sp[r * sw + c]; se2 += d * d; }
      printf("frame %d recheck psnr %.1f\n", i, se2 ? 10 * log10(255.0 * 255 * w * h / se2) : 99);
    }
    if (psnr < 30 && i == 0 && getenv("VTDEC_DUMP")) {
      FILE *f = fopen("/tmp/vtdec-out.y", "wb");
      for (size_t r = 0; r < h; r++) fwrite(y + r * pitch, 1, w, f);
      fclose(f); f = fopen("/tmp/vtdec-src.y", "wb"); fwrite(sp, 1, src.length, f); fclose(f);
      printf("dumped %zux%zu pitch %zu fmt %08x\n", w, h, pitch, (unsigned)CVPixelBufferGetPixelFormatType(img));
    }
  }
  CVPixelBufferUnlockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
  if (abs(v - want) > 6) { mismatches++; if (mismatches < 4) printf("frame %d luma %d want %d\n", i, v, want); }
  decoded++;
}

int main(int argc, char **argv)
{
  setvbuf(stdout, NULL, _IONBF, 0);
  @autoreleasepool {
    const int N = argc > 1 ? atoi(argv[1]) : 30;
    if (getenv("VTDEC_BASE")) base = atoi(getenv("VTDEC_BASE"));
    const int W = argc > 3 ? atoi(argv[3]) : 1280, H = argc > 4 ? atoi(argv[4]) : 720;
    const bool hwEncode = argc > 2 && !strcmp(argv[2], "hwenc");
    const CMVideoCodecType codec = getenv("VTDEC_HEVC") ? kCMVideoCodecType_HEVC : kCMVideoCodecType_H264;
    samples = [NSMutableArray array];
    if (getenv("VTDEC_NOISE")) sources = [NSMutableArray array];

    NSMutableDictionary *spec = [(hwEncode
      ? @{(id)kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder: @YES}
      : @{(id)kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder: @NO}) mutableCopy];
    if (getenv("VTDEC_LOWLATENCY")) spec[(id)kVTVideoEncoderSpecification_EnableLowLatencyRateControl] = @YES;
    VTCompressionSessionRef enc;
    OSStatus st = VTCompressionSessionCreate(NULL, W, H, codec,
      (__bridge CFDictionaryRef)spec, NULL, NULL, encoded, NULL, &enc);
    printf("%s encoder (%s) create %d\n", codec == kCMVideoCodecType_HEVC ? "hevc" : "h264", hwEncode ? "hw" : "sw", (int)st);
    if (st) return 1;
    CFBooleanRef hw = NULL;
    VTSessionCopyProperty(enc, kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder, NULL, &hw);
    printf("encoder using hw: %d\n", hw == kCFBooleanTrue);
    VTSessionSetProperty(enc, kVTCompressionPropertyKey_RealTime, getenv("VTDEC_REALTIME") ? kCFBooleanTrue : kCFBooleanFalse);
    if (getenv("VTDEC_BITRATE"))
      printf("bitrate %d\n", (int)VTSessionSetProperty(enc, kVTCompressionPropertyKey_AverageBitRate, (__bridge CFTypeRef)@(atoi(getenv("VTDEC_BITRATE")))));
    if (getenv("VTDEC_HIGH") || getenv("VTDEC_MAIN"))
      printf("profile %d\n", (int)VTSessionSetProperty(enc, kVTCompressionPropertyKey_ProfileLevel, getenv("VTDEC_MAIN") ? kVTProfileLevel_H264_Main_AutoLevel : kVTProfileLevel_H264_High_AutoLevel));
    if (getenv("VTDEC_KEYINT"))
      VTSessionSetProperty(enc, kVTCompressionPropertyKey_MaxKeyFrameInterval, (__bridge CFTypeRef)@(atoi(getenv("VTDEC_KEYINT"))));

    for (int i = 0; i < N; i++) {
      CVPixelBufferRef pb = NULL;
      CVPixelBufferPoolRef pool = getenv("VTDEC_POOL") ? VTCompressionSessionGetPixelBufferPool(enc) : NULL;
      if (pool) CVPixelBufferPoolCreatePixelBuffer(NULL, pool, &pb);
      const int SW = getenv("VTDEC_SRCW") ? atoi(getenv("VTDEC_SRCW")) : W, SH = getenv("VTDEC_SRCH") ? atoi(getenv("VTDEC_SRCH")) : H;
      if (!pb) CVPixelBufferCreate(NULL, SW, SH, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
        (__bridge CFDictionaryRef)@{(id)kCVPixelBufferIOSurfacePropertiesKey: @{}}, &pb);
      if (i == 0) printf("input from %s, format %08x\n", pool ? "session pool" : "own buffer", (unsigned)CVPixelBufferGetPixelFormatType(pb));
      CVPixelBufferLockBaseAddress(pb, 0);
      for (size_t p = 0; p < 2; p++)
        memset(CVPixelBufferGetBaseAddressOfPlane(pb, p), p ? 128 : base + (8 * i) % 200,
          CVPixelBufferGetBytesPerRowOfPlane(pb, p) * CVPixelBufferGetHeightOfPlane(pb, p));
      if (getenv("VTDEC_NOISE")) {
        // Textured, moving content; keep the centre pixel at the expected luma.
        uint8_t *y = CVPixelBufferGetBaseAddressOfPlane(pb, 0);
        size_t pitch = CVPixelBufferGetBytesPerRowOfPlane(pb, 0), h = CVPixelBufferGetHeightOfPlane(pb, 0), w = CVPixelBufferGetWidthOfPlane(pb, 0);
        for (size_t r = 0; r < h; r++) for (size_t c = 0; c < w; c++) y[r * pitch + c] = (uint8_t)((r * 7 + c * 3 + i * 5) ^ (rand() & (getenv("VTDEC_AMP") ? atoi(getenv("VTDEC_AMP")) : 31)));
        for (size_t r = h / 2 - 8; r < h / 2 + 8; r++) memset(y + r * pitch + w / 2 - 8, base + (8 * i) % 200, 16);
      }
      if (sources) {
        size_t w = CVPixelBufferGetWidthOfPlane(pb, 0), h = CVPixelBufferGetHeightOfPlane(pb, 0), pitch = CVPixelBufferGetBytesPerRowOfPlane(pb, 0);
        NSMutableData *d = [NSMutableData dataWithLength:w * h];
        for (size_t r = 0; r < h; r++) memcpy((uint8_t *)d.mutableBytes + r * w, (uint8_t *)CVPixelBufferGetBaseAddressOfPlane(pb, 0) + r * pitch, w);
        [sources addObject:d];
      }
      CVPixelBufferUnlockBaseAddress(pb, 0);
      st = VTCompressionSessionEncodeFrame(enc, pb, CMTimeMake(i, 30), kCMTimeInvalid, NULL, NULL, NULL);
      CVPixelBufferRelease(pb);
      if (st) { printf("encode frame %d: %d\n", i, (int)st); break; }
    }
    VTCompressionSessionCompleteFrames(enc, kCMTimeInvalid);
    size_t bytes = 0;
    for (id s in samples) bytes += CMSampleBufferGetTotalSampleSize((__bridge CMSampleBufferRef)s);
    printf("encoded %lu frames, %zu bytes\n", (unsigned long)samples.count, bytes);
    if (!samples.count) return 1;

    CMFormatDescriptionRef fmt = CMSampleBufferGetFormatDescription((__bridge CMSampleBufferRef)samples[0]);
    for (size_t p = 0; codec == kCMVideoCodecType_H264 && p < 2; p++) {
      const uint8_t *ps; size_t n;
      if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fmt, p, &ps, &n, NULL, NULL)) break;
      printf("%s", p ? "pps " : "sps ");
      for (size_t i = 0; i < n; i++) printf("%02x", ps[i]);
      printf("\n");
    }
    if (getenv("VTDEC_DUMP")) {
      // Annex B copy of the stream for host-side inspection.
      const bool hevc = codec == kCMVideoCodecType_HEVC;
      FILE *f = fopen(hevc ? "/tmp/vtdec.265" : "/tmp/vtdec.264", "wb");
      static const uint8_t sc[4] = {0, 0, 0, 1};
      for (size_t p = 0; p < (hevc ? 3 : 2); p++) {
        const uint8_t *ps; size_t n;
        if (!(hevc ? CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(fmt, p, &ps, &n, NULL, NULL)
                   : CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fmt, p, &ps, &n, NULL, NULL))) { fwrite(sc, 1, 4, f); fwrite(ps, 1, n, f); }
      }
      for (id s in samples) {
        CMBlockBufferRef bb = CMSampleBufferGetDataBuffer((__bridge CMSampleBufferRef)s);
        size_t len = CMBlockBufferGetDataLength(bb); uint8_t *d = malloc(len);
        CMBlockBufferCopyDataBytes(bb, 0, len, d);
        for (size_t o = 0; o + 4 <= len;) { size_t n = (d[o] << 24) | (d[o + 1] << 16) | (d[o + 2] << 8) | d[o + 3]; fwrite(sc, 1, 4, f); fwrite(d + o + 4, 1, n, f); o += 4 + n; }
        free(d);
      }
      fclose(f);
    }
    VTDecompressionOutputCallbackRecord cb = {decodedCb, NULL};
    VTDecompressionSessionRef dec;
    st = VTDecompressionSessionCreate(NULL, fmt,
      (__bridge CFDictionaryRef)(getenv("VTDEC_SWDEC")
        ? @{(id)kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder: @NO}
        : @{(id)kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder: @YES}),
      (__bridge CFDictionaryRef)@{(id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange)},
      &cb, &dec);
    printf("hw decoder create %d\n", (int)st);
    if (st) return 1;
    for (id s in samples)
      VTDecompressionSessionDecodeFrame(dec, (__bridge CMSampleBufferRef)s, 0, NULL, NULL);
    VTDecompressionSessionWaitForAsynchronousFrames(dec);
    printf("decoded %d errors %d mismatches %d", decoded, decodeErrors, mismatches);
    if (sources) printf(" min psnr %.1f", psnrMin);
    printf("\n");
    return decoded == (int)samples.count && !mismatches ? 0 : 2;
  }
}
