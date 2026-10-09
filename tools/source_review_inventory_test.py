#!/usr/bin/env python3
"""Fail closed when the repository's all-file review scope changes."""

from collections import Counter
import hashlib
import os
from pathlib import Path
import plistlib
import re
import shlex
import shutil
import struct
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]


def macho_nm_command():
    configured = os.environ.get("NM")
    if configured:
        result = shlex.split(configured)
        assert result, "empty NM command"
        return result
    candidates = ["llvm-nm"] + [
        f"llvm-nm-{version}" for version in range(22, 13, -1)
    ]
    candidates.append("nm")
    for candidate in candidates:
        path = shutil.which(candidate)
        if path:
            return [path]
    raise AssertionError("no nm implementation found")

# This digest covers the sorted, NUL-delimited path inventory, not file
# contents. Git commit identity already fixes contents; this independent guard
# prevents a new, removed or renamed path from silently escaping SG-11 review.
EXPECTED_PATH_DIGEST = "45eb185f5d5eb095838147e70090e5880b5b490254a8ab9963939a14bff708a0"
EXPECTED_TOP_LEVEL_COUNTS = {
    ".github": 1,
    ".gitignore": 1,
    "LICENSE": 1,
    "Lilu.kext": 40,
    "MacKernelSDK": 1227,
    "NootedGreen": 43,
    "NootedGreen.xcodeproj": 4,
    "README.md": 1,
    "Release alias": 1,
    "docs": 5,
    "sle_Internal": 109,
    "tools": 76,
}

