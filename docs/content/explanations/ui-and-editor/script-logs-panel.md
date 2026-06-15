+++
title = 'Script logs panel'
weight = 9
+++

# Script logs panel

The Script Logs panel is the play-mode window into `sa.log` output from gameplay scripts. It sits in the diagnostics group beside the [Physics panel](../physics-panel/) and [Stats and the Profiler](../metrics-dashboard/) (open it from the Tools → Diagnostics menu) and docks by default in the bottom **Assets and Timeline** group. While the scene is Playing it streams each script's log lines as `<time> [<entity>] <message>` rows; in Edit it shows an empty state and polls nothing — a closed panel or a panel in Edit makes zero control round-trips.

`sa.log(message)` from a script is captured on the engine side into a bounded ring on the edit context, tagged with the entity whose handler is running (`ScriptHost::currentSenderUuid`) and a wall-clock timestamp. The line is *also* written to the engine log, but the panel is how you see it inside the editor. Because the bridge is a host-bound `std::function` (`ScriptHost::logSink`, wired in `host.cppm`), `Saffron.Script` never imports `Saffron.SceneEdit` — the same POD-bridge pattern the physics bindings use.

## Telemetry, gated to matter

The reconcile poll drains logs only when the panel is **open and play is active** — scripts run only during play:

```ts
if (isPanelOpen(state, "scriptLogs") && state.playState !== "edit") {
  const drained = await client.drainScriptLogs(scriptLogsSince);
  appendScriptLogs(drained.events, drained.overflowed);
  scriptLogsSince = Math.max(scriptLogsSince, drained.highWaterSeq);
}
```

`drain-script-logs` returns the lines past a seq cursor with an `overflowed` flag (mirroring `drain-script-errors`/`drain-contacts`); the engine ring is bounded and the editor keeps a deeper newest-at-bottom window. The cursor and the editor buffer **reset on each fresh play**, and the buffer is **retained after Stop** so you can read what happened. The list is windowed — only the visible rows mount, so a long session stays smooth — and it auto-scrolls to the newest line unless you have scrolled up to read history. A wrapped ring surfaces a *lines-dropped* marker.

## Searching with typed verbs

The search bar is **AnimaSearchbar** — the first of the engine's generic `anima/` components, ported from the sibling `saffron-hive` project. Instead of a free-text box plus a row of filter buttons, you type a verb and the bar autocompletes it into a **chip**:

- Type `Entity:` and a dropdown lists the scene's entities; pick one and it becomes an `Entity: Robot` chip that filters the feed to that entity. Multiple `Entity:` chips OR-group (any of them).
- Anything that is not a chip is **free text**, matched case-insensitively against the message (AND-ed with the chips).

This collapses the "is this free text or a filter?" ambiguity into one bar. `Ctrl+F` focuses the search field while the panel has focus. The chip model (`parseQuery`/`serialize`/`SearchState`) is framework-agnostic and unit-tested; only the view is React.

## Code

| What | File | Symbols |
|---|---|---|
| The panel | `editor/src/panels/ScriptLogsPanel.tsx` | `ScriptLogsPanel`, the `Entity:` chip config, the windowed list, Ctrl+F |
| The search component | `editor/src/components/anima/` | `AnimaSearchbar`, `AnimaSearchField`, `chipSearch` (the model + test) |
| Registration | `editor/src/components/dock/panelRegistry.tsx` · `editor/src/state/dockLayout.ts` | the `scriptLogs` registry entry, `SCENE_PANEL_IDS`, `DEFAULT_LEAF` (`leaf:assets`) |
| Store state + poll | `editor/src/state/store.ts` | `scriptLogs`, `appendScriptLogs`, the open-AND-playing poll block + `scriptLogsSince` |
| Typed wrapper | `editor/src/control/client.ts` | `drainScriptLogs` |
| Engine capture + command | `sceneedit/scene_edit_context.cppm` · `scene_edit_play.cpp` · `script/script_runtime.cpp` · `host/host.cppm` · `control/control_commands_scene.cpp` | `ScriptLog` ring + `pushScriptLog`, the `sa.log` rebind, `ScriptHost::logSink`, `drain-script-logs` |
