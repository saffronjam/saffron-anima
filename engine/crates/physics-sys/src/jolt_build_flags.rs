// The cross-platform-deterministic Jolt build flag set, as pure data.
//
// This is the single source of truth for the flags, `include!`d into `build.rs` (where it is
// fed to `cc`) and declared as a `mod` in the crate's test build (where the flag set is
// asserted). Sharing the data this way keeps the determinism contract testable under
// `cargo test` without coupling the test to `cc` or to compiling Jolt.
//
// The values mirror `cmake/Dependencies.cmake:75-109` together with what Jolt's own CMake
// derives from those options for an x86_64 clang build (`Jolt/Jolt.cmake:540-692`,
// `Build/CMakeLists.txt:230-237`), so a reviewer can diff the two:
//
//   - CROSS_PLATFORM_DETERMINISTIC ON  → `JPH_CROSS_PLATFORM_DETERMINISTIC` (`Dependencies.cmake:75`)
//   - DOUBLE_PRECISION OFF             → single precision = the *absence* of `JPH_DOUBLE_PRECISION` (`:76`)
//   - the default x86 instruction-set options (USE_AVX2/AVX/SSE4.x/LZCNT/TZCNT/F16C ON,
//     AVX512 OFF, FMADD ON-but-suppressed-by-determinism) → the `-m*` flags and matching
//     `JPH_USE_*` defines Jolt emits in lockstep (`Jolt.cmake:590-684`). The defines and the
//     `-m` flags MUST stay paired: a define without its flag (or vice versa) is a silent ABI
//     skew, exactly what `EMIT_X86_INSTRUCTION_SET_DEFINITIONS` exists to prevent.
//   - `-ffp-model=precise -ffp-contract=off` is the determinism FP pairing (`Build/CMakeLists.txt:233,237`).
//   - `-Wno-error` overrides Jolt's own `-Werror` (`Dependencies.cmake:93`).
//   - `-pthread` is dropped from compile and re-added at link only (`Dependencies.cmake:109`).
//
// A plain `//` header (not `//!`) is deliberate: an inner doc comment is illegal when this file
// is `include!`d mid-`build.rs` rather than parsed as a module root.

/// The frozen Saffron determinism flag set for vendored Jolt's translation units.
pub(crate) struct JoltBuildFlags {
    /// Preprocessor defines applied to every Jolt TU *and* the shim TU (they change Jolt's
    /// struct layout, so they must reach all of Jolt and the shim identically).
    pub(crate) defines: &'static [(&'static str, Option<&'static str>)],
    /// Arch + floating-point flags confined to this crate's TUs. The FP pair
    /// (`-ffp-model=precise` + `-ffp-contract=off`) is the determinism contract; the `-m*`
    /// arch flags are Jolt's x86 instruction-set selection, applied here only.
    pub(crate) arch_fp_flags: &'static [&'static str],
    /// Jolt builds itself with `-Werror`; clang 21 flags the FP-model/FP-contract pairing
    /// under `-Woverriding-option`, failing Jolt's own build. Drop `-Werror` (`Dependencies.cmake:93`)
    /// and silence the expected `-Woverriding-option` (the pairing is exactly what we want —
    /// Jolt does the same for its emscripten path, `Build/CMakeLists.txt:242`).
    pub(crate) warning_flags: &'static [&'static str],
    /// Native threads linked at link time — the C++ `-pthread`, which was dropped from the
    /// per-TU *compile* options (it only mattered at link) and re-emitted as a link flag
    /// (`Dependencies.cmake:105-109`).
    pub(crate) link_threads: bool,
}

impl JoltBuildFlags {
    /// The frozen flag set. `const` so it is a single immutable definition with no runtime
    /// construction.
    pub(crate) const DETERMINISTIC: Self = Self {
        defines: &[
            // The master determinism switch and the single-precision contract (the latter by
            // omission — `JPH_DOUBLE_PRECISION` is deliberately never listed).
            ("JPH_CROSS_PLATFORM_DETERMINISTIC", None),
            // ObjectStream + RTTI attributes are ON in Jolt's defaults (`Build/CMakeLists.txt:102`),
            // and the engine builds the full library, so the shim must agree.
            ("JPH_OBJECT_STREAM", None),
            // x86 instruction-set defines, paired with the `-m*` flags below. AVX512 OFF and
            // FMADD suppressed-by-determinism are the *absence* of `JPH_USE_AVX512`/`JPH_USE_FMADD`.
            ("JPH_USE_AVX2", None),
            ("JPH_USE_AVX", None),
            ("JPH_USE_SSE4_1", None),
            ("JPH_USE_SSE4_2", None),
            ("JPH_USE_LZCNT", None),
            ("JPH_USE_TZCNT", None),
            ("JPH_USE_F16C", None),
            // Distribution-style config: no asserts, profiler, or FP exceptions. This keeps the
            // `-sys` archive identical whether the consuming Rust crate is built dev or release,
            // and is the standard Jolt shipping ABI.
            ("NDEBUG", None),
        ],
        arch_fp_flags: &[
            "-ffp-model=precise",
            "-ffp-contract=off",
            // The x86 instruction-set flags Jolt emits for USE_AVX2 ON + determinism
            // (`Jolt.cmake:657,667-674,681`); FMADD's `-mfma` is omitted under determinism.
            "-mavx2",
            "-mbmi",
            "-mpopcnt",
            "-mlzcnt",
            "-mf16c",
            "-mfpmath=sse",
        ],
        warning_flags: &["-Wno-error", "-Wno-overriding-option"],
        link_threads: true,
    };
}