EXPECTED_DEPENDENCY_DIGEST = (
    "ccdd62d875abf05e3cbf1624b59508350c0702eec1bc01ba63caeb72cd907c1c"
)
EXPECTED_DEPENDENCY_COUNTS = {
    "Lilu.kext": 15,
    "MacKernelSDK": 313,
    "NootedGreen": 42,
}
EXPECTED_DEPENDENCY_DIGESTS = {
    "Lilu.kext": "daace5409ae90b66d64ca8852a91ac88215f043c6e96d65cc17c43362911b561",
    "MacKernelSDK": "dd900b10e301afd4ae24ddb9871ba83e2b90fc5f090968c752da79cf04d359f0",
    "NootedGreen": "8f5a65bbdd1973f4ad044de29793c3e493a12ecbbe43ab20f96618a12b60092e",
}
EXPECTED_VENDOR_IDENTITIES = {
    "Lilu.kext/Contents/Info.plist":
        "af975c4f98c1449645cde5504bcce5935d3d30c4dddf70fe1c41e84fa0b422f7",
    "Lilu.kext/Contents/MacOS/Lilu":
        "cc1facd7a782b347b2202f56da7af0b7fe7ebea03bd176b9607c98109ecfdcbb",
    "Lilu.kext/Contents/Resources/Library/plugin_start.cpp":
        "9ebc54138b05bebc86d78479209f3a6fdafedeb5b732be190cc449b6b82e6ebf",
    "MacKernelSDK/Library/x86_64/libkmod.a":
        "7a751671bbe96c9c7b5416078df600b768c00c0dc5178d8acd8f52539c7ccd4e",
    "MacKernelSDK/Library/kmod/c_start.c":
        "2dffd2880dbe7f37df5d5cd6fb0573a603e462ef759105caf959b2cb1406fe96",
    "MacKernelSDK/Library/kmod/c_stop.c":
        "061a10dba798abc95bf5438ca302d9fbf3a996c262f9d0cbe9eadc420472ba83",
}
EXPECTED_DIRECT_VENDOR_INCLUDES = {
    "Lilu.kext/Contents/Resources/Headers/kern_api.hpp",
    "Lilu.kext/Contents/Resources/Headers/kern_devinfo.hpp",
    "Lilu.kext/Contents/Resources/Headers/kern_iokit.hpp",
    "Lilu.kext/Contents/Resources/Headers/kern_patcher.hpp",
    "Lilu.kext/Contents/Resources/Headers/kern_util.hpp",
    "Lilu.kext/Contents/Resources/Headers/plugin_start.hpp",
    "MacKernelSDK/Headers/IOKit/IOBufferMemoryDescriptor.h",
    "MacKernelSDK/Headers/IOKit/IOCatalogue.h",
    "MacKernelSDK/Headers/IOKit/IOInterruptEventSource.h",
    "MacKernelSDK/Headers/IOKit/IOLib.h",
    "MacKernelSDK/Headers/IOKit/IOLocks.h",
    "MacKernelSDK/Headers/IOKit/IOReturn.h",
    "MacKernelSDK/Headers/IOKit/IOTimerEventSource.h",
    "MacKernelSDK/Headers/IOKit/IOWorkLoop.h",
    "MacKernelSDK/Headers/IOKit/pci/IOPCIDevice.h",
    "MacKernelSDK/Headers/i386/machine_routines.h",
    "MacKernelSDK/Headers/kern/clock.h",
    "MacKernelSDK/Headers/kern/energy_perf.h",
    "MacKernelSDK/Headers/kern/sched_prim.h",
    "MacKernelSDK/Headers/kern/thread_call.h",
    "MacKernelSDK/Headers/libkern/OSAtomic.h",
    "MacKernelSDK/Headers/libkern/c++/OSSet.h",
    "MacKernelSDK/Headers/mach/vm_param.h",
    "MacKernelSDK/Headers/stddef.h",
    "MacKernelSDK/Headers/stdint.h",
    "MacKernelSDK/Headers/string.h",
}
EXPECTED_PRODUCT_SYMBOL_COUNTS = {
    "undefined": 79,
    "defined": 649,
    "external": 68,
}
EXPECTED_PRODUCT_SYMBOL_DIGESTS = {
    "undefined": "51be69ce360d74d9623b5a8a667f477b6076645caccff1e0c368be6ee91fe6d5",
    "defined": "f2795bfdc966e8774d3d6512df31b6e38894b99d6873296d399487675e2445b4",
    "external": "d9ee5d44c90fca71af6204bb6c8b8c3b8a37d56b8780a310e7a81669e42c2c63",
}
EXPECTED_EXTERNAL_ABI_PARTITIONS = {
    "Lilu": (16, "c826042ab195d1496195d180c6b4edf01160cdb5158e43e2f4a6d51eb56eee1a"),
    "kernel": (52, "bd083937eb559b618d306eb88a905a15b21e8fffcae97523019eaafcb153c64f"),
}

# These programs require an execution environment deliberately absent from the
# ordinary CI job, or are invoked/imported through another reviewed harness.
# Keeping the exact list here prevents an executable tool from silently being
# added outside the all-program ledger.
INDIRECT_OR_EXTERNAL_PROGRAMS = {
    "tools/host_vf_contained_run.sh": "source-contracted guarded host controller",
    "tools/host_vf_runtime_manifest.py": "imported by manifest mutation test",
    "tools/host_vf_runtime_preflight.sh": "source-contracted read-only preflight",
    "tools/host_vf_trace_analyzer.py": "executed by trace analyzer test and guarded host controller",
    "tools/install_tgl_media_userspace.sh": "source-contracted guest installer",
    "tools/linux_mmio_mapper/GhidraMMIOExport.py": "Ghidra-only; AST parsed offline",
    "tools/linux_mmio_mapper/batch_map_kexts.py": "imported by mapper harness",
    "tools/linux_mmio_mapper/confirm_with_linux_source.py": "imported/executed by mapper harness",
    "tools/linux_mmio_mapper/cross_reference_binary.py": "imported by mapper harness",
    "tools/linux_mmio_mapper/generate_header_from_approved.py": "imported by mapper harness",
    "tools/linux_mmio_mapper/validate_mappings.py": "imported by mapper harness",
    "tools/tahoe_ioaccel_mapping_contract_test.py": "manual pinned System/Boot KC contract",
    "tools/vf_command_pool_free_test.py": "manual pinned KC Unicorn execution",
    "tools/vf_command_pool_native_abi_test.py": "manual pinned payload Unicorn execution",
}
EXPECTED_TOOL_DATA = {
    "tools/linux_mmio_mapper/README.txt",
    "tools/linux_mmio_mapper/approved_auto_renames.json",
    "tools/linux_mmio_mapper/linux_mmio_aliases.h",
    "tools/linux_mmio_mapper/mapping.schema.json",
    "tools/linux_mmio_mapper/sample_mapping.json",
    "tools/parsed.gdt",
}

