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
    Unknown(&'a str),
}

pub trait Output {
    fn write_str(&mut self, text: &str);
}

const LINE_BUFFER_SIZE: usize = 128;
const MAX_LINE_LENGTH: usize = LINE_BUFFER_SIZE - 1;
const BANNER: &str = "VMOS runtime console ready\n";
const PROMPT: &str = "vmos> ";

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
            Command::Help | Command::Version | Command::Ping | Command::Echo(_) => {
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

fn write_u64<W: Output>(output: &mut W, mut value: u64) {
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
        _ => Command::Unknown(name),
    }
}

pub fn dispatch_command<W: Output + ?Sized>(input: &str, output: &mut W) {
    match parse_command(input) {
        Command::Empty => {}
        Command::Help => {
            output.write_str("commands: help version ping echo <text> selftest stats\n");
        }
        Command::Version => {
            output.write_str("VMOS runtime console | ARM64 | seL4/Microkit | Rust\n");
        }
        Command::Ping => output.write_str("pong\n"),
        Command::Echo(text) => {
            output.write_str(text);
            output.write_str("\n");
        }
        Command::Selftest | Command::Stats => {}
        Command::Unknown(name) => {
            output.write_str("ERROR: unknown command '");
            output.write_str(name);
            output.write_str("'; use 'help'\n");
        }
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
        assert_eq!(response("help"), "commands: help version ping echo <text> selftest stats\n");
        assert_eq!(response("version"), "VMOS runtime console | ARM64 | seL4/Microkit | Rust\n");
        assert_eq!(response("ping"), "pong\n");
        assert_eq!(response("echo hello"), "hello\n");
        assert_eq!(response("echo"), "\n");
        assert_eq!(response("bad"), "ERROR: unknown command 'bad'; use 'help'\n");
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
        assert_eq!(console.output.0, "VMOS runtime console ready\nvmos> ");
    }

    #[test]
    fn echoes_printable_input_and_executes_on_cr_or_lf() {
        let mut cr_console = started_console();
        type_bytes(&mut cr_console, b"ping\r");
        assert_eq!(cr_console.output.0, "VMOS runtime console ready\nvmos> ping\npong\nvmos> ");

        let mut lf_console = started_console();
        type_bytes(&mut lf_console, b"ping\n");
        assert_eq!(lf_console.output.0, "VMOS runtime console ready\nvmos> ping\npong\nvmos> ");
    }

    #[test]
    fn crlf_executes_one_command() {
        let mut console = started_console();
        type_bytes(&mut console, b"ping\r\n");
        assert_eq!(console.output.0, "VMOS runtime console ready\nvmos> ping\npong\nvmos> ");
    }

    #[test]
    fn backspace_and_delete_edit_with_terminal_erase() {
        let mut console = started_console();
        type_bytes(&mut console, b"pign\x08\x7fng\r");
        assert_eq!(
            console.output.0,
            "VMOS runtime console ready\nvmos> pign\x08 \x08\x08 \x08ng\npong\nvmos> "
        );
    }

    #[test]
    fn empty_line_only_refreshes_prompt() {
        let mut console = started_console();
        type_bytes(&mut console, b"\r");
        assert_eq!(console.output.0, "VMOS runtime console ready\nvmos> \nvmos> ");
    }

    #[test]
    fn rejects_overflow_and_recovers_on_the_next_line() {
        let mut console = started_console();
        type_bytes(&mut console, &[b'x'; 128]);
        type_bytes(&mut console, b"ignored\rping\r");

        let mut expected = String::from("VMOS runtime console ready\nvmos> ");
        expected.push_str(&"x".repeat(127));
        expected.push_str("\nERROR: line too long (maximum 127 bytes)\nvmos> ping\npong\nvmos> ");
        assert_eq!(console.output.0, expected);
    }

    #[test]
    fn rejects_non_ascii_input_and_recovers() {
        let mut console = started_console();
        type_bytes(&mut console, &[b'p', 0x80, b'i', b'n', b'g', b'\r']);
        type_bytes(&mut console, b"ping\r");
        assert_eq!(
            console.output.0,
            "VMOS runtime console ready\nvmos> p\nERROR: input must be printable ASCII\nvmos> ping\npong\nvmos> "
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
            .ends_with("stats\nstats: recognized=2 unknown=1 overflowed=1\nvmos> "));
    }

    #[test]
    fn selftest_reports_pass() {
        let mut console = started_console();
        type_bytes(&mut console, b"selftest\r");
        assert!(console.output.0.ends_with("selftest\nselftest: PASS\nvmos> "));
    }
}
