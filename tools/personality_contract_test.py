#!/usr/bin/env python3
"""Verify the bundled TGL personalities contain the contract cloned at runtime."""

import copy
import plistlib
import sys
from pathlib import Path


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
    require(len(sys.argv) >= 2, "usage: personality_contract_test.py INFO.plist ...")
    personalities = [verify(Path(argument)) for argument in sys.argv[1:]]
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
        f"{len(personalities) * 2} negative mutations rejected"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
