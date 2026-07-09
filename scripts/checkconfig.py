#!/usr/bin/env python3
#
# BEAU HYPERVISOR 2026
#
# Copyright (C) 2026 Hustler Lo.
# SPDX-License-Identifier: BSD-3-Clause
#

"""Validate BEAU platform Bconfig files against the Kconfig tree."""

import argparse
import ast
import re
import sys
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR / "Kconfiglib"))

import kconfiglib  # noqa: E402


ASSIGN_RE = re.compile(r"^\s*CONFIG_([A-Za-z0-9_]+)\s*=\s*(.*?)\s*$")
UNSET_RE = re.compile(r"^\s*#\s*CONFIG_([A-Za-z0-9_]+)\s+is not set\s*$")

PLATFORM_SYMBOLS = {
    "qemu": ("PLATFORM_QEMU", "STATIC_QEMU_PLATFORM"),
    "rk356x": ("PLATFORM_RK356X", "STATIC_RK356X_PLATFORM"),
}


def parse_bconfig(path):
    symbols = {}
    duplicates = []

    for lineno, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        match = ASSIGN_RE.match(line)
        value = None

        if match:
            name = match.group(1)
            value = match.group(2)
        else:
            match = UNSET_RE.match(line)
            if match:
                name = match.group(1)

        if match:
            if name in symbols:
                duplicates.append((name, lineno, symbols[name][1]))
            symbols[name] = (value, lineno)

    return symbols, duplicates


def unquote_string(value):
    if (len(value) >= 2) and (value[0] == value[-1] == '"'):
        try:
            parsed = ast.literal_eval(value)
            if isinstance(parsed, str):
                return parsed
        except (SyntaxError, ValueError):
            pass
    return value


def expected_value(sym, raw_value):
    if raw_value is None:
        return "n"
    if sym.orig_type == kconfiglib.STRING:
        return unquote_string(raw_value)
    return raw_value


def defined_symbol_names(kconf):
    return {sym.name for sym in kconf.unique_defined_syms if sym.name is not None}


def check_value_took(kconf, path, assignments, errors):
    for name, (raw_value, lineno) in assignments.items():
        sym = kconf.syms[name]
        expected = expected_value(sym, raw_value)

        if sym.str_value != expected:
            errors.append(
                f"{path}:{lineno}: CONFIG_{name} requested {expected!r}, "
                f"but Kconfig resolved {sym.str_value!r}"
            )


def check_platform(kconf, path, errors):
    platform = path.parent.name

    if platform not in PLATFORM_SYMBOLS:
        errors.append(f"{path}: unsupported platform directory '{platform}'")
        return

    board = kconf.syms["BOARD"].str_value
    scenario = kconf.syms["SCENARIO"].str_value
    if board != platform:
        errors.append(f"{path}: CONFIG_BOARD is {board!r}, expected {platform!r}")
    if scenario != platform:
        errors.append(f"{path}: CONFIG_SCENARIO is {scenario!r}, expected {platform!r}")

    expected_y = set(PLATFORM_SYMBOLS[platform])
    for symbols in PLATFORM_SYMBOLS.values():
        for name in symbols:
            expected = "y" if name in expected_y else "n"
            actual = kconf.syms[name].str_value
            if actual != expected:
                errors.append(
                    f"{path}: CONFIG_{name} is {actual!r}, expected {expected!r}"
                )


def check_bconfig(kconfig, path):
    kconf = kconfiglib.Kconfig(str(kconfig), suppress_traceback=True)
    known = defined_symbol_names(kconf)
    assignments, duplicates = parse_bconfig(path)
    errors = []

    for name, lineno, first_lineno in duplicates:
        errors.append(
            f"{path}:{lineno}: duplicate CONFIG_{name}; first assignment at line "
            f"{first_lineno}"
        )

    unknown = sorted(set(assignments) - known)
    for name in unknown:
        lineno = assignments[name][1]
        errors.append(f"{path}:{lineno}: CONFIG_{name} is not defined by Kconfig")

    missing = sorted(known - set(assignments))
    for name in missing:
        errors.append(f"{path}: missing CONFIG_{name}")

    if not errors:
        kconf.load_config(str(path))
        check_value_took(kconf, path, assignments, errors)
        check_platform(kconf, path, errors)

    return errors, len(assignments)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kconfig", default="Kconfig", help="top-level Kconfig file")
    parser.add_argument("bconfigs", nargs="+", help="platform Bconfig files")
    args = parser.parse_args()

    kconfig = Path(args.kconfig)
    all_errors = []

    for item in args.bconfigs:
        path = Path(item)
        errors, count = check_bconfig(kconfig, path)
        if errors:
            all_errors.extend(errors)
        else:
            print(f"checkconfig: {path} covers {count} Kconfig symbols")

    if all_errors:
        for error in all_errors:
            print(f"error: {error}", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
