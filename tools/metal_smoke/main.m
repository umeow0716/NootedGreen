#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <stdio.h>

static int fail(const char *message) {
	fprintf(stderr, "FAIL: %s\n", message);
	return 1;
}

int main(void) {
	@autoreleasepool {
		id<MTLDevice> device = MTLCreateSystemDefaultDevice();
		if (!device)
			return fail("MTLCreateSystemDefaultDevice returned nil");

		printf("device=%s\n", device.name.UTF8String);
		printf("registryID=0x%llx\n", device.registryID);

		id<MTLCommandQueue> queue = [device newCommandQueue];
		if (!queue)
			return fail("newCommandQueue returned nil");

		const NSUInteger length = 4096;
		const uint8_t expected = 0x5a;
		id<MTLBuffer> buffer =
			[device newBufferWithLength:length options:MTLResourceStorageModeShared];
		if (!buffer)
			return fail("newBufferWithLength returned nil");
		memset(buffer.contents, 0, length);

		id<MTLCommandBuffer> commandBuffer = queue.commandBuffer;
		if (!commandBuffer)
			return fail("commandBuffer returned nil");
		id<MTLBlitCommandEncoder> blit = commandBuffer.blitCommandEncoder;
		if (!blit)
			return fail("blitCommandEncoder returned nil");

		[blit fillBuffer:buffer range:NSMakeRange(0, length) value:expected];
		[blit endEncoding];
		[commandBuffer commit];
		[commandBuffer waitUntilCompleted];

		printf("status=%lu\n", (unsigned long)commandBuffer.status);
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
