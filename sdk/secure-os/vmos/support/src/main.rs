use sha2::{Digest, Sha256};
use std::env;
use std::fs::{self, File};
use std::io::Read;
use std::path::{Component, Path, PathBuf};
use std::process::ExitCode;

const PUBLIC_ELFS: [&str; 4] = ["sel4.elf", "loader.elf", "monitor.elf", "initialiser.elf"];

#[derive(Debug, Eq, PartialEq)]
struct ManifestEntry {
    digest: String,
    path: PathBuf,
}

fn parse_manifest(contents: &str) -> Result<Vec<ManifestEntry>, String> {
    let mut entries = Vec::new();
    let mut previous: Option<String> = None;

    for (index, line) in contents.lines().enumerate() {
        let line_number = index + 1;
        let (digest, path_text) = line
            .split_once("  ")
            .ok_or_else(|| format!("manifest line {line_number}: expected SHA-256 and path"))?;
        if digest.len() != 64 || !digest.bytes().all(|byte| byte.is_ascii_hexdigit()) {
            return Err(format!("manifest line {line_number}: invalid SHA-256 digest"));
        }

        let path = PathBuf::from(path_text);
        if path_text.is_empty()
            || path.is_absolute()
            || !path.components().all(|component| matches!(component, Component::Normal(_)))
        {
            return Err(format!("manifest line {line_number}: path must be a safe relative path"));
        }

        if let Some(previous_path) = &previous {
            if path_text == previous_path {
                return Err(format!(
                    "manifest line {line_number}: duplicate path {}",
                    path.display()
                ));
            }
            if path_text < previous_path {
                return Err(format!("manifest line {line_number}: paths must be sorted"));
            }
        }
        previous = Some(path_text.to_string());
        entries.push(ManifestEntry { digest: digest.to_ascii_lowercase(), path });
    }

    if entries.is_empty() {
        return Err("manifest must not be empty".to_string());
    }
    Ok(entries)
}

fn file_digest(path: &Path) -> Result<String, String> {
    let mut file =
        File::open(path).map_err(|error| format!("cannot open {}: {error}", path.display()))?;
    let mut hasher = Sha256::new();
    let mut buffer = [0_u8; 64 * 1024];
    loop {
        let count = file
            .read(&mut buffer)
            .map_err(|error| format!("cannot read {}: {error}", path.display()))?;
        if count == 0 {
            break;
        }
        hasher.update(&buffer[..count]);
    }
    Ok(format!("{:x}", hasher.finalize()))
}

fn verify_manifest(root: &Path, manifest: &Path) -> Result<(), String> {
    let contents = fs::read_to_string(manifest)
        .map_err(|error| format!("cannot read manifest {}: {error}", manifest.display()))?;
    for entry in parse_manifest(&contents)? {
        let source = root.join(&entry.path);
        if !source.is_file() {
            return Err(format!("vendored source is missing: {}", source.display()));
        }
        let actual = file_digest(&source)?;
        if actual != entry.digest {
            return Err(format!(
                "vendored source digest mismatch: {} (expected {}, got {})",
                entry.path.display(),
                entry.digest,
                actual
            ));
        }
    }
    Ok(())
}

fn safe_component(value: &str, option: &str) -> Result<(), String> {
    let path = Path::new(value);
    if value.is_empty()
        || path.is_absolute()
        || !path.components().all(|component| matches!(component, Component::Normal(_)))
        || path.components().count() != 1
    {
        return Err(format!("{option} must be one safe path component"));
    }
    Ok(())
}

fn publish_elfs(sdk: &Path, board: &str, config: &str, output: &Path) -> Result<(), String> {
    safe_component(board, "--board")?;
    safe_component(config, "--config")?;
    let source_directory = sdk.join("board").join(board).join(config).join("elf");

    for name in PUBLIC_ELFS {
        let source = source_directory.join(name);
        if !source.is_file() {
            return Err(format!("required internal ELF is missing: {}", source.display()));
        }
    }

    let destination_directory = output.join("elf");
    fs::create_dir_all(&destination_directory).map_err(|error| {
        format!("cannot create public ELF directory {}: {error}", destination_directory.display())
    })?;
    for name in PUBLIC_ELFS {
        let source = source_directory.join(name);
        let destination = destination_directory.join(name);
        fs::copy(&source, &destination).map_err(|error| {
            format!("cannot publish {} as {}: {error}", source.display(), destination.display())
        })?;
    }
    Ok(())
}

