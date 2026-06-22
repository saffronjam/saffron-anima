//! Sets an `$ORIGIN` rpath on the player binary so a staged export finds the C++ runtime libs
//! (`libc++.so.1` / `libc++abi.so.1`, pulled in by the vendored Jolt physics) sitting beside the
//! executable. The default library search path still applies, so a dev/in-toolbox run resolves
//! them from `/usr/lib64` as before — this only adds the folder-local lookup the shipped app needs.

fn main() {
    println!("cargo:rustc-link-arg=-Wl,-rpath,$ORIGIN");
    // Some linkers require `-z origin` to honor `$ORIGIN` in an rpath.
    println!("cargo:rustc-link-arg=-Wl,-z,origin");
}
