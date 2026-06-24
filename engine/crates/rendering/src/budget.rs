//! The frame-budget controller: auto-steps the render-quality tier to hold the frame budget.
//!
//! The engine already measures per-frame GPU+CPU work time; this turns that measurement into a
//! control signal. When auto-quality is on (`PerfConfig::auto_quality`), the controller watches the
//! frame work time against the budget (`1000 / target_fps`) and, after a sustained run of
//! over-budget frames, steps the [`QualityTier`] down (cheaper screen-space GI); after a sustained
//! run with comfortable headroom it steps back up. Hysteresis (consecutive-frame thresholds + a
//! cooldown after each switch) keeps it from oscillating, and it never auto-selects `Ultra` (a
//! deliberate stills/screenshot tier) or drops below `Low`.
//!
//! It reuses the Phase-3 tier system as its actuator — a step is just a `set_render_quality` — so it
//! adds no new render path. The dynamic-*resolution* variant (scaling the offscreen extent) is a
//! deeper, separate change; this tier-stepping controller is the safe, self-contained core.

use crate::quality::QualityTier;

/// Consecutive over-budget frames before stepping the tier down.
const OVER_BUDGET_STEP: u32 = 12;
/// Consecutive comfortably-under-budget frames before stepping the tier up.
const UNDER_BUDGET_STEP: u32 = 90;
/// A single frame this many times the budget forces an immediate downstep (a "panic" hitch).
const PANIC_MULTIPLE: f32 = 2.0;
/// Headroom required to consider stepping up: work below this fraction of budget.
const UPSTEP_HEADROOM_FRAC: f32 = 0.7;
/// Frames to wait after a switch before another, so a step's effect is measured before the next.
const COOLDOWN_FRAMES: u32 = 30;

/// The auto-quality tier ladder, cheapest first. `Ultra` is excluded — it is an explicit
/// stills/screenshot tier, never auto-selected.
const LADDER: [QualityTier; 3] = [QualityTier::Low, QualityTier::Medium, QualityTier::High];

/// The rolling state of the frame-budget controller.
#[derive(Clone, Copy, Debug, Default)]
pub struct BudgetController {
    over: u32,
    under: u32,
    cooldown: u32,
}

impl BudgetController {
    /// A fresh controller.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// Folds one frame's work time (ms) against the budget (ms) and returns the tier to switch to,
    /// or `None` to hold. `current` is the active tier. A non-positive `budget_ms` (uncapped) or a
    /// `current` tier off the auto ladder (e.g. `Ultra`/`Custom`) holds.
    pub fn update(
        &mut self,
        frame_ms: f32,
        budget_ms: f32,
        current: QualityTier,
    ) -> Option<QualityTier> {
        if budget_ms <= 0.0 {
            return None;
        }
        let rung = LADDER.iter().position(|&t| t == current)?;

        if self.cooldown > 0 {
            self.cooldown -= 1;
        }

        // Panic: a single very-late frame steps down at once (bypassing the streak + cooldown).
        if frame_ms > budget_ms * PANIC_MULTIPLE && rung > 0 {
            return Some(self.commit(LADDER[rung - 1]));
        }

        if frame_ms > budget_ms {
            self.over += 1;
            self.under = 0;
        } else if frame_ms < budget_ms * UPSTEP_HEADROOM_FRAC {
            self.under += 1;
            self.over = 0;
        } else {
            // In the comfortable band around budget — neither streak advances.
            self.over = 0;
            self.under = 0;
        }

        if self.cooldown > 0 {
            return None;
        }
        if self.over >= OVER_BUDGET_STEP && rung > 0 {
            return Some(self.commit(LADDER[rung - 1]));
        }
        if self.under >= UNDER_BUDGET_STEP && rung + 1 < LADDER.len() {
            return Some(self.commit(LADDER[rung + 1]));
        }
        None
    }

    /// Records a committed switch: reset the streaks and arm the cooldown.
    fn commit(&mut self, tier: QualityTier) -> QualityTier {
        self.over = 0;
        self.under = 0;
        self.cooldown = COOLDOWN_FRAMES;
        tier
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn sustained_over_budget_steps_down_one_rung() {
        let mut c = BudgetController::new();
        // Just under the panic multiple, so it takes the streak path.
        for _ in 0..OVER_BUDGET_STEP - 1 {
            assert_eq!(c.update(20.0, 16.0, QualityTier::High), None);
        }
        assert_eq!(
            c.update(20.0, 16.0, QualityTier::High),
            Some(QualityTier::Medium),
            "steps High → Medium after the over-budget streak"
        );
    }

    #[test]
    fn a_panic_frame_steps_down_immediately() {
        let mut c = BudgetController::new();
        assert_eq!(
            c.update(40.0, 16.0, QualityTier::Medium),
            Some(QualityTier::Low),
            "a >2× budget frame steps down at once"
        );
    }

    #[test]
    fn sustained_headroom_steps_up_then_cooldown_holds() {
        let mut c = BudgetController::new();
        // Comfortable headroom for the up-step streak.
        for _ in 0..UNDER_BUDGET_STEP - 1 {
            assert_eq!(c.update(5.0, 16.0, QualityTier::Low), None);
        }
        assert_eq!(
            c.update(5.0, 16.0, QualityTier::Low),
            Some(QualityTier::Medium),
            "steps Low → Medium after the headroom streak"
        );
        // The cooldown now holds even with continued headroom.
        assert_eq!(
            c.update(5.0, 16.0, QualityTier::Medium),
            None,
            "cooldown holds"
        );
    }

    #[test]
    fn never_steps_below_low_or_above_high() {
        let mut c = BudgetController::new();
        // Already at Low, panic frame: nothing below Low.
        assert_eq!(c.update(100.0, 16.0, QualityTier::Low), None);
        // At High with endless headroom: never auto-climbs to Ultra.
        let mut c = BudgetController::new();
        for _ in 0..UNDER_BUDGET_STEP * 2 {
            assert_eq!(c.update(1.0, 16.0, QualityTier::High), None);
        }
    }

    #[test]
    fn uncapped_or_off_ladder_holds() {
        let mut c = BudgetController::new();
        assert_eq!(
            c.update(100.0, 0.0, QualityTier::High),
            None,
            "uncapped holds"
        );
        assert_eq!(
            c.update(100.0, 16.0, QualityTier::Ultra),
            None,
            "Ultra is off the auto ladder"
        );
        assert_eq!(
            c.update(100.0, 16.0, QualityTier::Custom),
            None,
            "Custom is off the auto ladder"
        );
    }
}
