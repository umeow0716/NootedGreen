#!/usr/bin/env python3
"""Verify the bundled TGL personalities contain the contract cloned at runtime."""

import copy
import plistlib
import struct
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def load_personality(path: Path) -> dict:
    with path.open("rb") as stream:
        root = plistlib.load(stream)
    personalities = root.get("IOKitPersonalities")
    require(isinstance(personalities, dict), f"{path}: missing IOKitPersonalities")
    require(set(personalities) == {"Gen7"}, f"{path}: unexpected personality set")
    personality = personalities["Gen7"]
    require(isinstance(personality, dict), f"{path}: Gen7 is not a dictionary")
    return personality


def function_body(source: str, signature: str) -> str:
    start = source.find(signature)
    require(start >= 0, f"missing function: {signature}")
    brace = source.find("{", start)
    require(brace >= 0, f"missing function body: {signature}")
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[brace:index + 1]
    raise AssertionError(f"unterminated function: {signature}")


def verify_runtime_injection(source: str, label: str) -> None:
    body = function_body(
        source, "bool Gen11::injectAcceleratorPersonality(const char *bundleId)")
    required = (
        "OSDictionary::withDictionary(source)",
        "OSDictionary::withCapacity(2)",
        "OSNumber::withNumber(50ULL, 32)",
        "OSNumber::withNumber(400ULL, 32)",
        'h264Encode->setObject("VTQualityRating", qualityRating)',
        'h264Encode->setObject("VTRating", videoToolboxRating)',
        'dict->setObject("IOGVAH264EncodeCapabilities", h264Encode)',
        "OSSafeReleaseNULL(videoToolboxRating)",
        "OSSafeReleaseNULL(qualityRating)",
        "OSSafeReleaseNULL(h264Encode)",
        "if (!h264EncodeReady)",
        'OSString::withCString("AppleIntelICLGraphicsVADriver")',
        'dict->setObject("IODVDBundleName", mediaBundleName)',
        "OSSafeReleaseNULL(mediaBundleName)",
        "if (!mediaBundleReady)",
        "OSNumber::withNumber(0x1080080ULL, 32)",
        'dict->setObject("IOVARendererID", rendererId)',
        "OSSafeReleaseNULL(rendererId)",
        "if (!rendererReady)",
        "gIOCatalogue->addDrivers(array, true)",
    )
    for token in required:
        require(token in body, f"{label}: incomplete runtime H.264 injection: {token}")
    require('source->getObject("IOGVAH264EncodeCapabilities")' not in body,
            f"{label}: runtime injection still depends on an unpackaged source plist")
    ordered = (
        "OSDictionary::withDictionary(source)",
        "OSDictionary::withCapacity(2)",
        'dict->setObject("IOGVAH264EncodeCapabilities", h264Encode)',
        "if (!h264EncodeReady)",
        'dict->setObject("IODVDBundleName", mediaBundleName)',
        "if (!mediaBundleReady)",
        'dict->setObject("IOVARendererID", rendererId)',
        "if (!rendererReady)",
        "gIOCatalogue->addDrivers(array, true)",
    )
    positions = [body.index(token) for token in ordered]
    require(positions == sorted(positions),
            f"{label}: runtime H.264 clone/publish transaction is out of order")


def reject_source_mutation(source: str, old: str, new: str, label: str) -> None:
    require(source.count(old) == 1, f"mutation anchor is not unique: {label}")
    try:
        verify_runtime_injection(source.replace(old, new, 1), label)
    except AssertionError:
        return
    raise AssertionError(f"accepted runtime injection mutation: {label}")