#[cfg(test)]
fn scan_architecture_markers(path: &Path, violations: &mut Vec<String>) -> Result<(), String> {
    for entry in
        fs::read_dir(path).map_err(|error| format!("cannot scan {}: {error}", path.display()))?
    {
        let entry = entry.map_err(|error| format!("cannot scan {}: {error}", path.display()))?;
        let entry_path = entry.path();
        if entry_path.is_dir() {
            scan_architecture_markers(&entry_path, violations)?;
            continue;
        }
        let extension = entry_path.extension().and_then(|value| value.to_str());
        let is_makefile =
            entry_path.file_name().and_then(|value| value.to_str()) == Some("Makefile");
        if !is_makefile
            && !matches!(extension, Some("c" | "h" | "rs" | "s" | "S" | "toml" | "xml" | "ld"))
        {
            continue;
        }
        let contents = fs::read_to_string(&entry_path)
            .map_err(|error| format!("cannot read {}: {error}", entry_path.display()))?;
        let lowercase = contents.to_ascii_lowercase();
        if ["x86", "ia32", "pc99", "config_vtx"].iter().any(|marker| lowercase.contains(marker)) {
            violations.push(format!("x86 marker remains in {}", entry_path.display()));
        }
    }
    Ok(())
}

#[cfg(test)]
fn audit_arm64_only(root: &Path) -> Result<(), String> {
    let forbidden_paths = [
        "include/arch/x86",
        "include/plat/pc99",
        "src/arch/x86",
        "src/plat/pc99",
        "libsel4/arch_include/x86",
        "libsel4/sel4_arch_include/ia32",
        "libsel4/sel4_arch_include/x86_64",
        "libsel4/sel4_plat_include/pc99",
        "configs/X64_verified.cmake",
    ];
    let mut violations = Vec::new();
    for relative in forbidden_paths {
        if root.join(relative).exists() {
            violations.push(format!("forbidden architecture path remains: {relative}"));
        }
    }

    let root_cmake = fs::read_to_string(root.join("CMakeLists.txt"))
        .map_err(|error| format!("cannot read CMakeLists.txt: {error}"))?;
    if ["KernelArchX86", "KernelSel4ArchX86", "KernelSel4ArchIA32", "elf_x86"]
        .iter()
        .any(|marker| root_cmake.contains(marker))
    {
        violations.push("x86 build logic remains in CMakeLists.txt".to_string());
    }

    let sel4_config = fs::read_to_string(root.join("configs/seL4Config.cmake"))
        .map_err(|error| format!("cannot read configs/seL4Config.cmake: {error}"))?;
    if ["\"x86_64;", "\"ia32;", "\"x86;KernelArchX86"]
        .iter()
        .any(|marker| sel4_config.contains(marker))
    {
        violations
            .push("x86 architecture remains selectable in configs/seL4Config.cmake".to_string());
    }

    for relative in ["vmos/libmicrokit", "vmos/monitor"] {
        scan_architecture_markers(&root.join(relative), &mut violations)?;
    }

    let tool_config = root.join("vmos/tool/microkit/src/sel4.rs");
    let tool_config_contents = fs::read_to_string(&tool_config)
        .map_err(|error| format!("cannot read {}: {error}", tool_config.display()))?;
    if tool_config_contents.contains("\"x86_64\" => Arch::X86_64") {
        violations.push("VMOS tool still accepts x86_64 SDK configurations".to_string());
    }

    if violations.is_empty() {
        Ok(())
    } else {
        Err(violations.join("\n"))
    }
}

fn value_after(args: &[String], option: &str) -> Result<PathBuf, String> {
    let index = args
        .iter()
        .position(|argument| argument == option)
        .ok_or_else(|| format!("missing required option {option}"))?;
    args.get(index + 1).map(PathBuf::from).ok_or_else(|| format!("missing value for {option}"))
}

fn string_after(args: &[String], option: &str) -> Result<String, String> {
    let index = args
        .iter()
        .position(|argument| argument == option)
        .ok_or_else(|| format!("missing required option {option}"))?;
    args.get(index + 1).cloned().ok_or_else(|| format!("missing value for {option}"))
}

fn run(args: &[String]) -> Result<(), String> {
    match args.first().map(String::as_str) {
        Some("verify") => {
            let root = value_after(args, "--root")?;
            let manifest = value_after(args, "--manifest")?;
            verify_manifest(&root, &manifest)
        }
        Some("publish") => {
            let sdk = value_after(args, "--sdk")?;
            let board = string_after(args, "--board")?;
            let config = string_after(args, "--config")?;
            let output = value_after(args, "--output")?;
            publish_elfs(&sdk, &board, &config, &output)
        }
        Some(command) => Err(format!("unsupported command: {command}")),
        None => Err("missing command (expected: verify or publish)".to_string()),
    }
}

