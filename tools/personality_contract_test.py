#!/usr/bin/env python3
"""Verify the bundled TGL personalities contain the contract cloned at runtime."""

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


def verify(path: Path) -> None:
    personality = load_personality(path)
    dictionary_keys = (
        "Debug",
        "Development",
        "IOAccelDisplayPipeCapabilities",
        "IOCFPlugInTypes",
        "IOGVAHEVCDecodeCapabilities",
        "IOGVAHEVCEncodeCapabilities",
    )
    for key in dictionary_keys:
        require(isinstance(personality.get(key), dict) and personality[key],
                f"{path}: missing or empty {key}")

    display = personality["IOAccelDisplayPipeCapabilities"]
    require(display.get("DisplayPipeSupported") is True,
            f"{path}: DisplayPipeSupported is not boolean true")
    require(display.get("TransactionsSupported") is True,
            f"{path}: TransactionsSupported is not boolean true")

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
                f"{path}: {key} does not match the admitted payload contract")

    require(personality.get("IOProbeScore") == 1000,
            f"{path}: unexpected IOProbeScore")
    require(personality.get("IOVARendererID") == 17301568,
            f"{path}: unexpected IOVARendererID")

    decode = personality["IOGVAHEVCDecodeCapabilities"]
    encode = personality["IOGVAHEVCEncodeCapabilities"]
    require(decode.get("VTSupportedProfileArray") == [1, 2, 3],
            f"{path}: unexpected HEVC decode profiles")
    require(encode.get("VTSupportedProfileArray") == [1],
            f"{path}: unexpected HEVC encode profiles")


def main() -> int:
    require(len(sys.argv) >= 2, "usage: personality_contract_test.py INFO.plist ...")
    for argument in sys.argv[1:]:
        verify(Path(argument))
    print(f"PASS: complete native display/media personality contract in {len(sys.argv) - 1} payloads")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
