import { describe, expect, test } from "bun:test";
import {
  type CommandId,
  COMMANDS,
  COMMANDS_BY_ID,
  bindingFor,
  findConflict,
  formatBinding,
  isCommandId,
  matchesBinding,
  mouseCommandFor,
  mouseToken,
  normalizePressEvent,
} from "./keybindings";

interface KeyEventLike {
  key: string;
  code: string;
  ctrlKey: boolean;
  shiftKey: boolean;
  altKey: boolean;
  metaKey: boolean;
}

/// Build a KeyEventLike with sensible defaults so individual cases only set the
/// fields they care about.
function ev(over: Partial<KeyEventLike>): KeyEventLike {
  return {
    key: "",
    code: "",
    ctrlKey: false,
    shiftKey: false,
    altKey: false,
    metaKey: false,
    ...over,
  };
}

describe("normalizePressEvent", () => {
  test("a pure-modifier press returns null", () => {
    expect(normalizePressEvent(ev({ key: "Shift" }))).toBeNull();
    expect(normalizePressEvent(ev({ key: "Control" }))).toBeNull();
    expect(normalizePressEvent(ev({ key: "Alt" }))).toBeNull();
    expect(normalizePressEvent(ev({ key: "Meta" }))).toBeNull();
  });

  test("lowercases the main key and prefixes a held shift", () => {
    expect(normalizePressEvent(ev({ key: "F", shiftKey: true }))).toBe("shift+f");
  });

  test("a bare letter has no modifier prefix", () => {
    expect(normalizePressEvent(ev({ key: "W" }))).toBe("w");
  });

  test("the space key maps to the 'space' token", () => {
    expect(normalizePressEvent(ev({ key: " " }))).toBe("space");
  });

  test("modifiers serialize in fixed ctrl+shift+alt+meta order", () => {
    expect(
      normalizePressEvent(
        ev({ key: "f", ctrlKey: true, shiftKey: true, altKey: true, metaKey: true }),
      ),
    ).toBe("ctrl+shift+alt+meta+f");
  });

  test("only ctrl set yields a single ctrl prefix", () => {
    expect(normalizePressEvent(ev({ key: "z", ctrlKey: true }))).toBe("ctrl+z");
  });
});

describe("bindingFor", () => {
  test("falls to the registered default with no override", () => {
    expect(bindingFor("camera.focus", {})).toBe("f");
    expect(bindingFor("camera.flyForward", {})).toBe("KeyW");
  });

  test("prefers a user override over the default", () => {
    expect(bindingFor("camera.focus", { "camera.focus": "g" })).toBe("g");
  });
});

describe("matchesBinding", () => {
  test("a hold command matches on event.code", () => {
    // KeyW is the fly-forward physical code.
    expect(matchesBinding(ev({ key: "w", code: "KeyW" }), "camera.flyForward", {})).toBe(true);
  });

  test("a hold command ignores event.key (a press 'w' does not match)", () => {
    // No code set: a press-style "w" must not fire the held fly key.
    expect(matchesBinding(ev({ key: "w", code: "" }), "camera.flyForward", {})).toBe(false);
  });

  test("a press command matches its exact key-string", () => {
    expect(matchesBinding(ev({ key: "f", code: "KeyF" }), "camera.focus", {})).toBe(true);
  });

  test("a press 'f' does NOT fire when ctrl is held (exact-modifier match)", () => {
    expect(matchesBinding(ev({ key: "f", code: "KeyF", ctrlKey: true }), "camera.focus", {})).toBe(
      false,
    );
  });

  test("a press command honors an override binding", () => {
    expect(
      matchesBinding(ev({ key: "g", code: "KeyG" }), "camera.focus", { "camera.focus": "g" }),
    ).toBe(true);
    expect(
      matchesBinding(ev({ key: "f", code: "KeyF" }), "camera.focus", { "camera.focus": "g" }),
    ).toBe(false);
  });
});

