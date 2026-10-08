#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <VideoToolbox/VideoToolbox.h>

#include <dispatch/dispatch.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void stage(const char *message) {
	fprintf(stderr, "STAGE: %s\n", message);
	fflush(stderr);
}

static bool stopAfter(int argc, const char *argv[], const char *name) {
	return argc == 2 && strncmp(argv[1], "--stop-after=", 13) == 0 &&
	       strcmp(argv[1] + 13, name) == 0;
}

static int fail(const char *message) {
	fprintf(stderr, "FAIL: %s\n", message);
	return 1;
}

static int failStatus(const char *operation, OSStatus status) {
	fprintf(stderr, "FAIL: %s returned %d (0x%08x)\n", operation,
	        (int)status, (unsigned int)status);
	return 1;
}

static int metalSmoke(int argc, const char *argv[]) {
	stage("create-device: begin");
	id<MTLDevice> device = MTLCreateSystemDefaultDevice();
	if (!device)
		return fail("MTLCreateSystemDefaultDevice returned nil");
	stage("create-device: complete");

	fprintf(stderr, "device=%s\n", device.name.UTF8String);
	fprintf(stderr, "registryID=0x%llx\n", device.registryID);
	fflush(stderr);
	if (stopAfter(argc, argv, "device"))
		return 0;

	stage("create-command-queue: begin");
	id<MTLCommandQueue> queue = [device newCommandQueue];
	if (!queue)
		return fail("newCommandQueue returned nil");
	stage("create-command-queue: complete");
	if (stopAfter(argc, argv, "queue"))
		return 0;

	const NSUInteger length = 4096;
	const uint8_t expected = 0x5a;
	stage("create-buffer: begin");
	id<MTLBuffer> buffer =
		[device newBufferWithLength:length options:MTLResourceStorageModeShared];
	if (!buffer)
		return fail("newBufferWithLength returned nil");
	memset(buffer.contents, 0, length);
	stage("create-buffer: complete");
	if (stopAfter(argc, argv, "buffer"))
		return 0;

	stage("create-command-buffer: begin");
	id<MTLCommandBuffer> commandBuffer = queue.commandBuffer;
	if (!commandBuffer)
		return fail("commandBuffer returned nil");
	stage("create-command-buffer: complete");
	stage("create-blit-encoder: begin");
	id<MTLBlitCommandEncoder> blit = commandBuffer.blitCommandEncoder;
	if (!blit)
		return fail("blitCommandEncoder returned nil");
	stage("create-blit-encoder: complete");

	stage("encode-fill: begin");
	[blit fillBuffer:buffer range:NSMakeRange(0, length) value:expected];
	[blit endEncoding];
	stage("encode-fill: complete");
	if (stopAfter(argc, argv, "encode"))
		return 0;

	stage("commit: begin");
	[commandBuffer commit];
	stage("commit: complete");
	if (stopAfter(argc, argv, "commit"))
		return 0;

	stage("wait: begin");
	[commandBuffer waitUntilCompleted];
	stage("wait: complete");

	fprintf(stderr, "status=%lu\n", (unsigned long)commandBuffer.status);
	if (commandBuffer.error)
		fprintf(stderr, "Metal error: %s\n",
		        commandBuffer.error.localizedDescription.UTF8String);
	if (commandBuffer.status != MTLCommandBufferStatusCompleted ||
	    commandBuffer.error)
		return fail("Metal command buffer did not complete successfully");

	const uint8_t *bytes = buffer.contents;
	for (NSUInteger i = 0; i < length; i++) {
		if (bytes[i] != expected) {
			fprintf(stderr,
			        "FAIL: GPU result mismatch at byte %lu: got 0x%02x expected 0x%02x\n",
			        (unsigned long)i, bytes[i], expected);
			return 1;
		}
	}

	puts("PASS: Metal blit completed and all 4096 GPU-written bytes match");
	return 0;
}

struct EncodeState {
	dispatch_semaphore_t done;
	OSStatus status;
	CMSampleBufferRef sample;
	bool called;
};

struct DecodeState {
	dispatch_semaphore_t done;
	OSStatus status;
	CVImageBufferRef image;
	bool called;
};

