#!/usr/bin/env python3
"""Check the pinned accelerator's private GPU telemetry provider contract."""

import plistlib
import re
import subprocess
import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 4:
        raise SystemExit("usage: telemetry_contract_test.py SOURCE PLIST MACHO")

    source_path, plist_path, binary_path = map(Path, sys.argv[1:])
    source = source_path.read_text(encoding="utf-8")
    with plist_path.open("rb") as stream:
        info = plistlib.load(stream)
    libraries = info["OSBundleLibraries"]

    assert info["OSBundleRequired"] == "Root"
    assert libraries["com.StezzaPilot.NootedGreen"] == "1.0.0"
    assert "org.smichaud.HookCase" not in libraries

    undefined = subprocess.run(
        ["nm", "-u", str(binary_path)],
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    telemetry_imports = {
        match.group(1)
        for match in re.finditer(r"\b(_gpu_(?:accumulate_time|describe))\s*$", undefined, re.MULTILINE)
    }
    assert telemetry_imports == {"_gpu_accumulate_time", "_gpu_describe"}

    for symbol in telemetry_imports:
        assert f'KernelPatcher::KernelID, "{symbol}"' in source
    assert re.search(
        r'extern\s+"C"\s+EXPORT\s+uint64_t\s+gpu_accumulate_time\s*\(', source
    )
    assert re.search(r'extern\s+"C"\s+EXPORT\s+void\s+gpu_describe\s*\(', source)
    assert "kernelGpuAccumulateTime(scope, gpuId, gpuDomain, accumulatedNs, timestampNs)" in source
    assert "kernelGpuDescribe(descriptor)" in source

    print("PASS: exact Tahoe GPU telemetry ABI is forwarded by NootedGreen")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
