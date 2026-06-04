/// Field-kind dispatcher: maps a `(component, field, value)` to a typed widget.
/// There is deliberately NO per-component switch — the panel iterates whatever
/// `inspect` returns and this resolver picks a widget by (1) the explicit
/// `FIELD_HINTS` parity table for the known components, else (2) the value's shape
/// ({x,y,z}->vec3, {x,y,z,w}->vec4, number/boolean/string), else (3) a read-only
/// text fallback so an unmapped field is still visible. So a future engine-side
/// `registerComponent` surfaces with no edit here beyond an optional hint.
///
/// Units (the 57x bug guard): ONLY `Transform.rotation` converts — UI shows degrees,
/// the wire carries radians — driven by the `convertRadians` hint. SpotLight
/// innerAngle/outerAngle are degrees on BOTH sides (no conversion); their `unit:"deg"`
/// is just a label/clamp. The widget value passed in/out of `renderField` is always
/// already in the WIRE unit (radians for rotation); conversion happens at the widget
/// boundary inside this file.
import { NumberDrag } from "./NumberDrag";
import { SliderField } from "./SliderField";
import { VectorEditor } from "./VectorEditor";
import { ColorField } from "./ColorField";
import { AssetPicker } from "./AssetPicker";
import { Switch } from "@/components/ui/switch";
import { Input } from "@/components/ui/input";

export type FieldKind =
  | "vec3"
  | "vec4"
  | "color3"
  | "color4"
  | "number"
  | "slider"
  | "bool"
  | "text"
  | "uuid";

/// Asset kind a `uuid` field references (phase-7's AssetPicker filters by this).
export type AssetKind = "mesh" | "texture";

export interface FieldHint {
  kind: FieldKind;
  min?: number;
  max?: number;
  step?: number;
  /// Degree semantics: `convertRadians` true converts UI<->wire (Transform.rotation
  /// only). `unit:"deg"` is a display label/clamp with NO conversion (spot angles).
  unit?: "deg";
  convertRadians?: boolean;
  /// For `uuid` fields: which asset catalog the phase-7 picker filters to.
  asset?: AssetKind;
}

/// The explicit parity contract mirroring the C++ widgets in
/// `registerBuiltinComponents` (editor_components.cpp). Keyed `Component.field`.
/// Anything not listed falls back to value-shape inference.
export const FIELD_HINTS: Record<string, FieldHint> = {
  "Name.name": { kind: "text" },

  "Transform.translation": { kind: "vec3", step: 0.05 },
  "Transform.scale": { kind: "vec3", step: 0.05 },
  // Edited in DEGREES, stored/serialized in RADIANS.
  "Transform.rotation": { kind: "vec3", step: 0.5, unit: "deg", convertRadians: true },

  "Mesh.mesh": { kind: "uuid", asset: "mesh" },

  "Camera.fov": { kind: "number", min: 1, max: 179, step: 0.5 },
  "Camera.near": { kind: "number", min: 0.001, step: 0.01 },
  "Camera.far": { kind: "number", min: 0.1, step: 1 },
  "Camera.primary": { kind: "bool" },

  "Material.baseColor": { kind: "color4" },
  "Material.albedoTexture": { kind: "uuid", asset: "texture" },
  "Material.metallic": { kind: "slider", min: 0, max: 1, step: 0.01 },
  "Material.roughness": { kind: "slider", min: 0, max: 1, step: 0.01 },
  "Material.emissive": { kind: "color3" },
  "Material.emissiveStrength": { kind: "number", min: 0, max: 100, step: 0.05 },
  "Material.unlit": { kind: "bool" },

  "DirectionalLight.direction": { kind: "vec3", step: 0.01 },
  "DirectionalLight.color": { kind: "color3" },
  "DirectionalLight.intensity": { kind: "number", min: 0, max: 50, step: 0.05 },
  "DirectionalLight.ambient": { kind: "number", min: 0, max: 1, step: 0.01 },

  "PointLight.color": { kind: "color3" },
  "PointLight.intensity": { kind: "number", min: 0, max: 100, step: 0.05 },
  "PointLight.range": { kind: "number", min: 0, max: 200, step: 0.1 },

  "SpotLight.direction": { kind: "vec3", step: 0.01 },
  "SpotLight.color": { kind: "color3" },
  "SpotLight.intensity": { kind: "number", min: 0, max: 100, step: 0.05 },
  "SpotLight.range": { kind: "number", min: 0, max: 200, step: 0.1 },
  // Degrees on BOTH sides — unit:"deg" is label/clamp only, NO conversion.
  "SpotLight.innerAngle": { kind: "number", min: 0, max: 89, step: 0.1, unit: "deg" },
  "SpotLight.outerAngle": { kind: "number", min: 0, max: 89, step: 0.1, unit: "deg" },

  "ReflectionProbe.influenceRadius": { kind: "number", min: 0.1, max: 500, step: 0.1 },
  "ReflectionProbe.intensity": { kind: "number", min: 0, max: 8, step: 0.01 },
  "ReflectionProbe.boxProjection": { kind: "bool" },
  "ReflectionProbe.boxExtent": { kind: "vec3", step: 0.1 },
};

