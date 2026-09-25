#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <stdbool.h>
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

int main(int argc, const char *argv[]) {
	@autoreleasepool {
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
}
