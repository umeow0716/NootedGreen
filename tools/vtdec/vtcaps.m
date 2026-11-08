// vtcaps: which codecs VideoToolbox decodes in hardware, and the hardware
// encoders it lists.
#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>

int main(void)
{
	@autoreleasepool {
		const struct { const char *name; CMVideoCodecType type; } codecs[] = {
			{"H.264", kCMVideoCodecType_H264}, {"HEVC", kCMVideoCodecType_HEVC},
			{"VP9", kCMVideoCodecType_VP9}, {"AV1", kCMVideoCodecType_AV1},
			{"JPEG", kCMVideoCodecType_JPEG}, {"MPEG-2", kCMVideoCodecType_MPEG2Video},
			{"MPEG-4", kCMVideoCodecType_MPEG4Video},
		};
		for (size_t i = 0; i < sizeof(codecs) / sizeof(*codecs); i++)
			printf("decode %-7s hw=%d\n", codecs[i].name, VTIsHardwareDecodeSupported(codecs[i].type));
		CFArrayRef list = NULL;
		VTCopyVideoEncoderList(NULL, &list);
		for (NSDictionary *e in (__bridge NSArray *)list)
			if ([e[(__bridge id)kVTVideoEncoderList_IsHardwareAccelerated] boolValue])
				printf("hw encoder %s\n", [e[(__bridge id)kVTVideoEncoderList_EncoderID] UTF8String]);
		if (list)
			CFRelease(list);
	}
	return 0;
}