static void compressionOutput(void *outputCallbackRefCon,
	                          void *sourceFrameRefCon,
	                          OSStatus status,
	                          VTEncodeInfoFlags infoFlags,
	                          CMSampleBufferRef sampleBuffer) {
	(void)sourceFrameRefCon;
	(void)infoFlags;
	struct EncodeState *state = outputCallbackRefCon;
	if (state->called)
		return;
	state->called = true;
	state->status = status;
	if (status == noErr && sampleBuffer &&
	    CMSampleBufferDataIsReady(sampleBuffer))
		state->sample = (CMSampleBufferRef)CFRetain(sampleBuffer);
	dispatch_semaphore_signal(state->done);
}

static void decompressionOutput(void *decompressionOutputRefCon,
	                            void *sourceFrameRefCon,
	                            OSStatus status,
	                            VTDecodeInfoFlags infoFlags,
	                            CVImageBufferRef imageBuffer,
	                            CMTime presentationTimeStamp,
	                            CMTime presentationDuration) {
	(void)sourceFrameRefCon;
	(void)infoFlags;
	(void)presentationTimeStamp;
	(void)presentationDuration;
	struct DecodeState *state = decompressionOutputRefCon;
	if (state->called)
		return;
	state->called = true;
	state->status = status;
	if (status == noErr && imageBuffer)
		state->image = (CVImageBufferRef)CFRetain(imageBuffer);
	dispatch_semaphore_signal(state->done);
}

static bool copyRequiredHardwareProperty(VTSessionRef session,
	                                     CFStringRef key,
	                                     const char *label) {
	CFTypeRef value = NULL;
	OSStatus status = VTSessionCopyProperty(
		session, key, kCFAllocatorDefault, &value);
	if (status != noErr || !value || CFGetTypeID(value) != CFBooleanGetTypeID()) {
		fprintf(stderr, "FAIL: %s property unavailable status=%d\n",
		        label, (int)status);
		if (value)
			CFRelease(value);
		return false;
	}
	bool enabled = CFBooleanGetValue((CFBooleanRef)value);
	CFRelease(value);
	fprintf(stderr, "%s=%d\n", label, enabled ? 1 : 0);
	return enabled;
}

static bool waitForCallback(dispatch_semaphore_t semaphore,
	                        const char *label) {
	long result = dispatch_semaphore_wait(
		semaphore, dispatch_time(DISPATCH_TIME_NOW, 10LL * NSEC_PER_SEC));
	if (result != 0) {
		fprintf(stderr, "FAIL: %s callback timed out\n", label);
		return false;
	}
	return true;
}

static bool fillNV12(CVPixelBufferRef pixelBuffer, uint8_t luma) {
	if (!CVPixelBufferIsPlanar(pixelBuffer) ||
	    CVPixelBufferGetPlaneCount(pixelBuffer) != 2)
		return false;
	CVReturn result = CVPixelBufferLockBaseAddress(pixelBuffer, 0);
	if (result != kCVReturnSuccess)
		return false;
	uint8_t *y = CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 0);
	uint8_t *uv = CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 1);
	size_t yStride = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 0);
	size_t uvStride = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 1);
	size_t yHeight = CVPixelBufferGetHeightOfPlane(pixelBuffer, 0);
	size_t uvHeight = CVPixelBufferGetHeightOfPlane(pixelBuffer, 1);
	if (!y || !uv || !yStride || !uvStride || !yHeight || !uvHeight) {
		CVPixelBufferUnlockBaseAddress(pixelBuffer, 0);
		return false;
	}
	for (size_t row = 0; row < yHeight; row++)
		memset(y + row * yStride, luma, yStride);
	for (size_t row = 0; row < uvHeight; row++)
		memset(uv + row * uvStride, 0x80, uvStride);
	CVPixelBufferUnlockBaseAddress(pixelBuffer, 0);
	return true;
}

