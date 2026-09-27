#!/usr/bin/env python3
import argparse
import json
import re
from pathlib import Path

HEX_RE = re.compile(r"^0x[0-9a-fA-F]+$")
SYMBOL_RE = re.compile(r"^[A-Z][A-Z0-9_]*$")
PLATFORM_RE = re.compile(r"^[A-Z][A-Z0-9_]*$")
REQUIRED_APPROVED = {
    "address", "platform", "canonical_linux_symbol", "apple_aliases", "source"
}
REQUIRED_SOURCE = {"repo", "path", "line", "macro_text"}


def comment_text(value):
    """Return one safe C-comment line without changing identifier semantics."""
    return str(value).replace("\r", " ").replace("\n", " ").replace("*/", "* /")


def sanitize_aliases(aliases):
    if not isinstance(aliases, list):
        return []
    out = []
    for a in aliases:
        if isinstance(a, str) and a.strip():
            out.append(a.strip())
    return out


def parse_int_hex(addr):
    if not isinstance(addr, str) or not HEX_RE.match(addr):
        raise ValueError(f"Invalid address format: {addr}")
    return int(addr, 16)


def sort_records(records):
    return sorted(records, key=lambda r: parse_int_hex(r["address"]))


def load_approved(path):
    doc = json.loads(Path(path).read_text(encoding="utf-8"))
    if not isinstance(doc, dict):
        raise ValueError("Top-level JSON must be an object")

    platform = doc.get("platform")
    approved = doc.get("approved")
    approved_count = doc.get("approved_count")

    if not isinstance(platform, str) or not PLATFORM_RE.fullmatch(platform):
        raise ValueError("Missing or invalid platform")
    if not isinstance(approved, list):
        raise ValueError("Missing or invalid approved array")
    if not isinstance(approved_count, int) or approved_count != len(approved):
        raise ValueError("approved_count does not match approved array")

    filtered = []
    seen_addresses = set()
    seen_symbols = set()
    for i, r in enumerate(approved):
        if not isinstance(r, dict):
            raise ValueError(f"approved[{i}] must be an object")
        if set(r) != REQUIRED_APPROVED:
            raise ValueError(f"approved[{i}] has missing or unexpected keys")

        address = r.get("address")
        symbol = r.get("canonical_linux_symbol")
        record_platform = r.get("platform")
        aliases = r.get("apple_aliases")
        source = r.get("source")

        if not isinstance(address, str) or not HEX_RE.match(address):
            raise ValueError(f"approved[{i}] has invalid address: {address}")
        if not isinstance(symbol, str) or not SYMBOL_RE.match(symbol):
            raise ValueError(
                f"approved[{i}] has invalid canonical_linux_symbol: {symbol}"
            )
        if record_platform != platform:
            raise ValueError(f"approved[{i}] platform differs from top-level platform")
        if not isinstance(aliases, list) or any(
            not isinstance(alias, str) or not alias.strip() for alias in aliases
        ):
            raise ValueError(f"approved[{i}] has invalid apple_aliases")
        if len(set(aliases)) != len(aliases):
            raise ValueError(f"approved[{i}] has duplicate apple_aliases")
        if not isinstance(source, dict) or set(source) != REQUIRED_SOURCE:
            raise ValueError(f"approved[{i}] has invalid source object")
        if any(not isinstance(source[key], str) for key in ("repo", "path", "macro_text")):
            raise ValueError(f"approved[{i}] has non-string source provenance")
        if not all(source[key].strip() for key in ("repo", "path", "macro_text")):
            raise ValueError(f"approved[{i}] has incomplete source provenance")
        if not isinstance(source["line"], int) or source["line"] < 1:
            raise ValueError(f"approved[{i}] has invalid source line")

        normalized_address = address.lower()
        if normalized_address in seen_addresses:
            raise ValueError(f"approved[{i}] duplicates address {address}")
        if symbol in seen_symbols:
            raise ValueError(f"approved[{i}] duplicates symbol {symbol}")
        seen_addresses.add(normalized_address)
        seen_symbols.add(symbol)

        filtered.append(
            {
                "address": normalized_address,
                "symbol": symbol,
                "platform": record_platform,
                "aliases": sanitize_aliases(aliases),
                "source": source,
            }
        )

    return platform, sort_records(filtered)


def build_header(platform, records, in_name):
    guard = f"LINUX_MMIO_ALIASES_{platform.upper()}_H"
    lines = []
    lines.append("/* Auto-generated. Do not edit by hand. */")
    lines.append(f"/* Source input: {in_name} */")
    lines.append(f"/* Platform: {platform} */")
    lines.append("")
    lines.append(f"#ifndef {guard}")
    lines.append(f"#define {guard}")
    lines.append("")
    lines.append("/*")
    lines.append(" * Linux canonical register names mapped from approved address matches.")
    lines.append(" * This keeps symbols grep-compatible with i915 code and headers.")
    lines.append(" */")
    lines.append("")

    seen_symbol = {}
    seen_addr = {}

    for r in records:
        symbol = r["symbol"]
        addr = r["address"]

        aliases = ", ".join(comment_text(alias) for alias in r["aliases"]) if r["aliases"] else "none"
        src_path = comment_text(r.get("source", {}).get("path", ""))
        src_line = r.get("source", {}).get("line", "")

        note = f"/* addr {addr}; apple aliases: {aliases}"
        if src_path and isinstance(src_line, int):
            note += f"; src {src_path}:{src_line}"
        note += " */"

        if symbol in seen_symbol or addr in seen_addr:
            raise ValueError("duplicate symbol/address reached header generation")

        seen_symbol[symbol] = addr
        seen_addr[addr] = symbol

        lines.append(note)
        lines.append(f"#define {symbol} 0x{int(addr, 16):x}")
        lines.append(f"#define {symbol}__MMIO _MMIO({symbol})")
        lines.append("")

    lines.append(f"#endif /* {guard} */")
    lines.append("")
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(
        description="Generate Linux-name-first MMIO header from approved mappings"
    )
    parser.add_argument(
        "input",
        help="Path to approved_auto_renames.json generated by validate_mappings.py",
    )
    parser.add_argument(
        "--out",
        default="linux_mmio_aliases.h",
        help="Output header path",
    )
    args = parser.parse_args()

    in_path = Path(args.input)
    if not in_path.exists():
        raise SystemExit(f"ERROR: input file not found: {in_path}")

    platform, records = load_approved(in_path)
    content = build_header(platform, records, in_path.name)
    Path(args.out).write_text(content, encoding="utf-8")
    print(f"Wrote {args.out} with {len(records)} approved mapping(s)")


if __name__ == "__main__":
    main()
