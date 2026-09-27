Linux-First MMIO Mapper

Files:
- GhidraMMIOExport.py: exports bounded MMIO operands and operand-level
  read/write classifications from an analyzed Mach-O.
- cross_reference_binary.py: indexes Linux i915 headers and matches exact or
  explicitly non-exact heuristic addresses from x86_64 otool output.
- confirm_with_linux_source.py: promotes a strict REVIEW_REQUIRED record only
  when Linux source contains a platform-relevant use of the same symbol.
- batch_map_kexts.py: runs the pipeline for multiple AppleIntel kexts using a
  collision-resistant output tag derived from each relative binary path.
- mapping.schema.json: strict schema contract for model outputs.
- validate_mappings.py: validates policy and writes safe AUTO_RENAME subset.
- generate_header_from_approved.py: validates the approved subset again and
  emits a comment-safe, conflict-free Linux-name-first header.
- sample_mapping.json: example using ICL_PWR_WELL_CTL_DDI2.
- approved_auto_renames.json / linux_mmio_aliases.h: checked-in deterministic
  output for the sample.

Usage:
1) Validate and extract safe renames:
   python3 validate_mappings.py sample_mapping.json --approved-out approved_auto_renames.json

2) Non-zero exit means your model output violated policy.

3) Generate Linux-name-first header from approved mappings:
   python3 generate_header_from_approved.py approved_auto_renames.json --out linux_mmio_aliases.h

4) From the repository root, run the deterministic policy/round-trip tests:
   python3 -B tools/linux_mmio_mapper_test.py

Policy enforced:
- Linux symbol is canonical when available.
- UNKNOWN_0x... cannot be AUTO_RENAME.
- AUTO_RENAME requires exact address match + platform match + HIGH confidence + no ambiguity.
- Linux-source promotion requires an actual platform-relevant source hit; a
  pre-existing platform_match claim is not accepted as confirmation.
- Duplicate addresses/symbols and incomplete provenance are rejected before
  header generation. Generated comments escape line and terminator injection.
- Confidence must match score band:
  HIGH >= 10, MEDIUM 7..9, LOW <= 6.
