/// The keybinding registry: every rebindable editor command, its default key, and
/// the parse/match/format helpers the handlers and the settings modal share. The
/// resolved overrides live in the editor store (`keyBindings`, deltas only —
/// settings.json stores just the changed commands, VS Code-style); handlers call
/// `matchesBinding(event, id, overrides)` instead of comparing key literals.
///
/// Two command kinds:
/// - "press": one-shot commands matched on a normalized key-string built from
///   `event.key` plus modifier prefixes in fixed order ("w", "shift+f", "escape").
///   Matching is exact-modifier: a binding of "f" does not fire on Ctrl+F, so
///   menu/OS chords pass through untouched unless explicitly bound.
/// - "hold": held-state fly-camera keys matched on the physical `event.code`
///   ("KeyW", "Space", "ShiftLeft"), no modifier combos.

export type CommandKind = "press" | "hold";

/// Conflict scope: bindings only collide within one scope. Global press commands
/// share one window listener; fly keys share the viewport fly listener; the
/// hierarchy/assets deletes are focus-scoped to their own panels, so the same key
/// in both is fine.
export type CommandScope = "global" | "hierarchy" | "assets" | "fly";

export type CommandId =
  | "gizmo.translate"
  | "gizmo.rotate"
  | "gizmo.scale"
  | "camera.focus"
  | "selection.deselect"
  | "hierarchy.delete"
  | "assets.delete"
  | "camera.flyForward"
  | "camera.flyBack"
  | "camera.flyLeft"
  | "camera.flyRight"
  | "camera.flyUp"
  | "camera.flyDown";

export interface CommandDef {
  id: CommandId;
  label: string;
  category: string;
  kind: CommandKind;
  /// Key-string for "press" commands, `event.code` for "hold" commands.
  default: string;
  scope: CommandScope;
}

/// Registry, in display order for the settings modal.
export const COMMANDS: readonly CommandDef[] = [
  {
    id: "gizmo.translate",
    label: "Translate gizmo",
    category: "Gizmo",
    kind: "press",
    default: "w",
    scope: "global",
  },
  {
    id: "gizmo.rotate",
    label: "Rotate gizmo",
    category: "Gizmo",
    kind: "press",
    default: "e",
    scope: "global",
  },
  {
    id: "gizmo.scale",
    label: "Scale gizmo",
    category: "Gizmo",
    kind: "press",
    default: "r",
    scope: "global",
  },
  {
    id: "camera.focus",
    label: "Focus selection",
    category: "Camera",
    kind: "press",
    default: "f",
    scope: "global",
  },
  {
    id: "selection.deselect",
    label: "Deselect",
    category: "Selection",
    kind: "press",
    default: "escape",
    scope: "global",
  },
  {
    id: "hierarchy.delete",
    label: "Delete entity",
    category: "Hierarchy",
    kind: "press",
    default: "delete",
    scope: "hierarchy",
  },
  {
    id: "assets.delete",
    label: "Delete asset / folder",
    category: "Assets",
    kind: "press",
    default: "delete",
    scope: "assets",
  },
  {
    id: "camera.flyForward",
    label: "Fly forward",
    category: "Fly camera",
    kind: "hold",
    default: "KeyW",
    scope: "fly",
  },
  {
    id: "camera.flyBack",
    label: "Fly back",
    category: "Fly camera",
    kind: "hold",
    default: "KeyS",
    scope: "fly",
  },
  {
    id: "camera.flyLeft",
    label: "Fly left",
    category: "Fly camera",
    kind: "hold",
    default: "KeyA",
    scope: "fly",
  },
  {
    id: "camera.flyRight",
    label: "Fly right",
    category: "Fly camera",
    kind: "hold",
    default: "KeyD",
    scope: "fly",
  },
  {
    id: "camera.flyUp",
    label: "Fly up",
    category: "Fly camera",
    kind: "hold",
    default: "Space",
    scope: "fly",
  },
  {
    id: "camera.flyDown",
    label: "Fly down",
    category: "Fly camera",
    kind: "hold",
    default: "ShiftLeft",
    scope: "fly",
  },
];

export const COMMANDS_BY_ID: Record<CommandId, CommandDef> = Object.fromEntries(
  COMMANDS.map((def) => [def.id, def]),
) as Record<CommandId, CommandDef>;

