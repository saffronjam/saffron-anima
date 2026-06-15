import { describe, expect, test } from "bun:test";
import { humanizeComponentName, humanizeFieldName } from "./humanize";

describe("humanizeFieldName", () => {
  test("renders camel-case fields as sentence-case labels", () => {
    expect(humanizeFieldName("albedoTexture")).toBe("Albedo texture");
    expect(humanizeFieldName("emissiveStrength")).toBe("Emissive strength");
  });

  test("preserves known abbreviations", () => {
    expect(humanizeFieldName("modelID")).toBe("Model ID");
    expect(humanizeFieldName("ormTexture")).toBe("ORM texture");
  });
});

describe("humanizeComponentName", () => {
  test("renders PascalCase component names as sentence-case labels", () => {
    expect(humanizeComponentName("AnimationPlayer")).toBe("Animation player");
    expect(humanizeComponentName("ModelInstance")).toBe("Model instance");
    expect(humanizeComponentName("SkinnedMesh")).toBe("Skinned mesh");
  });

  test("splits acronym-leading component names without lowercasing the acronym", () => {
    expect(humanizeComponentName("DDGIProbe")).toBe("DDGI probe");
  });
});
