#!/usr/bin/env python3
"""Validate the kext's Xcode, plist, scheme, source and CI build contract."""

import plistlib
import re
import xml.etree.ElementTree as ET
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE_DIR = ROOT / "NootedGreen"
PROJECT = ROOT / "NootedGreen.xcodeproj" / "project.pbxproj"
SCHEME = ROOT / "NootedGreen.xcodeproj" / "xcshareddata" / "xcschemes" / "NootedGreen.xcscheme"
WORKSPACE = ROOT / "NootedGreen.xcodeproj" / "project.xcworkspace" / "contents.xcworkspacedata"
WORKFLOW = ROOT / ".github" / "workflows" / "build-kext.yml"
ACCELERATION_SMOKE = ROOT / "tools" / "metal_smoke" / "main.m"


def validate_acceleration_smoke(source: str) -> None:
    compact = "".join(source.split())
    for token in (
        'strcmp(argv[1],"--media-smoke")==0',
        "constint32_twidth=1920",
        "constint32_theight=1080",
        "kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange",
        "kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder",
        "kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder",
        "kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder",
        "kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder",
        "VTCompressionSessionEncodeFrame",
        "VTCompressionSessionCompleteFrames",
        "VTDecompressionSessionDecodeFrame",
        "VTDecompressionSessionWaitForAsynchronousFrames",
        "10LL*NSEC_PER_SEC",
        "validateDecodedNV12",
        "PASS:VideoToolboxhardwareH.264encode/decodecompletedfor1920x1080NV12",
    ):
        assert token in compact, f"missing acceleration-smoke contract: {token}"
    assert source.count("VTCompressionSessionEncodeFrame(") == 1
    assert source.count("VTDecompressionSessionDecodeFrame(") == 1
    assert source.count("validateDecodedNV12(") == 2
    assert "DISPATCH_TIME_FOREVER" not in source
    assert "kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder" not in source
    assert "kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder" not in source
    order = (
        source.index('stage("media-create-hardware-encoder: begin")'),
        source.index('stage("media-hardware-encode-frame: begin")'),
        source.index('stage("media-create-hardware-decoder: begin")'),
        source.index('stage("media-hardware-decode-frame: begin")'),
        source.index("PASS: VideoToolbox hardware H.264 encode/decode"),
    )
    assert order == tuple(sorted(order)), "media encode/decode stage order"


def version_tuple(value: str):
    parts = value.split(".")
    assert parts and all(part.isdigit() for part in parts)
    return tuple(int(part) for part in parts)


def build_settings(pbx: str, object_id: str):
    pattern = re.compile(
        rf"\b{re.escape(object_id)} /\* ([^*]+) \*/ = \{{\n"
        rf"\s*isa = XCBuildConfiguration;\n"
        rf"\s*buildSettings = \{{(?P<settings>.*?)\n\s*\}};\n"
        rf"\s*name = ([^;]+);",
        re.DOTALL,
    )
    match = pattern.search(pbx)
    assert match, f"missing configuration object {object_id}"
    assert match.group(1).strip() == match.group(3).strip()
    return match.group(1).strip(), match.group("settings")


