#!/usr/bin/env python3
"""Offline contracts for every host-side Linux MMIO mapper stage."""

import ast
import copy
import importlib.util
import json
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MAPPER = ROOT / "tools" / "linux_mmio_mapper"
sys.dont_write_bytecode = True


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


validate = load_module("ng_validate_mappings", MAPPER / "validate_mappings.py")
generate = load_module("ng_generate_header", MAPPER / "generate_header_from_approved.py")
cross = load_module("ng_cross_reference", MAPPER / "cross_reference_binary.py")
batch = load_module("ng_batch_mapper", MAPPER / "batch_map_kexts.py")
confirm = load_module("ng_confirm_linux", MAPPER / "confirm_with_linux_source.py")


def expect_value_error(callback) -> None:
    try:
        callback()
    except ValueError:
        return
    raise AssertionError("expected ValueError")


def main() -> int:
    # Parse every tracked Python utility without importing Ghidra-only modules
    # or creating __pycache__ inside the worktree.
    python_files = sorted((ROOT / "tools").rglob("*.py"))
    for path in python_files:
        ast.parse(path.read_text(encoding="utf-8"), filename=str(path))

    sample = json.loads((MAPPER / "sample_mapping.json").read_text(encoding="utf-8"))
    schema = json.loads((MAPPER / "mapping.schema.json").read_text(encoding="utf-8"))
    assert schema["properties"]["platform"]["pattern"] == "^[A-Z][A-Z0-9_]*$"
    errors, _warnings = validate.validate_document(sample)
    assert errors == []
    approved = {
        "platform": sample["platform"],
        "approved_count": 1,
        "approved": validate.extract_auto_renames(sample),
    }
    checked_approved = json.loads(
        (MAPPER / "approved_auto_renames.json").read_text(encoding="utf-8")
    )
    assert approved == checked_approved

    platform, records = generate.load_approved(
        MAPPER / "approved_auto_renames.json"
    )
    header = generate.build_header(
        platform, records, "approved_auto_renames.json"
    )
    assert header == (MAPPER / "linux_mmio_aliases.h").read_text(encoding="utf-8")

    duplicate_address = copy.deepcopy(sample)
    duplicate_address["results"].append(copy.deepcopy(sample["results"][0]))
    assert any(
        "address duplicates" in error
        for error in validate.validate_document(duplicate_address)[0]
    )
    bad_platform = copy.deepcopy(sample)
    bad_platform["platform"] = "ICL*/INJECT"
    assert validate.validate_document(bad_platform)[0]

    conflicting_approved = copy.deepcopy(checked_approved)
    conflicting_approved["approved"].append(
        copy.deepcopy(conflicting_approved["approved"][0])
    )
    conflicting_approved["approved_count"] = 2
    with tempfile.TemporaryDirectory(prefix="ngreen-mmio-") as temp_dir:
        temp = Path(temp_dir)
        conflict_path = temp / "conflict.json"
        conflict_path.write_text(json.dumps(conflicting_approved), encoding="utf-8")
        expect_value_error(lambda: generate.load_approved(conflict_path))

        injected = copy.deepcopy(checked_approved)
        injected["approved"][0]["apple_aliases"] = ["alias */\n#define BAD 1"]
        injected_path = temp / "injected.json"
        injected_path.write_text(json.dumps(injected), encoding="utf-8")
        injected_platform, injected_records = generate.load_approved(injected_path)
        safe_header = generate.build_header(
            injected_platform, injected_records, injected_path.name
        )
        assert "*/\n#define BAD" not in safe_header
        assert "* / #define BAD" in safe_header

        # Existing platform_match evidence must not substitute for an actual
        # platform-relevant Linux source hit during promotion.
        review = copy.deepcopy(sample)
        record = review["results"][0]
        record["canonical_linux_symbol"] = "TEST_CTL"
        record["status"] = "REVIEW_REQUIRED"
        review["results"] = [record]
        review_path = temp / "review.json"
        output_path = temp / "confirmed.json"
        report_path = temp / "report.json"
        source_root = temp / "linux"
        source_root.mkdir()
        (source_root / "generic.c").write_text(
            "unsigned int value = TEST_CTL;\n", encoding="utf-8"
        )
        review_path.write_text(json.dumps(review), encoding="utf-8")
        command = [
            sys.executable,
            str(MAPPER / "confirm_with_linux_source.py"),
            "--input", str(review_path),
            "--source-root", str(source_root),
            "--out", str(output_path),
            "--report-out", str(report_path),
        ]
        subprocess.run(command, check=True, capture_output=True, text=True)
        unconfirmed = json.loads(output_path.read_text(encoding="utf-8"))
        assert unconfirmed["results"][0]["status"] == "REVIEW_REQUIRED"

        (source_root / "intel_tgl.c").write_text(
            "unsigned int tgl_value = TEST_CTL;\n", encoding="utf-8"
        )
        subprocess.run(command, check=True, capture_output=True, text=True)
        confirmed = json.loads(output_path.read_text(encoding="utf-8"))
        assert confirmed["results"][0]["status"] == "AUTO_RENAME"

        invalid_limit = subprocess.run(
            command + ["--max-hits-per-symbol", "0"],
            capture_output=True,
            text=True,
        )
        assert invalid_limit.returncode == 2

    # Duplicate provenance for the same Linux symbol is not ambiguity; two
    # distinct, equally scored symbols at one address are.
    candidate = {
        "symbol": "TGL_TEST_CTL",
        "path": "display/intel_display_reg.h",
        "line": 10,
        "macro_text": "#define TGL_TEST_CTL _MMIO(0x45454)",
    }
    one_symbol = cross.build_mapping(
        {0x45454}, {0x45454: [candidate, copy.deepcopy(candidate)]}, "TGL"
    )
    assert one_symbol["results"][0]["status"] == "AUTO_RENAME"
    alternative = copy.deepcopy(candidate)
    alternative["symbol"] = "TGL_OTHER_CTL"
    ambiguous = cross.build_mapping(
        {0x45454}, {0x45454: [candidate, alternative]}, "TGL"
    )
    assert ambiguous["results"][0]["status"] == "REVIEW_REQUIRED"
    assert "TGL_OTHER_CTL" in ambiguous["results"][0]["evidence"]["ambiguity_notes"]

    assert batch.unique_run_tag(Path("a b/Foo.kext/Contents/MacOS/Foo")) != \
        batch.unique_run_tag(Path("a_b/Foo.kext/Contents/MacOS/Foo"))
    assert confirm.is_platform_relevant("drivers/intel_tgl.c", "TEST_CTL", "TGL")
    assert not confirm.is_platform_relevant("drivers/generic.c", "TEST_CTL", "TGL")

    print(
        f"PASS: {len(python_files)} Python utilities parsed; mapper validation, "
        "promotion, ambiguity, provenance and header-safety contracts"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