EXPECTED_PAYLOAD_BUNDLES = {
    "sle_Internal/le/AppleIntelTGLGraphics.kext": 5,
    "sle_Internal/le/AppleIntelTGLGraphicsFramebuffer.kext": 2,
    "sle_Internal/lep/AppleIntelTGLGraphicsFramebuffer.kext": 4,
    "sle_Internal/sle/AppleIntelGraphicsShared.bundle": 17,
    "sle_Internal/sle/AppleIntelTGLGraphics.kext": 5,
    "sle_Internal/sle/AppleIntelTGLGraphicsGLDriver.bundle": 6,
    "sle_Internal/sle/AppleIntelTGLGraphicsMTLDriver.bundle": 62,
    "sle_Internal/sle/AppleIntelTGLGraphicsVADriver.bundle": 4,
    "sle_Internal/sle/AppleIntelTGLGraphicsVAME.bundle": 4,
}
EXPECTED_PAYLOAD_CONTENT_DIGEST = (
    "b5319df28e74316aa3fbdc15946b79f08222fbf27410ab5280b3d2e689456a19"
)
ROUTE_REVIEWED_KERNEL_BINARIES = {
    "sle_Internal/le/AppleIntelTGLGraphics.kext/Contents/MacOS/AppleIntelTGLGraphics",
    "sle_Internal/le/AppleIntelTGLGraphicsFramebuffer.kext/Contents/MacOS/AppleIntelTGLGraphicsFramebuffer",
    "sle_Internal/lep/AppleIntelTGLGraphicsFramebuffer.kext/Contents/MacOS/AppleIntelTGLGraphicsFramebuffer",
    "sle_Internal/sle/AppleIntelTGLGraphics.kext/Contents/MacOS/AppleIntelTGLGraphics",
}
# This is the exact resource-only layout shipped by the reviewed 12.5
# AppleIntelGraphicsShared bundle: its stale Info.plist names an
# AppleIntelGraphicsSharedIL principal executable which is not present, while
# the five actual compiler dylibs are consumers.  Keep that inherited anomaly
# explicit and unique instead of weakening executable checks for every bundle.
RESOURCE_ONLY_EXECUTABLE_EXCEPTION = {
    "sle_Internal/sle/AppleIntelGraphicsShared.bundle":
        "AppleIntelGraphicsSharedIL",
}


def tracked_and_untracked_paths():
    output = subprocess.check_output(
        ["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"],
        cwd=ROOT,
    )
    paths = sorted(entry.decode("utf-8") for entry in output.split(b"\0") if entry)
    assert len(paths) == len(set(paths)), "duplicate path in Git review inventory"
    return paths


def digest_paths(paths):
    return hashlib.sha256(
        b"".join(path.encode("utf-8") + b"\0" for path in paths)
    ).hexdigest()


