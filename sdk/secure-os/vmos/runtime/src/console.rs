// Copyright 2026, BEAU OS contributors
// SPDX-License-Identifier: BSD-2-Clause

#[derive(Debug, Eq, PartialEq)]
pub enum Command<'a> {
    Empty,
    Help,
    Version,
    Ping,
    Echo(&'a str),
    Selftest,
    Stats,
    Ps,
    Unknown(&'a str),
}

pub trait Output {
    fn write_str(&mut self, text: &str);
}

const LINE_BUFFER_SIZE: usize = 128;
const MAX_LINE_LENGTH: usize = LINE_BUFFER_SIZE - 1;
const BANNER: &str = "VMOS runtime console ready\n";
const PROMPT: &str = "secure:\\> ";

struct CommandSpec {
    name: &'static str,
    usage: &'static str,
}

const COMMAND_SPECS: [CommandSpec; 7] = [
    CommandSpec { name: "help", usage: "help" },
    CommandSpec { name: "version", usage: "version" },
    CommandSpec { name: "ping", usage: "ping" },
    CommandSpec { name: "echo", usage: "echo <text>" },
    CommandSpec { name: "selftest", usage: "selftest" },
    CommandSpec { name: "stats", usage: "stats" },
    CommandSpec { name: "ps", usage: "ps" },
];

struct Program {
    id: u64,
    name: &'static str,
    state: &'static str,
}

const PROGRAMS: [Program; 2] = [
    Program { id: 0, name: "monitor", state: "running" },
    Program { id: 1, name: "vmos_runtime", state: "running" },
];

pub struct Console<W: Output> {
    output: W,
    line: [u8; LINE_BUFFER_SIZE],
    length: usize,
    discard: bool,
    invalid: bool,
    previous_was_cr: bool,
    recognized: u64,
    unknown: u64,
    overflowed: u64,
}

impl<W: Output> Console<W> {
    pub const fn new(output: W) -> Self {
        Self {
            output,
            line: [0; LINE_BUFFER_SIZE],
            length: 0,
            discard: false,
            invalid: false,
            previous_was_cr: false,
            recognized: 0,
            unknown: 0,
            overflowed: 0,
        }
    }

    pub fn start(&mut self) {
        self.output.write_str(BANNER);
        self.output.write_str(PROMPT);
    }

    pub fn input(&mut self, byte: u8) {
        if byte == b'\n' && self.previous_was_cr {
            self.previous_was_cr = false;
            return;
        }
        self.previous_was_cr = false;

        match byte {
            b'\r' => {
                self.finish_line();
                self.previous_was_cr = true;
            }
            b'\n' => self.finish_line(),
            b'\t' => self.complete_command(),
            b'\x08' | b'\x7f' => self.erase_character(),
            b' '..=b'~' => self.append_character(byte),
            _ => self.invalid = true,
        }
    }

    fn append_character(&mut self, byte: u8) {
        if self.discard || self.invalid {
            return;
        }
        if self.length == MAX_LINE_LENGTH {
            self.discard = true;
            return;
        }

        self.line[self.length] = byte;
        self.length += 1;
        let character = [byte];
        if let Ok(text) = core::str::from_utf8(&character) {
            self.output.write_str(text);
        }
    }

    fn erase_character(&mut self) {
        if self.discard || self.invalid || self.length == 0 {
            return;
        }

        self.length -= 1;
        self.output.write_str("\x08 \x08");
    }

    fn complete_command(&mut self) {
        if self.discard || self.invalid {
            self.output.write_str("\x07");
            return;
        }

        let Ok(prefix) = core::str::from_utf8(&self.line[..self.length]) else {
            self.output.write_str("\x07");
            return;
        };
        if prefix.bytes().any(|byte| byte.is_ascii_whitespace()) {
            self.output.write_str("\x07");
            return;
        }

        let mut match_count = 0;
        let mut unique_match = "";
        for command in &COMMAND_SPECS {
            if command.name.starts_with(prefix) {
                match_count += 1;
                unique_match = command.name;
            }
        }

        match match_count {
            0 => self.output.write_str("\x07"),
            1 => {
                let completion = &unique_match[self.length..];
                if self.length + completion.len() + 1 > MAX_LINE_LENGTH {
                    self.output.write_str("\x07");
                    return;
                }
                for byte in completion.bytes() {
                    self.append_character(byte);
                }
                self.append_character(b' ');
            }
            _ => {
                self.output.write_str("\n");
                let mut separator = "";
                for command in &COMMAND_SPECS {
                    if command.name.starts_with(prefix) {
                        self.output.write_str(separator);
                        self.output.write_str(command.name);
                        separator = " ";
                    }
                }
                self.output.write_str("\n");
                self.output.write_str(PROMPT);
                self.output.write_str(prefix);
            }
        }
    }

    fn finish_line(&mut self) {
        self.output.write_str("\n");
        if self.discard {
            self.overflowed += 1;
            self.output.write_str("ERROR: line too long (maximum 127 bytes)\n");
        } else if self.invalid {
            self.output.write_str("ERROR: input must be printable ASCII\n");
        } else if self.length != 0 {
            self.execute_line();
        }

        self.length = 0;
        self.discard = false;
        self.invalid = false;
        self.output.write_str(PROMPT);
    }

    fn execute_line(&mut self) {
        let Ok(line) = core::str::from_utf8(&self.line[..self.length]) else {
            self.output.write_str("ERROR: input must be printable ASCII\n");
            return;
        };

        match parse_command(line) {
            Command::Empty => {}
            Command::Unknown(_) => {
                self.unknown += 1;
                dispatch_command(line, &mut self.output);
            }
            Command::Selftest => {
                self.recognized += 1;
                if runtime_selftest() {
                    self.output.write_str("selftest: PASS\n");
                } else {
                    self.output.write_str("selftest: FAIL\n");
                }
            }
            Command::Stats => {
                self.recognized += 1;
                self.write_stats();
            }
            Command::Help | Command::Version | Command::Ping | Command::Echo(_) | Command::Ps => {
                self.recognized += 1;
                dispatch_command(line, &mut self.output);
            }
        }
    }

    fn write_stats(&mut self) {
        self.output.write_str("stats: recognized=");
        write_u64(&mut self.output, self.recognized);
        self.output.write_str(" unknown=");
        write_u64(&mut self.output, self.unknown);
        self.output.write_str(" overflowed=");
        write_u64(&mut self.output, self.overflowed);
        self.output.write_str("\n");
    }
}

fn write_u64<W: Output + ?Sized>(output: &mut W, mut value: u64) {
    let mut digits = [0_u8; 20];
    let mut start = digits.len();

    loop {
        start -= 1;
        digits[start] = b'0' + (value % 10) as u8;
        value /= 10;
        if value == 0 {
            break;
        }
    }

    if let Ok(text) = core::str::from_utf8(&digits[start..]) {
        output.write_str(text);
    }
}

fn runtime_selftest() -> bool {
    matches!(parse_command("ping"), Command::Ping)
        && matches!(parse_command("unknown"), Command::Unknown("unknown"))
        && MAX_LINE_LENGTH == 127
        && byte_checksum(b"VMOS") == 69
}

fn byte_checksum(bytes: &[u8]) -> u8 {
    bytes.iter().fold(0, |sum, byte| sum.wrapping_add(*byte))
}

pub fn parse_command(input: &str) -> Command<'_> {
    let input = input.trim_matches(|character: char| character.is_ascii_whitespace());
    if input.is_empty() {
        return Command::Empty;
    }

    let separator =
        input.find(|character: char| character.is_ascii_whitespace()).unwrap_or(input.len());
    let name = &input[..separator];
    let arguments =
        input[separator..].trim_matches(|character: char| character.is_ascii_whitespace());

    match name {
        "help" => Command::Help,
        "version" => Command::Version,
        "ping" => Command::Ping,
        "echo" => Command::Echo(arguments),
        "selftest" => Command::Selftest,
        "stats" => Command::Stats,
        "ps" => Command::Ps,
        _ => Command::Unknown(name),
    }
}

