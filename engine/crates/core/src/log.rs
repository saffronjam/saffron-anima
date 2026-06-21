//! Subsystem-tagged logging.
//!
//! Every line is `[saffron:<subsystem>] <message>` (with `warn:` / `error:`
//! ahead of the message for the non-info levels). The format is grep-relied-upon
//! and the validation-clean-log gate parses it, so it is frozen.
//!
//! The subsystem tag is derived from the caller's `module_path!()` (the leading
//! `saffron_<area>` crate segment → `<area>`).

/// Severity of a log line. `Warn` and `Error` insert their level before the
/// message.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum LogLevel {
    /// Informational; no level prefix.
    Info,
    /// A warning; prefixed `warn:`.
    Warn,
    /// An error; prefixed `error:`.
    Error,
}

/// Prints one stdout line `[saffron:<subsystem>] <message>` (`warn:` / `error:`
/// ahead of the message for the non-info levels).
///
/// Callers reporting on another component's behalf (e.g. the Vulkan debug
/// messenger) pass the subsystem explicitly; everything else uses the
/// [`log_info!`](crate::log_info) / [`log_warn!`](crate::log_warn) /
/// [`log_error!`](crate::log_error) macros, which derive the tag from
/// `module_path!()`.
pub fn log(level: LogLevel, subsystem: &str, message: &str) {
    println!("{}", format_line(level, subsystem, message));
}

/// Renders the single frozen log line for a level/subsystem/message.
fn format_line(level: LogLevel, subsystem: &str, message: &str) -> String {
    match level {
        LogLevel::Info => format!("[saffron:{subsystem}] {message}"),
        LogLevel::Warn => format!("[saffron:{subsystem}] warn: {message}"),
        LogLevel::Error => format!("[saffron:{subsystem}] error: {message}"),
    }
}

/// Maps a caller's `module_path!()` to its subsystem tag.
///
/// `saffron_core::uuid` → `core`, `saffron_rendering` → `rendering`. A path that
/// does not start with the `saffron_` crate prefix falls back to `engine`.
#[must_use]
pub fn subsystem_of(module_path: &str) -> &str {
    let crate_segment = module_path.split("::").next().unwrap_or(module_path);
    crate_segment.strip_prefix("saffron_").unwrap_or("engine")
}

/// Logs at info, tagged with the calling crate's subsystem.
#[macro_export]
macro_rules! log_info {
    ($($arg:tt)*) => {
        $crate::log(
            $crate::LogLevel::Info,
            $crate::subsystem_of(::core::module_path!()),
            &::std::format!($($arg)*),
        )
    };
}

/// Logs at warn, tagged with the calling crate's subsystem.
#[macro_export]
macro_rules! log_warn {
    ($($arg:tt)*) => {
        $crate::log(
            $crate::LogLevel::Warn,
            $crate::subsystem_of(::core::module_path!()),
            &::std::format!($($arg)*),
        )
    };
}

/// Logs at error, tagged with the calling crate's subsystem.
#[macro_export]
macro_rules! log_error {
    ($($arg:tt)*) => {
        $crate::log(
            $crate::LogLevel::Error,
            $crate::subsystem_of(::core::module_path!()),
            &::std::format!($($arg)*),
        )
    };
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn subsystem_strips_crate_prefix() {
        assert_eq!(subsystem_of("saffron_core"), "core");
        assert_eq!(subsystem_of("saffron_core::uuid"), "core");
        assert_eq!(
            subsystem_of("saffron_rendering::renderer::pass"),
            "rendering"
        );
    }

    #[test]
    fn subsystem_falls_back_to_engine() {
        assert_eq!(subsystem_of("some_other_crate"), "engine");
        assert_eq!(subsystem_of(""), "engine");
    }

    #[test]
    fn line_format_matches_contract() {
        assert_eq!(
            format_line(LogLevel::Info, "core", "hello"),
            "[saffron:core] hello"
        );
        assert_eq!(
            format_line(LogLevel::Warn, "core", "careful"),
            "[saffron:core] warn: careful"
        );
        assert_eq!(
            format_line(LogLevel::Error, "core", "boom"),
            "[saffron:core] error: boom"
        );
    }
}