static bool validateDecodedNV12(CVPixelBufferRef pixelBuffer,
	                            size_t width,
	                            size_t height,
	                            uint8_t expectedLuma) {
	if (CVPixelBufferGetWidth(pixelBuffer) != width ||
	    CVPixelBufferGetHeight(pixelBuffer) != height ||
	    CVPixelBufferGetPixelFormatType(pixelBuffer) !=
	        kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange ||
	    !CVPixelBufferIsPlanar(pixelBuffer) ||
	    CVPixelBufferGetPlaneCount(pixelBuffer) != 2)
		return false;
	if (CVPixelBufferLockBaseAddress(pixelBuffer,
	                                 kCVPixelBufferLock_ReadOnly) !=
	    kCVReturnSuccess)
		return false;
	const uint8_t *base = CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 0);
	size_t stride = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 0);
	size_t planeWidth = CVPixelBufferGetWidthOfPlane(pixelBuffer, 0);
	size_t planeHeight = CVPixelBufferGetHeightOfPlane(pixelBuffer, 0);
	uint64_t total = 0;
	if (base && stride && planeWidth == width && planeHeight == height) {
		for (size_t row = 0; row < planeHeight; row++) {
			for (size_t column = 0; column < planeWidth; column++)
				total += base[row * stride + column];
		}
	}
	CVPixelBufferUnlockBaseAddress(pixelBuffer,
	                               kCVPixelBufferLock_ReadOnly);
	if (!base || planeWidth != width || planeHeight != height)
		return false;
	double average = (double)total / (double)(planeWidth * planeHeight);
	fprintf(stderr, "decodedLumaAverage=%.3f expected=%u\n", average,
	        expectedLuma);
	return average >= (double)expectedLuma - 16.0 &&
	       average <= (double)expectedLuma + 16.0;
}