fn main() -> ExitCode {
    let args: Vec<String> = env::args().skip(1).collect();
    match run(&args) {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("vmos-build-support: error: {error}");
            ExitCode::FAILURE
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use sha2::{Digest, Sha256};
    use std::fs;
    use tempfile::tempdir;

    fn digest(data: &[u8]) -> String {
        format!("{:x}", Sha256::digest(data))
    }

    #[test]
    fn parses_a_valid_sorted_manifest() {
        let first = "0".repeat(64);
        let second = "f".repeat(64);
        let entries =
            parse_manifest(&format!("{first}  Cargo.toml\n{second}  loader/src/main.c\n")).unwrap();
        assert_eq!(entries.len(), 2);
        assert_eq!(entries[0].path, PathBuf::from("Cargo.toml"));
        assert_eq!(entries[1].path, PathBuf::from("loader/src/main.c"));
    }

    #[test]
    fn rejects_malformed_hashes() {
        let error = parse_manifest("abc  Cargo.toml\n").unwrap_err();
        assert!(error.contains("line 1"));
        assert!(error.contains("SHA-256"));
    }

    #[test]
    fn rejects_duplicate_and_unsorted_paths() {
        let digest = "0".repeat(64);
        let duplicate =
            parse_manifest(&format!("{digest}  Cargo.toml\n{digest}  Cargo.toml\n")).unwrap_err();
        assert!(duplicate.contains("duplicate"));

        let unsorted = parse_manifest(&format!("{digest}  loader/main.c\n{digest}  Cargo.toml\n"))
            .unwrap_err();
        assert!(unsorted.contains("sorted"));
    }

    #[test]
    fn rejects_paths_outside_the_vmos_root() {
        let digest = "0".repeat(64);
        for path in ["/tmp/file", "../outside/file", "loader/../../file"] {
            let error = parse_manifest(&format!("{digest}  {path}\n")).unwrap_err();
            assert!(error.contains("relative path"), "{error}");
        }
    }

    #[test]
    fn reports_missing_and_mismatched_files() {
        let directory = tempdir().unwrap();
        let manifest = directory.path().join("manifest");
        fs::write(&manifest, format!("{}  missing.c\n", "0".repeat(64))).unwrap();
        let missing = verify_manifest(directory.path(), &manifest).unwrap_err();
        assert!(missing.contains("missing.c"));
        assert!(missing.contains("missing"));

        fs::write(directory.path().join("source.c"), b"actual").unwrap();
        fs::write(&manifest, format!("{}  source.c\n", digest(b"different"))).unwrap();
        let mismatch = verify_manifest(directory.path(), &manifest).unwrap_err();
        assert!(mismatch.contains("source.c"));
        assert!(mismatch.contains("digest"));
    }

    #[test]
    fn verifies_a_valid_file_without_external_sources() {
        let directory = tempdir().unwrap();
        let source = directory.path().join("source.c");
        let manifest = directory.path().join("manifest");
        fs::write(&source, b"vendored source\n").unwrap();
        fs::write(&manifest, format!("{}  source.c\n", digest(b"vendored source\n"))).unwrap();

        verify_manifest(directory.path(), &manifest).unwrap();
    }

    #[test]
    fn publish_reports_a_missing_internal_elf() {
        let directory = tempdir().unwrap();
        let error = publish_elfs(
            &directory.path().join("sdk"),
            "qemu_virt_aarch64",
            "debug",
            &directory.path().join("output"),
        )
        .unwrap_err();

        assert!(error.contains("sel4.elf"), "{error}");
        assert!(error.contains("missing"), "{error}");
    }

    #[test]
    fn publish_copies_exactly_the_public_elf_set_and_can_be_repeated() {
        let directory = tempdir().unwrap();
        let sdk = directory.path().join("sdk");
        let internal = sdk.join("board/qemu_virt_aarch64/debug/elf");
        let output = directory.path().join("output");
        fs::create_dir_all(&internal).unwrap();
        for (name, contents) in [
            ("sel4.elf", b"sel4".as_slice()),
            ("loader.elf", b"loader".as_slice()),
            ("monitor.elf", b"monitor".as_slice()),
            ("initialiser.elf", b"initialiser".as_slice()),
        ] {
            fs::write(internal.join(name), contents).unwrap();
        }
        fs::write(internal.join("private.elf"), b"private").unwrap();

        publish_elfs(&sdk, "qemu_virt_aarch64", "debug", &output).unwrap();
        publish_elfs(&sdk, "qemu_virt_aarch64", "debug", &output).unwrap();

        let mut names: Vec<_> = fs::read_dir(output.join("elf"))
            .unwrap()
            .map(|entry| entry.unwrap().file_name().into_string().unwrap())
            .collect();
        names.sort();
        assert_eq!(names, ["initialiser.elf", "loader.elf", "monitor.elf", "sel4.elf"]);
        assert_eq!(fs::read(output.join("elf/sel4.elf")).unwrap(), b"sel4");
        assert_eq!(fs::read(output.join("elf/loader.elf")).unwrap(), b"loader");
        assert_eq!(fs::read(output.join("elf/monitor.elf")).unwrap(), b"monitor");
        assert_eq!(fs::read(output.join("elf/initialiser.elf")).unwrap(), b"initialiser");
    }

    #[test]
    fn repository_contains_no_x86_support() {
        let repository = Path::new(env!("CARGO_MANIFEST_DIR")).join("../..");
        audit_arm64_only(&repository).unwrap();
    }
}
