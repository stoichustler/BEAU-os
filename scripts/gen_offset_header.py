#!/usr/bin/env python3
"""Generate an assembly header from global absolute symbols in an ELF object."""

import argparse
import os
import pathlib
import re
import tempfile

try:
    from elftools.common.exceptions import ELFError
    from elftools.elf.elffile import ELFFile
    from elftools.elf.sections import SymbolTableSection
except ImportError as error:
    raise SystemExit(
        "pyelftools is required to generate ARM64 structure offsets"
    ) from error


SYMBOL_NAME = re.compile(r"^[A-Z][A-Z0-9_]*$")


def read_symbol_contract(symbol_file):
    symbols = []
    seen = set()
    for line_number, raw_line in enumerate(symbol_file, start=1):
        symbol = raw_line.strip()
        if not symbol or symbol.startswith("#"):
            continue
        if not SYMBOL_NAME.fullmatch(symbol):
            raise ValueError(
                f"invalid manifest symbol at line {line_number}: {symbol}")
        if symbol in seen:
            raise ValueError(f"duplicate manifest symbol: {symbol}")
        seen.add(symbol)
        symbols.append(symbol)
    if not symbols:
        raise ValueError("offset symbol manifest is empty")
    return frozenset(symbols)


def absolute_symbols(input_file, contract):
    elf = ELFFile(input_file)
    symbol_tables = [
        section for section in elf.iter_sections()
        if isinstance(section, SymbolTableSection)
    ]
    if not symbol_tables:
        raise ValueError("input ELF has no symbol table")

    symbols = {}
    defined_contract_symbols = set()
    for table in symbol_tables:
        for symbol in table.iter_symbols():
            name = symbol.name
            if not name:
                continue
            section = symbol.entry["st_shndx"]
            binding = symbol.entry["st_info"]["bind"]
            if name in contract and section != "SHN_UNDEF":
                defined_contract_symbols.add(name)
            if section != "SHN_ABS" or binding != "STB_GLOBAL":
                continue
            value = symbol.entry["st_value"]
            if name in symbols:
                raise ValueError(f"duplicate absolute symbol: {name}")
            symbols[name] = value

    invalid = sorted(defined_contract_symbols - symbols.keys())
    if invalid:
        raise ValueError(f"not global absolute: {', '.join(invalid)}")
    missing = sorted(contract - symbols.keys())
    if missing:
        raise ValueError(f"missing symbols: {', '.join(missing)}")
    unexpected = sorted(symbols.keys() - contract)
    if unexpected:
        raise ValueError(f"unexpected symbols: {', '.join(unexpected)}")
    return sorted(symbols.items())


def render_header(symbols):
    lines = [
        "/* THIS FILE IS AUTO-GENERATED. DO NOT EDIT. */",
        "#ifndef ARM64_GENERATED_OFFSETS_H",
        "#define ARM64_GENERATED_OFFSETS_H",
        "",
    ]
    lines.extend(f"#define {name} 0x{value:x}" for name, value in symbols)
    lines.extend(("", "#endif /* ARM64_GENERATED_OFFSETS_H */", ""))
    return "\n".join(lines)


def generate(input_path, output_path, symbol_path):
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with symbol_path.open("r", encoding="ascii") as symbol_file:
        contract = read_symbol_contract(symbol_file)
    with input_path.open("rb") as input_file:
        content = render_header(absolute_symbols(input_file, contract))

    temporary = None
    try:
        with tempfile.NamedTemporaryFile(
                mode="w", encoding="ascii", dir=output_path.parent,
                prefix=f".{output_path.name}.", delete=False) as output_file:
            temporary = pathlib.Path(output_file.name)
            output_file.write(content)
            output_file.flush()
            os.fsync(output_file.fileno())
        os.replace(temporary, output_path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(allow_abbrev=False)
    parser.add_argument("-i", "--input", required=True, type=pathlib.Path)
    parser.add_argument("-o", "--output", required=True, type=pathlib.Path)
    parser.add_argument("-s", "--symbols", required=True, type=pathlib.Path)
    args = parser.parse_args()

    try:
        generate(args.input, args.output, args.symbols)
    except (ELFError, OSError, ValueError) as error:
        parser.exit(1, f"gen_offset_header.py: {error}\n")


if __name__ == "__main__":
    main()
