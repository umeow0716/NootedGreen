#!/usr/bin/env python3
"""Verify every immutable input to a contained host VF runtime test."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import re
import struct
import subprocess
import zipfile


REQUIRED_KEYS = {
    "source_commit",
    "ci_run_id",
    "artifact_zip_path",
    "artifact_zip_sha256",
    "kext_executable_path",
    "kext_executable_sha256",
    "kext_uuid",
    "auxkc_path",
    "auxkc_sha256",
    "efi_backup_path",
    "efi_backup_sha256",
    "domain_xml_sha256",
}
PATH_KEYS = {
    "artifact_zip_path",
    "kext_executable_path",
    "auxkc_path",
    "efi_backup_path",
}
SHA_KEYS = {key for key in REQUIRED_KEYS if key.endswith("_sha256")}
HEX64 = re.compile(r"[0-9a-f]{64}")
COMMIT = re.compile(r"[0-9a-f]{40}")
UUID_TEXT = re.compile(
    r"[0-9A-F]{8}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{12}"
)


class ManifestError(ValueError):
    pass


def sha256_path(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def macho_uuid(data: bytes) -> str:
    header_size = struct.calcsize("<IiiIIIII")
    if len(data) < header_size:
        raise ManifestError("Mach-O header is truncated")
    magic, _, _, _, command_count, command_bytes, _, _ = struct.unpack_from(
        "<IiiIIIII", data
    )
    if magic != 0xFEEDFACF:
        raise ManifestError("kext executable is not a little-endian Mach-O 64 image")
    if command_count > 65536 or command_bytes > len(data) - header_size:
        raise ManifestError("Mach-O load-command table is invalid")
    cursor = header_size
    end = header_size + command_bytes
    uuids: list[str] = []
    for _ in range(command_count):
        if cursor + 8 > end:
            raise ManifestError("Mach-O load command is truncated")
        command, size = struct.unpack_from("<II", data, cursor)
        if size < 8 or cursor + size > end:
            raise ManifestError("Mach-O load-command size is invalid")
        if command == 0x1B:
            if size != 24:
                raise ManifestError("LC_UUID has an invalid size")
            raw = data[cursor + 8 : cursor + 24]
            text = raw.hex().upper()
            uuids.append(
                f"{text[0:8]}-{text[8:12]}-{text[12:16]}-"
                f"{text[16:20]}-{text[20:32]}"
            )
        cursor += size
    if cursor != end or len(uuids) != 1:
        raise ManifestError("Mach-O must contain exactly one LC_UUID")
    return uuids[0]


def load_manifest(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for number, raw_line in enumerate(path.read_text().splitlines(), 1):
        if not raw_line or raw_line.startswith("#"):
            continue
        fields = raw_line.split("\t")
        if len(fields) != 2 or not fields[0] or not fields[1]:
            raise ManifestError(f"manifest line {number} is not key<TAB>value")
        key, value = fields
        if key not in REQUIRED_KEYS:
            raise ManifestError(f"manifest line {number} has unknown key {key!r}")
        if key in values:
            raise ManifestError(f"manifest line {number} duplicates {key!r}")
        values[key] = value
    missing = REQUIRED_KEYS - values.keys()
    if missing:
        raise ManifestError(f"manifest is missing: {', '.join(sorted(missing))}")
    return values


def checked_path(text: str, label: str) -> Path:
    path = Path(text)
    if not path.is_absolute() or path.is_symlink() or not path.is_file():
        raise ManifestError(f"{label} must be an absolute, regular, non-symlink file")
    return path


def git_output(repo: Path, *args: str) -> str:
    result = subprocess.run(
        ["git", "-c", f"safe.directory={repo}", "-C", str(repo), *args],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    return result.stdout.strip()


def verify(manifest_path: Path, repo: Path, domain_xml: Path) -> dict[str, str]:
    values = load_manifest(manifest_path)
    if not COMMIT.fullmatch(values["source_commit"]):
        raise ManifestError("source_commit must be a full lowercase Git object ID")
    if not values["ci_run_id"].isdigit():
        raise ManifestError("ci_run_id must be decimal")
    for key in SHA_KEYS:
        if not HEX64.fullmatch(values[key]):
            raise ManifestError(f"{key} must be a lowercase SHA-256")
    if not UUID_TEXT.fullmatch(values["kext_uuid"]):
        raise ManifestError("kext_uuid must use uppercase canonical form")

    repo = repo.resolve(strict=True)
    if git_output(repo, "rev-parse", "HEAD") != values["source_commit"]:
        raise ManifestError("repository HEAD does not match source_commit")
    if git_output(repo, "status", "--porcelain=v1"):
        raise ManifestError("repository worktree is not clean")

    paths = {key: checked_path(values[key], key) for key in PATH_KEYS}
    domain_xml = checked_path(str(domain_xml), "domain XML snapshot")
    for key, path_key in (
        ("artifact_zip_sha256", "artifact_zip_path"),
        ("kext_executable_sha256", "kext_executable_path"),
        ("auxkc_sha256", "auxkc_path"),
        ("efi_backup_sha256", "efi_backup_path"),
    ):
        actual = sha256_path(paths[path_key])
        if actual != values[key]:
            raise ManifestError(f"{path_key} SHA-256 mismatch: {actual}")
    actual_xml = sha256_path(domain_xml)
    if actual_xml != values["domain_xml_sha256"]:
        raise ManifestError(f"inactive domain XML SHA-256 mismatch: {actual_xml}")

    executable = paths["kext_executable_path"].read_bytes()
    if macho_uuid(executable) != values["kext_uuid"]:
        raise ManifestError("extracted kext LC_UUID does not match manifest")
    with zipfile.ZipFile(paths["artifact_zip_path"]) as archive:
        candidates = [
            name
            for name in archive.namelist()
            if name.endswith("NootedGreen.kext/Contents/MacOS/NootedGreen")
            and not name.endswith("/")
        ]
        if len(candidates) != 1:
            raise ManifestError("CI zip must contain exactly one NootedGreen executable")
        archived = archive.read(candidates[0])
    if sha256_bytes(archived) != values["kext_executable_sha256"]:
        raise ManifestError("CI zip kext executable SHA-256 does not match manifest")
    if macho_uuid(archived) != values["kext_uuid"]:
        raise ManifestError("CI zip kext LC_UUID does not match manifest")
    return values


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("manifest", type=Path)
    parser.add_argument("repo", type=Path)
    parser.add_argument("domain_xml", type=Path)
    args = parser.parse_args()
    try:
        values = verify(args.manifest, args.repo, args.domain_xml)
    except (ManifestError, OSError, subprocess.CalledProcessError, zipfile.BadZipFile) as error:
        raise SystemExit(f"FAIL: runtime manifest verification: {error}") from error
    print(
        "PASS: immutable runtime manifest "
        f"commit={values['source_commit']} ci_run={values['ci_run_id']} "
        f"kext_uuid={values['kext_uuid']}"
    )


if __name__ == "__main__":
    main()
