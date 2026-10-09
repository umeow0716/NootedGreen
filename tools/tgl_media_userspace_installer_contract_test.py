#!/usr/bin/env python3
"""Fail-closed source contract for the pinned Tahoe TGL media installer."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "tools/install_tgl_media_userspace.sh"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def verify(source: str) -> None:
    required = (
        "source_va=/Library/Extensions/AppleIntelTGLGraphicsVADriver.bundle",
        "source_vame=/Library/Extensions/AppleIntelTGLGraphicsVAME.bundle",
        "gpu_root=/Library/GPUBundles",
        "target_va=$gpu_root/AppleIntelTGLGraphicsVADriver.bundle",
        "target_vame=$gpu_root/AppleIntelICLGraphicsVAME.bundle",
        "AppleIntelICLGraphicsVAME",
        "expected_va_exec=d265f2038135c2c5c3649b621d9aea48cf1cb74a1fb36069bb1b3562b3750129",
        "expected_vame_exec=677d73e17af59813f3254da7a98b81392c61e7f0811e3eb21ea93adf443f3230",
        "[[ $count == 4 ]]",
        "[[ $count == 5 ]]",
        "/usr/bin/codesign --verify --ignore-resources --verbose=4",
        "/usr/bin/ditto \"$source_va\" \"$stage_va\"",
        "/usr/sbin/chown -R root:wheel \"$stage_va\" \"$stage_vame\"",
        "/usr/bin/cmp -s",
        "/bin/mv \"$stage_va\" \"$target_va\"",
        "/bin/mv \"$stage_vame\" \"$target_vame\"",
        "/bin/sync",
        "NGRN_TGL_MEDIA_USERSPACE_READY",
    )
    for needle in required:
        require(needle in source, f"missing installer invariant: {needle}")
    forbidden = ("curl ", "wget ", "kmutil", "kextload", "virsh", "sudo ")
    for needle in forbidden:
        require(needle not in source, f"forbidden installer action: {needle}")


def main() -> int:
    source = SCRIPT.read_text()
    verify(source)
    mutations = (
        ("/Library/GPUBundles", "/Library/Extensions"),
        ("AppleIntelTGLGraphicsVADriver.bundle", "AppleIntelICLGraphicsVADriver.bundle"),
        ("AppleIntelICLGraphicsVAME", "AppleIntelTGLGraphicsVAME"),
        ("d265f2038135c2c5c3649b621d9aea48cf1cb74a1fb36069bb1b3562b3750129", "0" * 64),
        ("677d73e17af59813f3254da7a98b81392c61e7f0811e3eb21ea93adf443f3230", "0" * 64),
        ("/usr/bin/codesign --verify --ignore-resources --verbose=4", "true"),
        ("/usr/bin/ditto \"$source_va\" \"$stage_va\"", "true"),
        ("/usr/bin/cmp -s", "/bin/cmp -s"),
        ("/bin/mv \"$stage_va\" \"$target_va\"", "true"),
        ("/bin/sync", "/usr/bin/true"),
    )
    rejected = 0
    for old, new in mutations:
        require(old in source, f"mutation anchor absent: {old}")
        mutant = source.replace(old, new)
        try:
            verify(mutant)
        except AssertionError:
            rejected += 1
        else:
            raise AssertionError(f"installer mutation survived: {old} -> {new}")
    print(
        "PASS: exact Tahoe TGL VADriver GPUBundles mirror and complete ICL "
        f"VAME alias contract; {rejected} negative mutations rejected"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
