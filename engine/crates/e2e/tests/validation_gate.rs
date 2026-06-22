//! The validation-clean gate's regression probe: a planted Vulkan validation error must be
//! *caught*, proving the detector is wired (the right messenger prefix, the validation layer
//! actually enabled) rather than silently disabled.
//!
//! A gate that can never go red is worthless. Every other render-touching e2e asserts
//! `validation_errors()` is empty; this one boots the host with `SAFFRON_VK_PLANT_VALIDATION_ERROR`
//! set — which records one out-of-spec `vkCmdSetViewport` into each scene frame — and asserts the
//! harness *sees* the resulting `ERROR  vulkan  [validation] …` lines. If this test ever
//! goes green-with-empty-errors, the gate has been silently disabled and the suite has lost its
//! only headless detector for GPU-state bugs.

use std::time::Duration;

use saffron_e2e::TestEngine;

/// Booting with the plant env set surfaces the planted validation error through the harness's
/// `validation_errors()` filter — the detector is live (the inverse of every other test's
/// empty-errors assertion).
#[test]
fn planted_validation_error_is_detected() {
    let mut engine =
        TestEngine::boot(&[("SAFFRON_VK_PLANT_VALIDATION_ERROR", "1")]).expect("boot engine");

    // Render a few frames so the planted out-of-spec command is recorded and submitted, and the
    // validation layer's message reaches the captured log.
    engine.settle(Duration::from_millis(600));

    let errors = engine.validation_errors();
    assert!(
        !errors.is_empty(),
        "the planted validation error was NOT detected — the gate is silently disabled \
         (wrong messenger prefix, or the validation layer is not enabled). \
         A green here means every other test's `validation_errors() == []` proves nothing."
    );
    // The planted error is the zero-width viewport VUID, not some unrelated incidental issue.
    assert!(
        errors
            .iter()
            .any(|line| line.contains("VUID-VkViewport-width-01770")),
        "expected the planted zero-width-viewport VUID, saw:\n{}",
        errors.join("\n")
    );

    engine.shutdown();
}
