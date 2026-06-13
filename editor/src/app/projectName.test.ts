import { describe, expect, test } from "bun:test";
import { validProjectName } from "./ProjectStartupModal";

// The naming contract (ProjectStartupModal.tsx):
//   length must be 1..=63, and the whole string must match
//   /^[a-z0-9](?:[a-z0-9-]*[a-z0-9])?$/ — i.e. lowercase letters, digits, and
//   hyphens, starting and ending with a letter or digit.

describe("validProjectName — accepted names", () => {
  test("a single lowercase letter", () => {
    expect(validProjectName("a")).toBe(true);
  });

  test("a single digit", () => {
    expect(validProjectName("7")).toBe(true);
  });

  test("the placeholder hyphenated name", () => {
    expect(validProjectName("a-name-like-this")).toBe(true);
  });

  test("digits mixed with letters", () => {
    expect(validProjectName("level2")).toBe(true);
    expect(validProjectName("2nd-level")).toBe(true);
  });

  test("starts with a digit, ends with a letter", () => {
    expect(validProjectName("3d-scene")).toBe(true);
  });

  test("a two-character name (letter then digit)", () => {
    expect(validProjectName("a0")).toBe(true);
  });

  test("name of exactly the 63-char maximum", () => {
    expect(validProjectName("a".repeat(63))).toBe(true);
  });

  test("63 chars with interior hyphens", () => {
    // 'a' + 61 hyphens + 'z' = 63 chars, valid start/end.
    expect(validProjectName(`a${"-".repeat(61)}z`)).toBe(true);
  });
});

describe("validProjectName — rejected by length", () => {
  test("the empty string", () => {
    expect(validProjectName("")).toBe(false);
  });

  test("one character over the 63-char maximum", () => {
    expect(validProjectName("a".repeat(64))).toBe(false);
  });

  test("a very long name", () => {
    expect(validProjectName("a".repeat(200))).toBe(false);
  });
});

describe("validProjectName — rejected by case", () => {
  test("an uppercase letter anywhere", () => {
    expect(validProjectName("MyProject")).toBe(false);
    expect(validProjectName("myProject")).toBe(false);
    expect(validProjectName("project-Name")).toBe(false);
  });

  test("a single uppercase letter", () => {
    expect(validProjectName("A")).toBe(false);
  });
});

describe("validProjectName — rejected by hyphen position", () => {
  test("a leading hyphen", () => {
    expect(validProjectName("-name")).toBe(false);
  });

  test("a trailing hyphen", () => {
    expect(validProjectName("name-")).toBe(false);
  });

  test("a lone hyphen", () => {
    expect(validProjectName("-")).toBe(false);
  });

  test("leading and trailing hyphens together", () => {
    expect(validProjectName("-name-")).toBe(false);
  });

  test("interior consecutive hyphens are still allowed", () => {
    expect(validProjectName("a--b")).toBe(true);
  });
});

describe("validProjectName — rejected by whitespace", () => {
  test("a single space", () => {
    expect(validProjectName(" ")).toBe(false);
  });

  test("a space inside the name", () => {
    expect(validProjectName("a name")).toBe(false);
  });

  test("surrounding whitespace is not trimmed away", () => {
    expect(validProjectName(" name")).toBe(false);
    expect(validProjectName("name ")).toBe(false);
  });

  test("a tab character", () => {
    expect(validProjectName("a\tb")).toBe(false);
  });

  test("a newline inside the name", () => {
    expect(validProjectName("a\nb")).toBe(false);
  });

  test("a trailing newline (anchors are not multiline)", () => {
    // A trailing \n must be rejected: $ in a non-multiline regex would otherwise
    // tolerate it, but the function uses .test on the whole string.
    expect(validProjectName("name\n")).toBe(false);
  });
});

describe("validProjectName — rejected by path separators", () => {
  test("a forward slash", () => {
    expect(validProjectName("a/b")).toBe(false);
    expect(validProjectName("dir/project")).toBe(false);
  });

  test("a backslash", () => {
    expect(validProjectName("a\\b")).toBe(false);
  });

  test("a leading slash (absolute-path shape)", () => {
    expect(validProjectName("/name")).toBe(false);
  });

  test("a dot-segment traversal", () => {
    expect(validProjectName("..")).toBe(false);
    expect(validProjectName("../escape")).toBe(false);
  });
});

describe("validProjectName — rejected by disallowed characters", () => {
  test("a dot", () => {
    expect(validProjectName("name.json")).toBe(false);
    expect(validProjectName("a.b")).toBe(false);
  });

  test("an underscore", () => {
    expect(validProjectName("my_project")).toBe(false);
  });

  test("other punctuation", () => {
    for (const ch of ["@", "#", "!", "*", "?", ":", ";", ",", "(", ")", "+", "="]) {
      expect(validProjectName(`name${ch}`)).toBe(false);
    }
  });

  test("non-ASCII letters", () => {
    expect(validProjectName("café")).toBe(false);
    expect(validProjectName("naïve")).toBe(false);
  });
});