pub fn dispatch_command<W: Output + ?Sized>(input: &str, output: &mut W) {
    match parse_command(input) {
        Command::Empty => {}
        Command::Help => {
            output.write_str("commands:");
            for command in &COMMAND_SPECS {
                output.write_str(" ");
                output.write_str(command.usage);
            }
            output.write_str("\n");
        }
        Command::Version => {
            output.write_str("VMOS runtime console | ARM64 | seL4/Microkit | Rust\n");
        }
        Command::Ping => output.write_str("pong\n"),
        Command::Echo(text) => {
            output.write_str(text);
            output.write_str("\n");
        }
        Command::Ps => write_programs(output),
        Command::Selftest | Command::Stats => {}
        Command::Unknown(name) => {
            output.write_str("ERROR: unknown command '");
            output.write_str(name);
            output.write_str("'; use 'help'\n");
        }
    }
}

fn write_programs<W: Output + ?Sized>(output: &mut W) {
    output.write_str("ID  NAME          STATE\n");
    for program in &PROGRAMS {
        write_u64(output, program.id);
        output.write_str("   ");
        output.write_str(program.name);
        for _ in program.name.len()..14 {
            output.write_str(" ");
        }
        output.write_str(program.state);
        output.write_str("\n");
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[derive(Default)]
    struct StringOutput(String);

    impl Output for StringOutput {
        fn write_str(&mut self, text: &str) {
            self.0.push_str(text);
        }
    }

    fn response(input: &str) -> String {
        let mut output = StringOutput::default();
        dispatch_command(input, &mut output);
        output.0
    }

    #[test]
    fn dispatches_exact_command_responses() {
        assert_eq!(response("help"), "commands: help version ping echo <text> selftest stats ps\n");
        assert_eq!(response("version"), "VMOS runtime console | ARM64 | seL4/Microkit | Rust\n");
        assert_eq!(response("ping"), "pong\n");
        assert_eq!(response("echo hello"), "hello\n");
        assert_eq!(response("echo"), "\n");
        assert_eq!(response("bad"), "ERROR: unknown command 'bad'; use 'help'\n");
    }

    #[test]
    fn parses_ps_as_a_command() {
        assert_eq!(parse_command("ps"), Command::Ps);
    }

    #[test]
    fn ps_lists_every_persistent_userspace_program() {
        assert_eq!(
            response("ps"),
            "ID  NAME          STATE\n0   monitor       running\n1   vmos_runtime  running\n"
        );
    }

    #[test]
    fn accepts_leading_trailing_and_repeated_ascii_whitespace() {
        assert_eq!(response(" \t ping \r\n"), "pong\n");
        assert_eq!(response("\techo \t  hello world  \t"), "hello world\n");
    }

    #[test]
    fn command_names_are_case_sensitive() {
        assert_eq!(response("Ping"), "ERROR: unknown command 'Ping'; use 'help'\n");
    }

    fn started_console() -> Console<StringOutput> {
        let mut console = Console::new(StringOutput::default());
        console.start();
        console
    }

    fn type_bytes(console: &mut Console<StringOutput>, bytes: &[u8]) {
        for byte in bytes {
            console.input(*byte);
        }
    }

    #[test]
    fn start_prints_banner_and_prompt() {
        let console = started_console();
        assert_eq!(console.output.0, "VMOS runtime console ready\nsecure:\\> ");
    }

    #[test]
    fn echoes_printable_input_and_executes_on_cr_or_lf() {
        let mut cr_console = started_console();
        type_bytes(&mut cr_console, b"ping\r");
        assert_eq!(
            cr_console.output.0,
            "VMOS runtime console ready\nsecure:\\> ping\npong\nsecure:\\> "
        );

        let mut lf_console = started_console();
        type_bytes(&mut lf_console, b"ping\n");
        assert_eq!(
            lf_console.output.0,
            "VMOS runtime console ready\nsecure:\\> ping\npong\nsecure:\\> "
        );
    }

    #[test]
    fn crlf_executes_one_command() {
        let mut console = started_console();
        type_bytes(&mut console, b"ping\r\n");
        assert_eq!(
            console.output.0,
            "VMOS runtime console ready\nsecure:\\> ping\npong\nsecure:\\> "
        );
    }

    #[test]
    fn backspace_and_delete_edit_with_terminal_erase() {
        let mut console = started_console();
        type_bytes(&mut console, b"pign\x08\x7fng\r");
        assert_eq!(
            console.output.0,
            "VMOS runtime console ready\nsecure:\\> pign\x08 \x08\x08 \x08ng\npong\nsecure:\\> "
        );
    }

    #[test]
    fn tab_completes_a_unique_command_and_appends_a_space() {
        let mut console = started_console();
        type_bytes(&mut console, b"ve\t");
        assert_eq!(console.output.0, "VMOS runtime console ready\nsecure:\\> version ");
        assert_eq!(&console.line[..console.length], b"version ");
    }

    #[test]
    fn tab_lists_ambiguous_commands_and_redraws_the_input() {
        let mut console = started_console();
        type_bytes(&mut console, b"s\t");
        assert_eq!(
            console.output.0,
            "VMOS runtime console ready\nsecure:\\> s\nselftest stats\nsecure:\\> s"
        );
        assert_eq!(&console.line[..console.length], b"s");
    }

    #[test]
    fn tab_rings_the_terminal_bell_when_no_command_matches() {
        let mut console = started_console();
        type_bytes(&mut console, b"z\t");
        assert_eq!(console.output.0, "VMOS runtime console ready\nsecure:\\> z\x07");
        assert_eq!(&console.line[..console.length], b"z");
    }

    #[test]
    fn tab_does_not_complete_command_arguments() {
        let mut console = started_console();
        type_bytes(&mut console, b"echo value\t");
        assert_eq!(console.output.0, "VMOS runtime console ready\nsecure:\\> echo value\x07");
        assert_eq!(&console.line[..console.length], b"echo value");
    }

    #[test]
    fn empty_line_only_refreshes_prompt() {
        let mut console = started_console();
        type_bytes(&mut console, b"\r");
        assert_eq!(console.output.0, "VMOS runtime console ready\nsecure:\\> \nsecure:\\> ");
    }

    #[test]
    fn rejects_overflow_and_recovers_on_the_next_line() {
        let mut console = started_console();
        type_bytes(&mut console, &[b'x'; 128]);
        type_bytes(&mut console, b"ignored\rping\r");

        let mut expected = String::from("VMOS runtime console ready\nsecure:\\> ");
        expected.push_str(&"x".repeat(127));
        expected.push_str(
            "\nERROR: line too long (maximum 127 bytes)\nsecure:\\> ping\npong\nsecure:\\> ",
        );
        assert_eq!(console.output.0, expected);
    }

    #[test]
    fn rejects_non_ascii_input_and_recovers() {
        let mut console = started_console();
        type_bytes(&mut console, &[b'p', 0x80, b'i', b'n', b'g', b'\r']);
        type_bytes(&mut console, b"ping\r");
        assert_eq!(
            console.output.0,
            "VMOS runtime console ready\nsecure:\\> p\nERROR: input must be printable ASCII\nsecure:\\> ping\npong\nsecure:\\> "
        );
    }

    #[test]
    fn stats_count_recognized_unknown_and_overflowed_commands() {
        let mut console = started_console();
        type_bytes(&mut console, b"ping\rbad\r");
        type_bytes(&mut console, &[b'x'; 128]);
        type_bytes(&mut console, b"\rstats\r");
        assert!(console
            .output
            .0
            .ends_with("stats\nstats: recognized=2 unknown=1 overflowed=1\nsecure:\\> "));
    }

    #[test]
    fn selftest_reports_pass() {
        let mut console = started_console();
        type_bytes(&mut console, b"selftest\r");
        assert!(console.output.0.ends_with("selftest\nselftest: PASS\nsecure:\\> "));
    }

    #[test]
    fn ps_counts_as_a_recognized_command() {
        let mut console = started_console();
        type_bytes(&mut console, b"ps\rstats\r");
        assert!(console
            .output
            .0
            .ends_with("stats\nstats: recognized=2 unknown=0 overflowed=0\nsecure:\\> "));
    }
}
