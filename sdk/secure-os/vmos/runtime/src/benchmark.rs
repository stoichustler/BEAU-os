// Copyright 2026, BEAU OS contributors
// SPDX-License-Identifier: BSD-2-Clause

const MAX_ITERATIONS: u32 = 100_000;
const WARMUP_ITERATIONS: u32 = 32;

#[derive(Debug, Eq, PartialEq)]
pub struct Stats {
    pub iterations: u32,
    pub min_ticks: u64,
    pub average_ticks: u64,
    pub max_ticks: u64,
    pub counter_hz: u64,
}

#[derive(Debug, Eq, PartialEq)]
pub enum RunError {
    InvalidIterations,
    InvalidCounterFrequency,
    ReplyMismatch,
    Unavailable,
}

pub trait IpcBackend {
    fn counter_frequency_hz(&self) -> u64;
    fn round_trip_ticks(&mut self) -> Result<u64, RunError>;
}

#[derive(Debug, Eq, PartialEq)]
pub enum ParseError {
    Usage,
    IterationsOutOfRange,
}

pub fn parse_ipc_iterations(arguments: &str) -> Result<u32, ParseError> {
    let mut words = arguments.split_ascii_whitespace();
    if words.next() != Some("ipc") {
        return Err(ParseError::Usage);
    }

    let iterations = match words.next() {
        Some(value) => value.parse().map_err(|_| ParseError::Usage)?,
        None => 1_000,
    };
    if words.next().is_some() {
        return Err(ParseError::Usage);
    }
    if iterations == 0 || iterations > MAX_ITERATIONS {
        return Err(ParseError::IterationsOutOfRange);
    }

    Ok(iterations)
}

pub fn run_ipc<B: IpcBackend>(backend: &mut B, iterations: u32) -> Result<Stats, RunError> {
    if iterations == 0 || iterations > MAX_ITERATIONS {
        return Err(RunError::InvalidIterations);
    }
    let counter_hz = backend.counter_frequency_hz();
    if counter_hz == 0 {
        return Err(RunError::InvalidCounterFrequency);
    }

    for _ in 0..WARMUP_ITERATIONS {
        backend.round_trip_ticks()?;
    }

    let mut minimum = u64::MAX;
    let mut maximum = 0_u64;
    let mut total = 0_u128;
    for _ in 0..iterations {
        let ticks = backend.round_trip_ticks()?;
        minimum = minimum.min(ticks);
        maximum = maximum.max(ticks);
        total += u128::from(ticks);
    }

    Ok(Stats {
        iterations,
        min_ticks: minimum,
        average_ticks: (total / u128::from(iterations)) as u64,
        max_ticks: maximum,
        counter_hz,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    struct SequenceBackend {
        calls: u32,
    }

    struct ZeroFrequencyBackend;

    impl IpcBackend for ZeroFrequencyBackend {
        fn counter_frequency_hz(&self) -> u64 {
            0
        }

        fn round_trip_ticks(&mut self) -> Result<u64, RunError> {
            Ok(1)
        }
    }

    struct FailingBackend;

    impl IpcBackend for FailingBackend {
        fn counter_frequency_hz(&self) -> u64 {
            62_500_000
        }

        fn round_trip_ticks(&mut self) -> Result<u64, RunError> {
            Err(RunError::ReplyMismatch)
        }
    }

    impl IpcBackend for SequenceBackend {
        fn counter_frequency_hz(&self) -> u64 {
            62_500_000
        }

        fn round_trip_ticks(&mut self) -> Result<u64, RunError> {
            self.calls += 1;
            let measured_index = self.calls.saturating_sub(WARMUP_ITERATIONS);
            Ok(match measured_index {
                1 => 40,
                2 => 20,
                3 => 30,
                _ => 9_999,
            })
        }
    }

    #[test]
    fn ipc_benchmark_uses_a_bounded_default_iteration_count() {
        assert_eq!(parse_ipc_iterations("ipc"), Ok(1_000));
    }

    #[test]
    fn ipc_benchmark_accepts_an_explicit_iteration_count() {
        assert_eq!(parse_ipc_iterations("ipc 42"), Ok(42));
    }

    #[test]
    fn ipc_benchmark_rejects_zero_iterations() {
        assert_eq!(parse_ipc_iterations("ipc 0"), Err(ParseError::IterationsOutOfRange));
    }

    #[test]
    fn ipc_benchmark_enforces_the_upper_iteration_boundary() {
        assert_eq!(parse_ipc_iterations("ipc 100000"), Ok(100_000));
        assert_eq!(parse_ipc_iterations("ipc 100001"), Err(ParseError::IterationsOutOfRange));
    }

    #[test]
    fn ipc_benchmark_excludes_warmup_and_reports_tick_statistics() {
        let mut backend = SequenceBackend { calls: 0 };

        assert_eq!(
            run_ipc(&mut backend, 3),
            Ok(Stats {
                iterations: 3,
                min_ticks: 20,
                average_ticks: 30,
                max_ticks: 40,
                counter_hz: 62_500_000,
            })
        );
        assert_eq!(backend.calls, WARMUP_ITERATIONS + 3);
    }

    #[test]
    fn ipc_benchmark_rejects_invalid_direct_iteration_counts() {
        let mut backend = SequenceBackend { calls: 0 };

        assert_eq!(run_ipc(&mut backend, 0), Err(RunError::InvalidIterations));
        assert_eq!(run_ipc(&mut backend, MAX_ITERATIONS + 1), Err(RunError::InvalidIterations));
        assert_eq!(backend.calls, 0);
    }

    #[test]
    fn ipc_benchmark_rejects_a_zero_counter_frequency() {
        assert_eq!(run_ipc(&mut ZeroFrequencyBackend, 1), Err(RunError::InvalidCounterFrequency));
    }

    #[test]
    fn ipc_benchmark_propagates_backend_errors() {
        assert_eq!(run_ipc(&mut FailingBackend, 1), Err(RunError::ReplyMismatch));
    }
}
