#!/usr/bin/env python3
"""Offline positive and mutation tests for the runtime manifest verifier."""

from pathlib import Path
import hashlib
import importlib.util
import struct
import subprocess
import tempfile
import uuid
import zipfile


MODULE_PATH = Path(__file__).with_name("host_vf_runtime_manifest.py")
SPEC = importlib.util.spec_from_file_location("host_vf_runtime_manifest", MODULE_PATH)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def write_manifest(path: Path, values: dict[str, str]) -> None:
    path.write_text("".join(f"{key}\t{values[key]}\n" for key in sorted(values)))


def run_git(repo: Path, *args: str) -> str:
    return subprocess.run(
        ["git", "-C", str(repo), *args],
        check=True,
        stdout=subprocess.PIPE,
        text=True,
    ).stdout.strip()


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="ngreen-host-manifest-") as directory:
        root = Path(directory)
        repo = root / "repo"
        repo.mkdir()
        run_git(repo, "init", "-q")
        run_git(repo, "config", "user.name", "contract-test")
        run_git(repo, "config", "user.email", "contract-test@example.invalid")
        (repo / "tracked").write_text("pinned\n")
        run_git(repo, "add", "tracked")
        run_git(repo, "commit", "-q", "-m", "fixture")
        commit = run_git(repo, "rev-parse", "HEAD")

        image_uuid = uuid.UUID("9773b4c9-ff71-36b0-8107-910c0f318ec5")
        command = struct.pack("<II16s", 0x1B, 24, image_uuid.bytes)
        executable = struct.pack(
            "<IiiIIIII", 0xFEEDFACF, 0x01000007, 3, 11, 1, len(command), 0, 0
        ) + command
        executable_path = root / "NootedGreen"
        executable_path.write_bytes(executable)
        artifact = root / "artifact.zip"
        with zipfile.ZipFile(artifact, "w") as archive:
            archive.writestr(
                "NootedGreen.kext/Contents/MacOS/NootedGreen", executable
            )
        auxkc = root / "AuxiliaryKernelExtensions.kc"
        auxkc.write_bytes(b"auxkc-fixture")
        efi = root / "efi-backup.qcow2"
        efi.write_bytes(b"efi-fixture")
        domain_xml = root / "domain.xml"
        domain_xml.write_bytes(b"<domain/>\n")
        manifest = root / "manifest.tsv"
        values = {
            "source_commit": commit,
            "ci_run_id": "36330781623",
            "artifact_zip_path": str(artifact),
            "artifact_zip_sha256": MODULE.sha256_path(artifact),
            "kext_executable_path": str(executable_path),
            "kext_executable_sha256": sha(executable),
            "kext_uuid": str(image_uuid).upper(),
            "auxkc_path": str(auxkc),
            "auxkc_sha256": MODULE.sha256_path(auxkc),
            "efi_backup_path": str(efi),
            "efi_backup_sha256": MODULE.sha256_path(efi),
            "domain_xml_sha256": MODULE.sha256_path(domain_xml),
        }
        write_manifest(manifest, values)
        MODULE.verify(manifest, repo, domain_xml)

        original = artifact.read_bytes()
        artifact.write_bytes(original + b"mutation")
        try:
            MODULE.verify(manifest, repo, domain_xml)
        except MODULE.ManifestError:
            pass
        else:
            raise AssertionError("mutated artifact zip was accepted")
        artifact.write_bytes(original)

        with manifest.open("a") as stream:
            stream.write(f"ci_run_id\t{values['ci_run_id']}\n")
        try:
            MODULE.load_manifest(manifest)
        except MODULE.ManifestError:
            pass
        else:
            raise AssertionError("duplicate manifest key was accepted")

    print("PASS: runtime manifest hash/UUID/zip/worktree mutation contracts")


if __name__ == "__main__":
    main()