def compiler_dependency_paths():
    compiler = shlex.split(os.environ.get("CXX", "clang++"))
    assert compiler, "empty CXX command"
    sources = sorted((ROOT / "NootedGreen").glob("*.cpp"))
    sources.append(
        ROOT / "Lilu.kext/Contents/Resources/Library/plugin_start.cpp"
    )
    dependencies = set()
    common = [
        "--target=x86_64-apple-macos13", "-std=c++14", "-ffreestanding",
        "-fno-builtin", "-DKERNEL=1", "-DKERNEL_PRIVATE=1",
        "-DMODULE_VERSION=100", "-DPRODUCT_NAME=NootedGreen",
        "-D__MAC_OS_X_VERSION_MIN_REQUIRED=130000", "-I.",
        "-ILilu.kext/Contents/Resources", "-IMacKernelSDK/Headers",
        "-MM", "-MT", "dependency-target",
    ]
    for source in sources:
        output = subprocess.check_output(
            compiler + common + [str(source.relative_to(ROOT))],
            cwd=ROOT,
            text=True,
        )
        # No reviewed path contains Make-special whitespace. Collapse only the
        # dependency generator's line continuations, then discard its target.
        words = output.replace("\\\n", " ").split()
        assert words and words[0] == "dependency-target:"
        dependencies.update(words[1:])
    return sorted(dependencies)


def direct_vendor_includes():
    includes = set()
    pattern = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)', re.MULTILINE)
    for source in sorted((ROOT / "NootedGreen").glob("*.cpp")) + sorted(
            (ROOT / "NootedGreen").glob("*.hpp")):
        for include in pattern.findall(source.read_text(encoding="utf-8")):
            if include.startswith("kern_"):
                continue
            if include.startswith("Headers/"):
                relative = "Lilu.kext/Contents/Resources/" + include
            else:
                relative = "MacKernelSDK/Headers/" + include
            assert (ROOT / relative).is_file(), (
                f"direct vendor include does not resolve: {source}: {include}"
            )
            includes.add(relative)
    return includes


def product_symbol_surface():
    compiler = shlex.split(os.environ.get("CXX", "clang++"))
    nm = macho_nm_command()
    assert compiler, "empty CXX command"
    common = [
        "--target=x86_64-apple-macos13", "-std=c++14", "-c",
        "-ffreestanding", "-fno-builtin", "-fno-exceptions", "-fno-rtti",
        "-fno-stack-protector", "-fno-asynchronous-unwind-tables",
        "-fvisibility=hidden", "-DKERNEL=1", "-DKERNEL_PRIVATE=1",
        "-DMODULE_VERSION=100", "-DPRODUCT_NAME=NootedGreen",
        "-D__MAC_OS_X_VERSION_MIN_REQUIRED=130000", "-I.",
        "-ILilu.kext/Contents/Resources", "-IMacKernelSDK/Headers",
    ]

    def symbols(output):
        return {
            line.split()[-1] for line in output.splitlines()
            if line.split() and line.split()[-1].startswith("_")
        }

    undefined = set()
    defined = set()
    with tempfile.TemporaryDirectory(prefix="ngreen-abi-") as directory:
        for source in sorted((ROOT / "NootedGreen").glob("*.cpp")):
            relative = str(source.relative_to(ROOT))
            obj = str(Path(directory) / (source.name + ".o"))
            result = subprocess.run(
                compiler + common + [relative, "-o", obj], cwd=ROOT,
                text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            )
            assert result.returncode == 0, (
                f"product ABI compile failed: {relative}\n{result.stderr}"
            )
            unresolved = symbols(subprocess.check_output(
                nm + ["-u", obj], text=True
            ))
            globals_ = symbols(subprocess.check_output(
                nm + ["-g", obj], text=True
            ))
            undefined.update(unresolved)
            defined.update(globals_ - unresolved)
    external = undefined - defined
    surfaces = {
        "undefined": sorted(undefined),
        "defined": sorted(defined),
        "external": sorted(external),
    }
    for name, values in surfaces.items():
        actual_digest = digest_paths(values)
        assert len(values) == EXPECTED_PRODUCT_SYMBOL_COUNTS[name], (
            f"product {name} symbol count changed: {len(values)}; "
            f"digest {actual_digest}"
        )
        assert actual_digest == EXPECTED_PRODUCT_SYMBOL_DIGESTS[name], (
            f"product {name} symbol surface changed: {actual_digest}"
        )

    lilu_binary = str(ROOT / "Lilu.kext/Contents/MacOS/Lilu")
    lilu_undefined = symbols(subprocess.check_output(
        nm + ["-u", lilu_binary], text=True
    ))
    lilu_globals = symbols(subprocess.check_output(
        nm + ["-g", lilu_binary], text=True
    ))
    lilu_defined = lilu_globals - lilu_undefined
    partitions = {
        "Lilu": sorted(external & lilu_defined),
        "kernel": sorted(external - lilu_defined),
    }
    assert not set(partitions["Lilu"]) & set(partitions["kernel"])
    assert set(partitions["Lilu"]) | set(partitions["kernel"]) == external
    for name, values in partitions.items():
        expected_count, expected_digest = EXPECTED_EXTERNAL_ABI_PARTITIONS[name]
        assert len(values) == expected_count, (
            f"product {name} ABI import count changed: {len(values)}"
        )
        actual_digest = digest_paths(values)
        assert actual_digest == expected_digest, (
            f"product {name} ABI import surface changed: {actual_digest}"
        )
    return surfaces