def verify_personality(personality: dict, label: str) -> None:
    dictionary_keys = (
        "Debug",
        "Development",
        "IOAccelDisplayPipeCapabilities",
        "IOCFPlugInTypes",
        "IOGVAH264EncodeCapabilities",
        "IOGVAHEVCDecodeCapabilities",
        "IOGVAHEVCEncodeCapabilities",
    )
    for key in dictionary_keys:
        require(isinstance(personality.get(key), dict) and personality[key],
                f"{label}: missing or empty {key}")

    display = personality["IOAccelDisplayPipeCapabilities"]
    require(display.get("DisplayPipeSupported") is True,
            f"{label}: DisplayPipeSupported is not boolean true")
    require(display.get("TransactionsSupported") is True,
            f"{label}: TransactionsSupported is not boolean true")

    expected_strings = {
        "IOClass": "IntelAccelerator",
        "IOMatchCategory": "IOAccelerator",
        "IOProviderClass": "IOPCIDevice",
        "IOPCIClassMatch": "0x03000000&0xff000000",
        "MetalPluginName": "AppleIntelTGLGraphicsMTLDriver",
        "IOGLBundleName": "AppleIntelTGLGraphicsGLDriver",
        "IODVDBundleName": "AppleIntelTGLGraphicsVADriver",
        "IOGVACodec": "Gen10",
        "IOGVAScaler": "Gen10",
        "IOGVABGRAEnc": "Gen10",
    }
    for key, expected in expected_strings.items():
        require(personality.get(key) == expected,
                f"{label}: {key} does not match the admitted payload contract")

    require(personality.get("IOProbeScore") == 1000,
            f"{label}: unexpected IOProbeScore")
    require(personality.get("IOVARendererID") == 17301568,
            f"{label}: unexpected IOVARendererID")

    h264_encode = personality["IOGVAH264EncodeCapabilities"]
    require(h264_encode == {"VTQualityRating": 50, "VTRating": 400},
            f"{label}: H.264 encode capability does not match Tahoe Intel contract")

    decode = personality["IOGVAHEVCDecodeCapabilities"]
    encode = personality["IOGVAHEVCEncodeCapabilities"]
    require(decode.get("VTSupportedProfileArray") == [1, 2, 3],
            f"{label}: unexpected HEVC decode profiles")
    require(encode.get("VTSupportedProfileArray") == [1],
            f"{label}: unexpected HEVC encode profiles")


def verify_media_engine_loader_contract() -> None:
    va_driver = (
        ROOT / "sle_Internal/sle/AppleIntelTGLGraphicsVADriver.bundle/Contents/"
        "MacOS/AppleIntelTGLGraphicsVADriver"
    )
    media_engine = (
        ROOT / "sle_Internal/sle/AppleIntelTGLGraphicsVAME.bundle/Contents/"
        "MacOS/AppleIntelTGLGraphicsVAME"
    )
    va_bytes = va_driver.read_bytes()
    loader_target = (
        b"AppleIntelICLGraphicsVAME.bundle/Contents/MacOS/"
        b"AppleIntelICLGraphicsVAME"
    )
    require(va_bytes.count(loader_target) == 1,
            "TGL VA driver media-engine loader target changed")
    for prefix in (b"/Library/GPUBundles/", b"/System/Library/Extensions/"):
        require(va_bytes.count(prefix) == 1,
                f"TGL VA driver media-engine search path changed: {prefix!r}")

    me_bytes = media_engine.read_bytes()
    require(struct.unpack_from("<I", me_bytes)[0] == 0xFEEDFACF,
            "TGL media-engine payload is not a 64-bit Mach-O")
    command_count = struct.unpack_from("<I", me_bytes, 16)[0]
    offset = 32
    symbol_table = None
    for _ in range(command_count):
        command, size = struct.unpack_from("<II", me_bytes, offset)
        require(size >= 8 and offset + size <= len(me_bytes),
                "invalid TGL media-engine Mach-O load command")
        if command == 0x2:  # LC_SYMTAB
            symbol_table = struct.unpack_from("<IIII", me_bytes, offset + 8)
        offset += size
    require(symbol_table is not None,
            "TGL media-engine payload has no LC_SYMTAB")
    symoff, nsyms, stroff, strsize = symbol_table
    require(symoff + nsyms * 16 <= len(me_bytes) and
            stroff + strsize <= len(me_bytes),
            "TGL media-engine symbol table is out of bounds")
    symbols = set()
    for index in range(nsyms):
        string_index = struct.unpack_from("<I", me_bytes, symoff + index * 16)[0]
        if not 0 < string_index < strsize:
            continue
        start = stroff + string_index
        end = me_bytes.find(b"\0", start, stroff + strsize)
        require(end >= 0, "unterminated TGL media-engine symbol")
        symbols.add(me_bytes[start:end].decode("ascii"))
    for symbol in (
        "_AVD_CreateAVDAccelerator",
        "_AVD_DestroyAVDAccelerator",
    ):
        require(symbol in symbols,
                f"TGL media-engine payload does not export {symbol}")