const RAD_TO_DEG = 180 / Math.PI;
const DEG_TO_RAD = Math.PI / 180;

function isVec3(v: unknown): v is Record<string, number> {
  return typeof v === "object" && v !== null && "x" in v && "y" in v && "z" in v && !("w" in v);
}

function isVec4(v: unknown): v is Record<string, number> {
  return typeof v === "object" && v !== null && "x" in v && "y" in v && "z" in v && "w" in v;
}

/// Infer a kind from the value shape when no FIELD_HINTS entry exists. Keeps an
/// unmapped (e.g. newly added) field renderable instead of dropped.
function inferKind(value: unknown): FieldKind {
  if (isVec4(value)) {
    return "vec4";
  }
  if (isVec3(value)) {
    return "vec3";
  }
  if (typeof value === "number") {
    return "number";
  }
  if (typeof value === "boolean") {
    return "bool";
  }
  return "text";
}

export function resolveHint(component: string, field: string, value: unknown): FieldHint {
  const hint = FIELD_HINTS[`${component}.${field}`];
  if (hint) {
    return hint;
  }
  return { kind: inferKind(value) };
}

export interface FieldRenderContext {
  /// Drag bracket so the panel can gate the reconcile poll off mid-scrub.
  onDragStart(): void;
  onDragEnd(): void;
}

/// Render one field's widget. `value` is the raw wire value (rotation in radians).
/// `onChange(next)` receives the new WIRE value (rotation already converted back to
/// radians here), ready for the panel's read-modify-write.
export function renderField(
  component: string,
  field: string,
  value: unknown,
  onChange: (next: unknown) => void,
  ctx: FieldRenderContext,
): React.ReactElement {
  const hint = resolveHint(component, field, value);

  switch (hint.kind) {
    case "vec3":
    case "vec4": {
      const axes =
        hint.kind === "vec4" ? (["x", "y", "z", "w"] as const) : (["x", "y", "z"] as const);
      const wire = (value ?? {}) as Record<string, number>;
      // Display in degrees only for the converting hint (Transform.rotation).
      const display: Record<string, number> = hint.convertRadians
        ? Object.fromEntries(axes.map((a) => [a, (wire[a] ?? 0) * RAD_TO_DEG]))
        : wire;
      return (
        <VectorEditor
          axes={axes}
          value={display}
          step={hint.step}
          onChange={(axis, v) => {
            const wireValue = hint.convertRadians ? v * DEG_TO_RAD : v;
            onChange({ ...wire, [axis]: wireValue });
          }}
          onDragStart={ctx.onDragStart}
          onDragEnd={ctx.onDragEnd}
        />
      );
    }

    case "color3":
    case "color4": {
      const wire = (value ?? {}) as Record<string, number>;
      return (
        <ColorField
          kind={hint.kind}
          value={wire}
          onChange={(axis, v) => onChange({ ...wire, [axis]: v })}
          onDragStart={ctx.onDragStart}
          onDragEnd={ctx.onDragEnd}
        />
      );
    }

    case "slider":
      return (
        <SliderField
          value={typeof value === "number" ? value : 0}
          min={hint.min ?? 0}
          max={hint.max ?? 1}
          step={hint.step ?? 0.01}
          onChange={(v) => onChange(v)}
          onDragStart={ctx.onDragStart}
          onDragEnd={ctx.onDragEnd}
        />
      );

    case "number":
      return (
        <NumberDrag
          value={typeof value === "number" ? value : 0}
          min={hint.min}
          max={hint.max}
          step={hint.step}
          onChange={(v) => onChange(v)}
          onDragStart={ctx.onDragStart}
          onDragEnd={ctx.onDragEnd}
        />
      );

    case "bool":
      return <Switch checked={value === true} onCheckedChange={(checked) => onChange(checked)} />;

    case "uuid":
      // A Uuid field → the thumbnail combo + drag-drop target. The asset catalog
      // it filters to comes from the hint (`asset: "mesh" | "texture"`); a Uuid
      // field with no hint defaults to texture (the common case beyond Mesh.mesh).
      return (
        <AssetPicker
          value={typeof value === "string" ? value : "0"}
          assetType={hint.asset ?? "texture"}
          onChange={(v) => onChange(v)}
        />
      );

    case "text":
    default:
      return (
        <Input
          type="text"
          className="h-7 rounded-sm bg-background px-1.5 py-0.5 font-mono text-[11px]"
          value={typeof value === "string" ? value : JSON.stringify(value)}
          onChange={(event) => onChange(event.currentTarget.value)}
        />
      );
  }
}