def verify_dependency_closure():
    dependencies = compiler_dependency_paths()
    assert len(dependencies) == 370, (
        f"compiled dependency count changed: {len(dependencies)}"
    )
    assert digest_paths(dependencies) == EXPECTED_DEPENDENCY_DIGEST, (
        "compiled dependency path closure changed"
    )
    counts = Counter(path.split("/", 1)[0] for path in dependencies)
    assert counts == Counter(EXPECTED_DEPENDENCY_COUNTS), (
        f"compiled dependency partition changed: {dict(sorted(counts.items()))}"
    )
    for prefix, expected in EXPECTED_DEPENDENCY_DIGESTS.items():
        partition = [
            path for path in dependencies if path.startswith(prefix + "/")
        ]
        assert digest_paths(partition) == expected, (
            f"compiled {prefix} dependency closure changed"
        )

    product_programs = sorted(
        str(path.relative_to(ROOT))
        for suffix in ("*.cpp", "*.hpp")
        for path in (ROOT / "NootedGreen").glob(suffix)
    )
    product_dependencies = [
        path for path in dependencies if path.startswith("NootedGreen/")
    ]
    assert product_dependencies == product_programs, (
        "a product source/header is outside the real compiler closure"
    )

    includes = direct_vendor_includes()
    assert includes == EXPECTED_DIRECT_VENDOR_INCLUDES, (
        f"direct vendor ABI include surface changed: {sorted(includes)}"
    )

    for relative, expected in EXPECTED_VENDOR_IDENTITIES.items():
        actual = hashlib.sha256((ROOT / relative).read_bytes()).hexdigest()
        assert actual == expected, f"vendored identity changed: {relative}"

    ar = shlex.split(os.environ.get("AR", "ar"))
    archive = "MacKernelSDK/Library/x86_64/libkmod.a"
    members = subprocess.check_output(
        ar + ["t", archive], cwd=ROOT, text=True
    ).splitlines()
    metadata = [member for member in members if member.startswith("__.SYMDEF")]
    objects = [member for member in members if not member.startswith("__.SYMDEF")]
    assert set(metadata) <= {"__.SYMDEF", "__.SYMDEF SORTED"}, (
        f"linked libkmod archive metadata changed: {metadata}"
    )
    assert objects == ["c_start.o", "c_stop.o"], (
        f"linked libkmod object member set changed: {objects}"
    )
    symbols = product_symbol_surface()
    return dependencies, len(includes), len(symbols["external"])