def main() -> int:
    pbx = PROJECT.read_text(encoding="utf-8")
    assert "PBXShellScriptBuildPhase" not in pbx
    assert "kern_fw.cpp" not in pbx
    assert "rm -rf" not in pbx
    assert 'productType = "com.apple.product-type.kernel-extension";' in pbx
    assert "path = MacKernelSDK/Library/x86_64/libkmod.a;" in pbx
    assert (ROOT / "MacKernelSDK" / "Library" / "x86_64" / "libkmod.a").is_file()

    sources_match = re.search(
        r"/\* Begin PBXSourcesBuildPhase section \*/(?P<body>.*?)"
        r"/\* End PBXSourcesBuildPhase section \*/",
        pbx,
        re.DOTALL,
    )
    assert sources_match
    compiled = set(re.findall(r"/\* ([^*]+) in Sources \*/", sources_match.group("body")))
    product_cpp = {path.name for path in SOURCE_DIR.glob("*.cpp")}
    assert compiled == product_cpp | {"plugin_start.cpp"}
    for name in compiled:
        assert pbx.count(f"/* {name} in Sources */") == 2

    # Every product header must be reachable from a compiled product source;
    # otherwise it is dead source rather than part of the reviewed driver.
    include_re = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.MULTILINE)
    pending = list(SOURCE_DIR.glob("*.cpp"))
    visited = set()
    while pending:
        path = pending.pop()
        path = path.resolve()
        if path in visited:
            continue
        visited.add(path)
        for include in include_re.findall(path.read_text(encoding="utf-8")):
            candidate = (path.parent / include).resolve()
            if candidate.is_file() and candidate.is_relative_to(SOURCE_DIR.resolve()):
                pending.append(candidate)
    product_headers = {path.resolve() for path in SOURCE_DIR.glob("*.hpp")}
    assert product_headers <= visited, sorted(
        str(path.relative_to(ROOT)) for path in product_headers - visited
    )

    # An installed AuxKC must remain bootable when the GPU is deliberately not
    # passed through for maintenance.  Missing hardware is not an ABI failure:
    # it disables all later private-kext processing, while real PF/VF failures
    # retain their existing fail-closed paths after driverReady publication.
    green = (SOURCE_DIR / "kern_green.cpp").read_text(encoding="utf-8")
    green_header = (SOURCE_DIR / "kern_green.hpp").read_text(encoding="utf-8")
    assert 'PANIC_COND(!this->iGPU' not in green
    assert 'SYSLOG("ngreen", "No built-in PCI GPU; NootedGreen remains inactive")' in green
    assert green.index("if (!this->iGPU)") < green.index("ngResolveKernelTelemetry(patcher)")
    assert green.index("DeviceInfo::deleter(devInfo);\n\tthis->driverReady = true;") < \
        green.index("bool NGreen::processKext")
    process_kext = green[green.index("bool NGreen::processKext"):]
    assert process_kext.index("if (!this->driverReady)") < \
        process_kext.index("gen11.processKext")
    assert "bool driverReady {false};" in green_header

    # SR-IOV PF/VF classification may read BAR0 after memory decoding is
    # enabled, but it must not leave an unclassified function capable of DMA.
    # Only a positively identified physical GPU may regain Bus Master here;
    # the VF path admits it later, after its HWS/MSI boundary.
    pci_id = green.index("this->deviceId = WIOKit::readPCIConfigValue(")
    sriov_class = green.index("NGGpuCapabilities::sriov(this->deviceId)", pci_id)
    early_bus_stop = green.index("this->iGPU->setBusMasterEnable(false);", sriov_class)
    memory_enable = green.index("this->iGPU->setMemoryEnable(true);", early_bus_stop)
    identity = green.index("const bool physicalAccess = ngPhysicalGpuAccessAllowed();", memory_enable)
    identity_bus_state = green.index(
        "this->iGPU->setBusMasterEnable(physicalAccess);", identity)
    command_readback = green.index(
        "this->iGPU->configRead16(kIOPCIConfigCommand)", identity_bus_state)
    command_check = green.index(
        "busMasterEnabled != physicalAccess", command_readback)
    config_routes = green.index("KernelPatcher::routeVirtual(this->iGPU", command_check)
    assert pci_id < sriov_class < early_bus_stop < memory_enable < identity < \
        identity_bus_state < command_readback < command_check < config_routes
    assert "setBusMasterEnable(true)" not in green
    assert "sriovCapability != NGGpuCapabilities::Sriov::Absent" in \
        green[sriov_class:memory_enable]

    target_match = re.search(
        r"isa = PBXNativeTarget;\n\s*buildConfigurationList = ([0-9A-F]+)", pbx
    )
    assert target_match
    target_list_id = target_match.group(1)
    list_match = re.search(
        rf"^\s*{target_list_id} /\* Build configuration list for PBXNativeTarget"
        rf"[^\n]* = \{{\n\s*isa = XCConfigurationList;\n"
        rf"\s*buildConfigurations = \((?P<ids>.*?)\);",
        pbx,
        re.DOTALL | re.MULTILINE,
    )
    assert list_match
    config_ids = re.findall(r"\b([0-9A-F]{24})\b", list_match.group("ids"))
    assert len(config_ids) == 3
    configs = dict(build_settings(pbx, object_id) for object_id in config_ids)
    assert set(configs) == {"Debug", "Release", "Sanitize"}
    common = (
        'CLANG_CXX_LANGUAGE_STANDARD = "c++17";',
        "INFOPLIST_FILE = NootedGreen/Info.plist;",
        "MACOSX_DEPLOYMENT_TARGET = 13.0;",
        "MODULE_VERSION = 1.0.0;",
        "MODULE_NAME = com.StezzaPilot.NootedGreen;",
        "PRODUCT_BUNDLE_IDENTIFIER = com.StezzaPilot.NootedGreen;",
        'OTHER_LDFLAGS = "-static";',
    )
    for settings in configs.values():
        for required in common:
            assert required in settings
    debug = configs["Debug"]
    for analyzer in (
        "CLANG_ANALYZER_DEADCODE_DEADSTORES",
        "CLANG_ANALYZER_DIVIDE_BY_ZERO",
        "CLANG_ANALYZER_NULL_DEREFERENCE",
    ):
        assert f"{analyzer} = YES;" in debug
    assert "GCC_GENERATE_DEBUGGING_SYMBOLS = YES;" in debug
    assert "DEPLOYMENT_POSTPROCESSING = NO;" in debug
    assert "STRIP_INSTALLED_PRODUCT = NO;" in debug
    assert "DEPLOYMENT_POSTPROCESSING = YES;" in configs["Release"]
    assert "STRIP_INSTALLED_PRODUCT = YES;" in configs["Release"]
    assert '"-fsanitize=undefined,nullability",' in configs["Sanitize"]
    assert "DEPLOYMENT_POSTPROCESSING = NO;" in configs["Sanitize"]
    assert "STRIP_INSTALLED_PRODUCT = NO;" in configs["Sanitize"]
    assert pbx.count("ARCHS = x86_64;") == 3

    info = plistlib.loads((SOURCE_DIR / "Info.plist").read_bytes())
    assert info["CFBundleIdentifier"] == "com.StezzaPilot.NootedGreen"
    assert info["CFBundleExecutable"] == "NootedGreen"
    assert info["CFBundlePackageType"] == "KEXT"
    assert info["CFBundleVersion"] == "1.0.0"
    assert info["OSBundleRequired"] == "Root"
    personality = info["IOKitPersonalities"]["com.StezzaPilot.NootedGreen"]
    assert personality == {
        "CFBundleIdentifier": "$(PRODUCT_BUNDLE_IDENTIFIER)",
        "IOClass": "$(PRODUCT_NAME:rfc1034identifier)",
        "IOMatchCategory": "$(PRODUCT_NAME:rfc1034identifier)",
        "IOProviderClass": "IOResources",
        "IOResourceMatch": "IOKit",
        "SchedulerType": 5,
    }
    required_lilu = info["OSBundleLibraries"]["as.vit9696.Lilu"]
    lilu_info = plistlib.loads((ROOT / "Lilu.kext" / "Contents" / "Info.plist").read_bytes())
    assert version_tuple(lilu_info["OSBundleCompatibleVersion"]) <= \
        version_tuple(required_lilu) <= version_tuple(lilu_info["CFBundleVersion"])

    scheme = ET.parse(SCHEME).getroot()
    buildable = scheme.find("./BuildAction/BuildActionEntries/BuildActionEntry/BuildableReference")
    assert buildable is not None
    assert buildable.attrib["BlueprintIdentifier"] == "1C748C261C21952C0024EED2"
    assert buildable.attrib["BuildableName"] == "NootedGreen.kext"
    assert buildable.attrib["ReferencedContainer"] == "container:NootedGreen.xcodeproj"
    workspace = ET.parse(WORKSPACE).getroot()
    file_ref = workspace.find("FileRef")
    assert file_ref is not None and file_ref.attrib["location"] == "self:"

    workflow = WORKFLOW.read_text(encoding="utf-8")
    acceleration_smoke = ACCELERATION_SMOKE.read_text(encoding="utf-8")
    validate_acceleration_smoke(acceleration_smoke)
    smoke_mutations = (
        acceleration_smoke.replace(
            "kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder",
            "kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder",
            1,
        ),
        acceleration_smoke.replace(
            "kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder",
            "kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder",
            1,
        ),
        acceleration_smoke.replace("const int32_t width = 1920", "const int32_t width = 1919", 1),
        acceleration_smoke.replace("10LL * NSEC_PER_SEC", "DISPATCH_TIME_FOREVER", 1),
        acceleration_smoke.replace(
            "kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder",
            "kVTCompressionPropertyKey_EncoderID",
            1,
        ),
        acceleration_smoke.replace(
            "kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder",
            "kVTDecompressionPropertyKey_PixelBufferPool",
            1,
        ),
        acceleration_smoke.replace("validateDecodedNV12", "skipDecodedNV12", 1),
    )
    for index, mutation in enumerate(smoke_mutations, 1):
        try:
            validate_acceleration_smoke(mutation)
        except (AssertionError, ValueError):
            continue
        raise AssertionError(f"escaped acceleration-smoke mutation {index}")
    assert "HookCase" not in workflow
    assert not (ROOT / "HookCase-master").exists()
    for required in (
        "codex/tahoe-sriov-vf",
        '      - "NootedGreen/**"',
        '      - "NootedGreen.xcodeproj/**"',
        '      - "tools/**"',
        "bash tools/check-static.sh",
        "-scheme NootedGreen",
        "ARCHS=x86_64",
        "Build x86_64 Metal smoke test",
        "-framework VideoToolbox",
        "-framework CoreMedia",
        "-framework CoreVideo",
    ):
        assert required in workflow

    print(
        f"PASS: {len(product_cpp)} product sources, {len(product_headers)} reachable "
        "headers, 3 target configurations, plist/scheme/workspace/CI contracts"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
