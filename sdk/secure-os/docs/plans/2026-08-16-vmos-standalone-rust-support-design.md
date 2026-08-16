# VMOS Standalone Rust Support Design

## Scope

VMOS must remain buildable and testable after the adjacent `../microkit`
checkout is removed. Existing vendored Microkit implementation files remain
unchanged. New project functionality is implemented in Rust.

This change also publishes the primary ARM64 ELF outputs at:

```text
build/vmos/<board>/<config>/elf/
```

The deeper Microkit SDK layout remains an internal compatibility staging area
so the unchanged Microkit host tool can continue discovering board metadata.

## Rust support tool

A standalone Rust crate under `vmos/support/` provides two commands:

- `verify`: validate vendored files against an in-repository SHA-256 manifest;
- `publish`: copy the four required ARM64 ELFs from the internal SDK to the
  shallow output directory.

The crate is deliberately separate from the vendored Microkit Cargo workspace,
so adding it does not modify any upstream file listed by the integrity
manifest.

## Standalone integrity model

`vmos/UPSTREAM_SHA256` replaces the path-only manifest as the immutable source
baseline. Each sorted entry records a SHA-256 digest and a path relative to
`vmos/`.

```text
UPSTREAM_SHA256
    |
    v
Rust verifier
    +--> reject malformed, duplicate, absolute, or parent-relative paths
    +--> reject missing/non-file entries
    +--> reject digest mismatches
    `--> succeed without reading any sibling repository
```

Historical design documents may describe where the initial import came from,
but active builds, tests, and usage documentation must not access a sibling
Microkit checkout.

## Output publication

The unchanged build pipeline first produces a Microkit-compatible internal SDK:

```text
build/vmos/<board>/<config>/sdk/board/<board>/<config>/elf/
```

After the internal build succeeds, the Rust publisher validates and copies:

- `sel4.elf`
- `loader.elf`
- `monitor.elf`
- `initialiser.elf`

to:

```text
build/vmos/<board>/<config>/elf/
```

Publication fails before reporting success if any required source artifact is
missing. The internal SDK is retained for `microkit` tool compatibility and is
not presented as the primary user output.

## Build integration

`Makefile.vmos` invokes the existing legacy Python builder, then invokes the
Rust publisher. `test` runs both the existing legacy regression tests and Rust
unit tests. `verify-source` invokes only the Rust integrity verifier.

No new feature logic is added to `build_vmos.py`. Python tests that directly
access a sibling repository are removed.

## Verification

1. Rust unit tests cover malformed manifests, digest mismatches, successful
   verification, missing ELF inputs, and successful shallow publication.
2. Deleting or renaming the sibling Microkit checkout cannot affect tests or
   builds.
3. The existing ARM64 debug build succeeds and all four shallow ELF outputs
   are AArch64 ELF64 files.
4. Vendored Microkit files continue matching `UPSTREAM_SHA256`.