def verify_tool_ledger(paths):
    program_suffixes = {".py", ".sh", ".cpp", ".m"}
    programs = {
        path for path in paths
        if path.startswith("tools/") and Path(path).suffix in program_suffixes
    }
    assert len(programs) == 70, f"tool program count changed: {len(programs)}"

    check_static = (ROOT / "tools/check-static.sh").read_text(encoding="utf-8")
    workflow = (ROOT / ".github/workflows/build-kext.yml").read_text(
        encoding="utf-8"
    )
    direct = {
        path for path in programs if path in check_static or path in workflow
    }
    indirect = set(INDIRECT_OR_EXTERNAL_PROGRAMS)
    assert not direct & indirect, "tool program is both direct and external-only"
    assert programs == direct | indirect, (
        f"unowned tool programs: {sorted(programs - direct - indirect)}"
    )

    data = {
        path for path in paths
        if path.startswith("tools/") and Path(path).suffix not in program_suffixes
    }
    assert data == EXPECTED_TOOL_DATA, (
        f"tool data/fixture ledger changed: {sorted(data)}"
    )
    return len(direct), len(indirect), len(data)


def verify_payload_ledger(paths):
    payload_paths = sorted(
        path for path in paths if path.startswith("sle_Internal/")
    )
    assert len(payload_paths) == 109, (
        f"payload path count changed: {len(payload_paths)}"
    )

    bundle_counts = Counter()
    for relative in payload_paths:
        parts = Path(relative).parts
        assert len(parts) >= 3, f"payload outside a bundle: {relative}"
        bundle_counts["/".join(parts[:3])] += 1
    assert bundle_counts == Counter(EXPECTED_PAYLOAD_BUNDLES), (
        f"payload bundle topology changed: {dict(sorted(bundle_counts.items()))}"
    )

    content_digest = hashlib.sha256()
    for relative in payload_paths:
        content_digest.update(relative.encode("utf-8") + b"\0")
        content_digest.update(hashlib.sha256((ROOT / relative).read_bytes()).digest())
    assert content_digest.hexdigest() == EXPECTED_PAYLOAD_CONTENT_DIGEST, (
        "payload content identity changed"
    )

    plist_paths = [
        relative for relative in payload_paths
        if relative.endswith(".plist") or relative.endswith("/CodeResources")
    ]
    assert len(plist_paths) == 43, (
        f"payload plist/CodeResources count changed: {len(plist_paths)}"
    )
    for relative in plist_paths:
        parsed = plistlib.loads((ROOT / relative).read_bytes())
        assert isinstance(parsed, dict), f"non-dictionary plist: {relative}"

    macho_paths = set()
    for relative in payload_paths:
        path = ROOT / relative
        if "/Contents/MacOS/" not in relative:
            continue
        header = path.read_bytes()[:8]
        assert len(header) == 8, f"short executable payload: {relative}"
        magic, cpu_type = struct.unpack("<II", header)
        assert magic == 0xFEEDFACF, f"non-64-bit Mach-O payload: {relative}"
        assert cpu_type == 0x01000007, f"non-x86_64 payload: {relative}"
        macho_paths.add(relative)
    assert len(macho_paths) == 15, (
        f"payload Mach-O count changed: {len(macho_paths)}"
    )
    assert ROUTE_REVIEWED_KERNEL_BINARIES < macho_paths, (
        "route-reviewed kernel payload partition is incomplete"
    )
    assert len(macho_paths - ROUTE_REVIEWED_KERNEL_BINARIES) == 11, (
        "opaque userspace Mach-O payload partition changed"
    )

    observed_exceptions = {}
    for bundle in sorted(EXPECTED_PAYLOAD_BUNDLES):
        info_path = ROOT / bundle / "Contents/Info.plist"
        assert info_path.is_file(), f"missing bundle Info.plist: {bundle}"
        info = plistlib.loads(info_path.read_bytes())
        expected_type = "KEXT" if bundle.endswith(".kext") else "BNDL"
        assert info.get("CFBundlePackageType") == expected_type, (
            f"wrong package type: {bundle}"
        )
        executable = info.get("CFBundleExecutable")
        assert isinstance(executable, str) and executable, (
            f"missing CFBundleExecutable: {bundle}"
        )
        executable_path = ROOT / bundle / "Contents/MacOS" / executable
        if not executable_path.is_file():
            observed_exceptions[bundle] = executable
    assert observed_exceptions == RESOURCE_ONLY_EXECUTABLE_EXCEPTION, (
        f"bundle executable anomalies changed: {observed_exceptions}"
    )
    return len(plist_paths), len(macho_paths)