describe("findConflict", () => {
  test("a same-scope collision returns the conflicting command id", () => {
    // Rebinding gizmo.rotate (global) to "f" collides with camera.focus (global, default "f").
    expect(findConflict("gizmo.rotate", "f", {})).toBe("camera.focus");
  });

  test("no conflict for a free key in the same scope", () => {
    expect(findConflict("gizmo.rotate", "p", {})).toBeNull();
  });

  test("the same key in a different scope does NOT conflict", () => {
    // hierarchy.delete and assets.delete both default to "delete" but live in different scopes.
    expect(findConflict("hierarchy.delete", "delete", {})).toBeNull();
  });

  test("does not report the command against itself", () => {
    // camera.focus already binds "f"; checking "f" for itself is not a conflict.
    expect(findConflict("camera.focus", "f", {})).toBeNull();
  });

  test("respects overrides when detecting a same-scope collision", () => {
    // gizmo.rotate is overridden to "f", so binding camera.focus -> "f" collides with it.
    const conflict = findConflict("camera.focus", "f", { "gizmo.rotate": "f" });
    expect(conflict).toBe("gizmo.rotate");
  });
});

describe("formatBinding", () => {
  test("hold: a KeyW code shows the bare letter", () => {
    expect(formatBinding(COMMANDS_BY_ID["camera.flyForward"], "KeyW")).toBe("W");
  });

  test("hold: a sided modifier code shows 'Left Shift'", () => {
    expect(formatBinding(COMMANDS_BY_ID["camera.flyDown"], "ShiftLeft")).toBe("Left Shift");
  });

  test("press: 'shift+f' becomes 'Shift+F'", () => {
    expect(formatBinding(COMMANDS_BY_ID["camera.focus"], "shift+f")).toBe("Shift+F");
  });

  test("press: 'escape' becomes 'Esc'", () => {
    expect(formatBinding(COMMANDS_BY_ID["selection.deselect"], "escape")).toBe("Esc");
  });

  test("press: a single letter upper-cases", () => {
    expect(formatBinding(COMMANDS_BY_ID["gizmo.translate"], "w")).toBe("W");
  });

  test("press: 'ctrl+shift+z' becomes 'Ctrl+Shift+Z'", () => {
    expect(formatBinding(COMMANDS_BY_ID["edit.redo"], "ctrl+shift+z")).toBe("Ctrl+Shift+Z");
  });
});

describe("mouse bindings", () => {
  test("mouseToken builds the namespaced value", () => {
    expect(mouseToken("back")).toBe("mouse:back");
    expect(mouseToken("middle")).toBe("mouse:middle");
  });

  test("mouseCommandFor resolves the default-bound command", () => {
    expect(mouseCommandFor("mouse:back", {})).toBe("tab.navBack");
    expect(mouseCommandFor("mouse:forward", {})).toBe("tab.navForward");
    expect(mouseCommandFor("mouse:middle", {})).toBe("tab.close");
  });

  test("mouseCommandFor honors an override and returns null when unbound", () => {
    // Rebinding tab.navBack to the middle button makes it answer that token...
    const overrides = { "tab.navBack": "mouse:middle" };
    expect(mouseCommandFor("mouse:middle", overrides)).toBe("tab.navBack");
    // ...and nothing answers the back button anymore.
    expect(mouseCommandFor("mouse:back", overrides)).toBeNull();
    expect(mouseCommandFor("mouse:left", {})).toBeNull();
  });

  test("formatBinding labels a mouse token", () => {
    expect(formatBinding(COMMANDS_BY_ID["tab.navBack"], "mouse:back")).toBe("Back button");
    expect(formatBinding(COMMANDS_BY_ID["tab.close"], "mouse:middle")).toBe("Middle button");
  });

  test("mouse commands only conflict within the tabs scope", () => {
    // Rebinding tab.close to the back button collides with tab.navBack (both 'tabs' scope).
    expect(findConflict("tab.close", "mouse:back", {})).toBe("tab.navBack");
    // A key default in another scope is never flagged against a mouse token.
    expect(findConflict("tab.navBack", "mouse:back", {})).toBeNull();
  });
});

describe("registry helpers", () => {
  test("isCommandId accepts a registered id and rejects junk", () => {
    expect(isCommandId("camera.focus")).toBe(true);
    expect(isCommandId("camera.bogus")).toBe(false);
  });

  test("COMMANDS_BY_ID indexes every entry in COMMANDS", () => {
    for (const def of COMMANDS) {
      expect(COMMANDS_BY_ID[def.id as CommandId]).toBe(def);
    }
  });
});