static int mediaSmoke(void) {
	const int32_t width = 1920;
	const int32_t height = 1080;
	const uint8_t expectedLuma = 0x5a;
	int result = 1;
	VTCompressionSessionRef encoder = NULL;
	VTDecompressionSessionRef decoder = NULL;
	CVPixelBufferRef source = NULL;
	struct EncodeState encodeState = {
		.done = dispatch_semaphore_create(0),
		.status = noErr,
		.sample = NULL,
		.called = false,
	};
	struct DecodeState decodeState = {
		.done = dispatch_semaphore_create(0),
		.status = noErr,
		.image = NULL,
		.called = false,
	};

	NSDictionary *encoderSpecification = @{
		(__bridge NSString *)
			kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder:
			@YES,
	};
	NSDictionary *pixelAttributes = @{
		(__bridge NSString *)kCVPixelBufferPixelFormatTypeKey:
			@(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
		(__bridge NSString *)kCVPixelBufferWidthKey: @(width),
		(__bridge NSString *)kCVPixelBufferHeightKey: @(height),
		(__bridge NSString *)kCVPixelBufferIOSurfacePropertiesKey: @{},
	};
	NSDictionary *frameProperties = @{
		(__bridge NSString *)kVTEncodeFrameOptionKey_ForceKeyFrame: @YES,
	};
	NSDictionary *decoderSpecification = @{
		(__bridge NSString *)
			kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder:
			@YES,
	};
	NSDictionary *decoderAttributes = @{
		(__bridge NSString *)kCVPixelBufferPixelFormatTypeKey:
			@(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
		(__bridge NSString *)kCVPixelBufferWidthKey: @(width),
		(__bridge NSString *)kCVPixelBufferHeightKey: @(height),
		(__bridge NSString *)kCVPixelBufferIOSurfacePropertiesKey: @{},
	};

	stage("media-create-hardware-encoder: begin");
	OSStatus status = VTCompressionSessionCreate(
		kCFAllocatorDefault, width, height, kCMVideoCodecType_H264,
		(__bridge CFDictionaryRef)encoderSpecification,
		(__bridge CFDictionaryRef)pixelAttributes, NULL,
		compressionOutput, &encodeState, &encoder);
	if (status != noErr || !encoder) {
		failStatus("VTCompressionSessionCreate", status);
		goto cleanup;
	}
	stage("media-create-hardware-encoder: complete");

	if (VTSessionSetProperty(encoder, kVTCompressionPropertyKey_RealTime,
	                         kCFBooleanTrue) != noErr ||
	    VTSessionSetProperty(encoder,
	                         kVTCompressionPropertyKey_AllowFrameReordering,
	                         kCFBooleanFalse) != noErr ||
	    VTCompressionSessionPrepareToEncodeFrames(encoder) != noErr) {
		fail("hardware encoder configuration failed");
		goto cleanup;
	}
	CVPixelBufferPoolRef pool = VTCompressionSessionGetPixelBufferPool(encoder);
	if (!pool || CVPixelBufferPoolCreatePixelBuffer(
			     kCFAllocatorDefault, pool, &source) != kCVReturnSuccess ||
	    !source || !fillNV12(source, expectedLuma)) {
		fail("NV12 encoder source allocation/fill failed");
		goto cleanup;
	}

	stage("media-hardware-encode-frame: begin");
	status = VTCompressionSessionEncodeFrame(
		encoder, source, CMTimeMake(0, 30), CMTimeMake(1, 30),
		(__bridge CFDictionaryRef)frameProperties, NULL, NULL);
	if (status != noErr) {
		failStatus("VTCompressionSessionEncodeFrame", status);
		goto cleanup;
	}
	status = VTCompressionSessionCompleteFrames(encoder, kCMTimeInvalid);
	if (status != noErr ||
	    !waitForCallback(encodeState.done, "hardware encode")) {
		if (status != noErr)
			failStatus("VTCompressionSessionCompleteFrames", status);
		goto cleanup;
	}
	if (encodeState.status != noErr || !encodeState.sample) {
		failStatus("hardware encode callback", encodeState.status);
		goto cleanup;
	}
	if (!copyRequiredHardwareProperty(
			encoder,
			kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder,
			"hardwareEncoder")) {
		fail("VideoToolbox did not use the required hardware encoder");
		goto cleanup;
	}
	CMBlockBufferRef block = CMSampleBufferGetDataBuffer(encodeState.sample);
	size_t encodedBytes = block ? CMBlockBufferGetDataLength(block) : 0;
	if (!encodedBytes) {
		fail("hardware encoder returned an empty sample");
		goto cleanup;
	}
	fprintf(stderr, "encodedBytes=%zu\n", encodedBytes);
	stage("media-hardware-encode-frame: complete");

	CMFormatDescriptionRef format =
		CMSampleBufferGetFormatDescription(encodeState.sample);
	if (!format) {
		fail("encoded sample has no format description");
		goto cleanup;
	}
	VTDecompressionOutputCallbackRecord callback = {
		.decompressionOutputCallback = decompressionOutput,
		.decompressionOutputRefCon = &decodeState,
	};

	stage("media-create-hardware-decoder: begin");
	status = VTDecompressionSessionCreate(
		kCFAllocatorDefault, format,
		(__bridge CFDictionaryRef)decoderSpecification,
		(__bridge CFDictionaryRef)decoderAttributes, &callback, &decoder);
	if (status != noErr || !decoder) {
		failStatus("VTDecompressionSessionCreate", status);
		goto cleanup;
	}
	stage("media-create-hardware-decoder: complete");

	stage("media-hardware-decode-frame: begin");
	VTDecodeInfoFlags decodeInfo = 0;
	status = VTDecompressionSessionDecodeFrame(
		decoder, encodeState.sample,
		kVTDecodeFrame_EnableAsynchronousDecompression, NULL, &decodeInfo);
	if (status != noErr) {
		failStatus("VTDecompressionSessionDecodeFrame", status);
		goto cleanup;
	}
	status = VTDecompressionSessionWaitForAsynchronousFrames(decoder);
	if (status != noErr ||
	    !waitForCallback(decodeState.done, "hardware decode")) {
		if (status != noErr)
			failStatus("VTDecompressionSessionWaitForAsynchronousFrames",
			           status);
		goto cleanup;
	}
	if (decodeState.status != noErr || !decodeState.image) {
		failStatus("hardware decode callback", decodeState.status);
		goto cleanup;
	}
	if (!copyRequiredHardwareProperty(
			decoder,
			kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder,
			"hardwareDecoder")) {
		fail("VideoToolbox did not use the required hardware decoder");
		goto cleanup;
	}
	if (!validateDecodedNV12(decodeState.image, width, height,
	                         expectedLuma)) {
		fail("decoded NV12 frame geometry/contents mismatch");
		goto cleanup;
	}
	stage("media-hardware-decode-frame: complete");
	puts("PASS: VideoToolbox hardware H.264 encode/decode completed for 1920x1080 NV12");
	result = 0;

cleanup:
	if (decoder) {
		VTDecompressionSessionInvalidate(decoder);
		CFRelease(decoder);
	}
	if (encoder) {
		VTCompressionSessionInvalidate(encoder);
		CFRelease(encoder);
	}
	if (decodeState.image)
		CFRelease(decodeState.image);
	if (encodeState.sample)
		CFRelease(encodeState.sample);
	if (source)
		CFRelease(source);
	return result;
}

int main(int argc, const char *argv[]) {
	@autoreleasepool {
		if (argc == 2 && strcmp(argv[1], "--media-smoke") == 0)
			return mediaSmoke();
		if (argc > 2 ||
		    (argc == 2 && strncmp(argv[1], "--stop-after=", 13) != 0))
			return fail("usage: metal-smoke [--stop-after=stage|--media-smoke]");
		return metalSmoke(argc, argv);
	}
}
