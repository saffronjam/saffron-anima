//! The `SaffronAnima` host binary entry point: `int main(){ return runHost(...); }`.
//!
//! Builds the editor host (the [`saffron_host::run_host`] apex), runs the loop, and exits
//! with its process exit code. The mode (headless editor vs standalone windowed) is decided
//! inside `run_host` from the editor-set environment.

fn main() -> std::process::ExitCode {
    let code = saffron_host::run_host("Saffron Anima", 1600, 900);
    std::process::ExitCode::from(u8::try_from(code).unwrap_or(1))
}