/// True when `value` names a registered command (filters stale settings.json keys).
export function isCommandId(value: string): value is CommandId {
  return value in COMMANDS_BY_ID;
}

interface KeyEventLike {
  key: string;
  code: string;
  ctrlKey: boolean;
  shiftKey: boolean;
  altKey: boolean;
  metaKey: boolean;
}

const MODIFIER_KEYS = new Set(["Control", "Shift", "Alt", "Meta"]);

/// Normalize a keydown into a press key-string ("shift+f"), or null when the event
/// carries no main key (a pure-modifier press, e.g. Shift alone).
export function normalizePressEvent(event: KeyEventLike): string | null {
  if (MODIFIER_KEYS.has(event.key)) {
    return null;
  }
  let key = event.key.toLowerCase();
  if (key === " ") {
    key = "space";
  }
  let prefix = "";
  if (event.ctrlKey) {
    prefix += "ctrl+";
  }
  if (event.shiftKey) {
    prefix += "shift+";
  }
  if (event.altKey) {
    prefix += "alt+";
  }
  if (event.metaKey) {
    prefix += "meta+";
  }
  return prefix + key;
}

/// The effective binding for a command: the user override, else the default.
export function bindingFor(id: CommandId, overrides: Record<string, string>): string {
  return overrides[id] ?? COMMANDS_BY_ID[id].default;
}

/// True when the keydown matches the command's effective binding. Press commands
/// compare the normalized key-string (exact modifier set); hold commands compare
/// the physical `event.code`.
export function matchesBinding(
  event: KeyEventLike,
  id: CommandId,
  overrides: Record<string, string>,
): boolean {
  const binding = bindingFor(id, overrides);
  if (COMMANDS_BY_ID[id].kind === "hold") {
    return event.code === binding;
  }
  return normalizePressEvent(event) === binding;
}

const PRESS_KEY_LABELS: Record<string, string> = {
  escape: "Esc",
  delete: "Delete",
  backspace: "Backspace",
  space: "Space",
  enter: "Enter",
  tab: "Tab",
  arrowup: "Up",
  arrowdown: "Down",
  arrowleft: "Left",
  arrowright: "Right",
};

const MODIFIER_LABELS: Record<string, string> = {
  ctrl: "Ctrl",
  shift: "Shift",
  alt: "Alt",
  meta: "Meta",
};

/// Display label for a physical `event.code`: "KeyW" → "W", "Digit3" → "3",
/// "ShiftLeft" → "Left Shift", anything else verbatim.
function formatCode(code: string): string {
  if (code.startsWith("Key") && code.length === 4) {
    return code.slice(3);
  }
  if (code.startsWith("Digit") && code.length === 6) {
    return code.slice(5);
  }
  const side = code.match(/^(Shift|Control|Alt|Meta)(Left|Right)$/);
  if (side) {
    return `${side[2]} ${side[1] === "Control" ? "Ctrl" : side[1]}`;
  }
  return code;
}

/// Human-readable form of a binding value for chips and tooltips:
/// "shift+f" → "Shift+F", "escape" → "Esc", "KeyW" (hold) → "W".
export function formatBinding(def: CommandDef, value: string): string {
  if (def.kind === "hold") {
    return formatCode(value);
  }
  const parts = value.split("+");
  const key = parts[parts.length - 1];
  const mods = parts.slice(0, -1).map((mod) => MODIFIER_LABELS[mod] ?? mod);
  const keyLabel = PRESS_KEY_LABELS[key] ?? (key.length === 1 ? key.toUpperCase() : key);
  return [...mods, keyLabel].join("+");
}

/// The command (if any) whose effective binding already equals `candidate` within
/// the same conflict scope as `forId`, excluding `forId` itself.
export function findConflict(
  forId: CommandId,
  candidate: string,
  overrides: Record<string, string>,
): CommandId | null {
  const scope = COMMANDS_BY_ID[forId].scope;
  for (const def of COMMANDS) {
    if (def.id === forId || def.scope !== scope) {
      continue;
    }
    if (bindingFor(def.id, overrides) === candidate) {
      return def.id;
    }
  }
  return null;
}