def verify_product_ownership():
    product_paths = sorted((ROOT / "NootedGreen").glob("*.cpp")) + sorted(
        (ROOT / "NootedGreen").glob("*.hpp")
    )
    product_text = "\n".join(
        path.read_text(encoding="utf-8") for path in product_paths
    )
    for marker in ("TODO", "FIXME", "XXX"):
        assert marker not in product_text, f"unresolved product marker: {marker}"

    gen11_cpp = (ROOT / "NootedGreen/kern_gen11.cpp").read_text(
        encoding="utf-8"
    )
    method_names = set(re.findall(
        r"\bGen11::([A-Za-z_][A-Za-z0-9_]*)\s*\(", gen11_cpp
    ))
    assert len(method_names) == 170, (
        f"Gen11 definition inventory changed: {len(method_names)}"
    )
    for name in method_names:
        references = len(re.findall(r"\b" + re.escape(name) + r"\b", product_text))
        assert references >= 3, f"unowned Gen11 method definition: {name}"

    gen11_header = (ROOT / "NootedGreen/kern_gen11.hpp").read_text(
        encoding="utf-8"
    )
    route_fields = re.findall(
        r"\bmach_vm_address_t\s+([A-Za-z_][A-Za-z0-9_]*)\s*\{", gen11_header
    )
    assert len(route_fields) == 104, (
        f"Gen11 route/original field inventory changed: {len(route_fields)}"
    )
    for name in route_fields:
        references = len(re.findall(r"\b" + re.escape(name) + r"\b", product_text))
        assert references >= 2, f"unowned Gen11 route/original field: {name}"
    return len(method_names), len(route_fields)


def main() -> int:
    paths = tracked_and_untracked_paths()
    top_level = Counter(path.split("/", 1)[0] for path in paths)
    assert top_level == Counter(EXPECTED_TOP_LEVEL_COUNTS), (
        f"all-file review scope changed: {dict(sorted(top_level.items()))}"
    )

    digest = digest_paths(paths)
    assert digest == EXPECTED_PATH_DIGEST, (
        f"all-file review path digest changed: {digest}"
    )

    for relative in paths:
        path = ROOT / relative
        assert path.exists() or path.is_symlink(), f"missing inventoried path: {relative}"
        if path.is_symlink():
            target = path.resolve(strict=True)
            assert target.is_relative_to(ROOT), (
                f"inventoried symlink escapes repository: {relative} -> {target}"
            )

    project_programs = [
        path for path in paths
        if path.startswith(("NootedGreen/", "tools/", "NootedGreen.xcodeproj/",
                            ".github/"))
    ]
    vendored = [
        path for path in paths
        if path.startswith(("Lilu.kext/", "MacKernelSDK/"))
    ]
    payload = [path for path in paths if path.startswith("sle_Internal/")]
    dependencies, direct_includes, external_symbols = verify_dependency_closure()
    direct_tools, indirect_tools, tool_data = verify_tool_ledger(paths)
    payload_plists, payload_machos = verify_payload_ledger(paths)
    product_methods, route_fields = verify_product_ownership()
    print(
        f"PASS: exact SG-11 inventory {len(paths)} paths; "
        f"{len(project_programs)} project program/build, {len(vendored)} vendored, "
        f"{len(payload)} payload/metadata; {len(dependencies)} compiled dependencies; "
        f"ABI {direct_includes} direct includes/{external_symbols} imports; "
        f"tools {direct_tools} direct/{indirect_tools} indirect/{tool_data} data; "
        f"payload {payload_plists} plists/{payload_machos} x86_64 Mach-O; "
        f"ownership {product_methods} Gen11 methods/{route_fields} route fields"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