def verify(path: Path) -> dict:
    personality = load_personality(path)
    verify_personality(personality, str(path))
    return personality


def reject_mutation(personality: dict, mutate, label: str) -> None:
    candidate = copy.deepcopy(personality)
    mutate(candidate)
    try:
        verify_personality(candidate, label)
    except AssertionError:
        return
    raise AssertionError(f"accepted personality mutation: {label}")


def main() -> int:
    require(len(sys.argv) >= 3,
            "usage: personality_contract_test.py SOURCE INFO.plist ...")
    source = Path(sys.argv[1]).read_text()
    verify_runtime_injection(source, sys.argv[1])
    verify_media_engine_loader_contract()
    for old, new, label in (
        ("OSNumber::withNumber(50ULL, 32)",
         "OSNumber::withNumber(0ULL, 32)", "wrong runtime quality rating"),
        ("OSNumber::withNumber(400ULL, 32)",
         "OSNumber::withNumber(0ULL, 32)", "wrong runtime VT rating"),
        ('dict->setObject("IOGVAH264EncodeCapabilities", h264Encode)',
         'dict->setObject("IOGVAH264EncodeCapabilitiesX", h264Encode)',
         "wrong runtime capability key"),
        ("if (!h264EncodeReady)", "if (false)",
         "removed runtime capability failure gate"),
        ('OSString::withCString("AppleIntelICLGraphicsVADriver")',
         'OSString::withCString("AppleIntelTGLGraphicsVADriver")',
         "restored pre-Tahoe TGL media userspace"),
        ('dict->setObject("IODVDBundleName", mediaBundleName)',
         'dict->setObject("IODVDBundleNameX", mediaBundleName)',
         "wrong runtime media bundle key"),
        ("if (!mediaBundleReady)", "if (false)",
         "removed runtime media bundle failure gate"),
        ("OSNumber::withNumber(0x1080080ULL, 32)",
         "OSNumber::withNumber(0x1080040ULL, 32)",
         "restored Tahoe-rejected TGL renderer slot"),
        ('dict->setObject("IOVARendererID", rendererId)',
         'dict->setObject("IOVARendererIDX", rendererId)',
         "wrong runtime renderer key"),
        ("if (!rendererReady)", "if (false)",
         "removed runtime renderer failure gate"),
    ):
        reject_source_mutation(source, old, new, label)

    personalities = [verify(Path(argument)) for argument in sys.argv[2:]]
    for personality in personalities:
        reject_mutation(
            personality,
            lambda value: value.pop("IOGVAH264EncodeCapabilities"),
            "missing H.264 encode capability")
        reject_mutation(
            personality,
            lambda value: value["IOGVAH264EncodeCapabilities"].__setitem__(
                "VTRating", 0),
            "wrong H.264 encoder rating")
    print(
        "PASS: complete native display/media personality contract in "
        f"{len(personalities)} payloads; "
        f"{len(personalities) * 2 + 10} source/runtime negative mutations rejected; "
        "runtime media clone selects exact Tahoe ICL userspace while preserving "
        "the native TGL Metal/GL payload contract"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
