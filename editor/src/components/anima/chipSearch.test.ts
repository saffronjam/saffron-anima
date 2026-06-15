import { describe, expect, it } from "bun:test";

import {
  emptySearchState,
  matchChipKeyword,
  parseQuery,
  serialize,
  stateToTokens,
  tokensToState,
  type SearchState,
} from "./chipSearch";

const KEYWORDS = ["type", "room"] as const;

describe("matchChipKeyword", () => {
  it("returns the keyword for a configured prefix", () => {
    expect(matchChipKeyword("type:light", KEYWORDS)).toBe("type");
    expect(matchChipKeyword("room:kitchen", KEYWORDS)).toBe("room");
  });
  it("returns null for an unconfigured prefix", () => {
    expect(matchChipKeyword("foo:bar", KEYWORDS)).toBeNull();
  });
  it("returns null when there is no colon", () => {
    expect(matchChipKeyword("type", KEYWORDS)).toBeNull();
  });
  it("returns null when the colon is at position 0", () => {
    expect(matchChipKeyword(":light", KEYWORDS)).toBeNull();
  });
  it("matches even when the value after the colon is empty", () => {
    expect(matchChipKeyword("type:", KEYWORDS)).toBe("type");
  });
  it("uses the first colon to split keyword and value", () => {
    expect(matchChipKeyword("type:a:b", KEYWORDS)).toBe("type");
  });
  it("is case-sensitive on the keyword", () => {
    expect(matchChipKeyword("Type:light", KEYWORDS)).toBeNull();
  });
  it("returns null when no keywords are configured", () => {
    expect(matchChipKeyword("type:light", [])).toBeNull();
  });
});

describe("serialize", () => {
  it("returns an empty string for an empty state", () => {
    expect(serialize(emptySearchState())).toBe("");
  });
  it("serializes free text only", () => {
    expect(serialize({ chips: [], freeText: "bedroom lamp" })).toBe("bedroom lamp");
  });
  it("serializes chips only", () => {
    expect(serialize({ chips: [{ keyword: "type", value: "light" }], freeText: "" })).toBe(
      "type:light",
    );
  });
  it("serializes chips and free text, chips first", () => {
    expect(
      serialize({
        chips: [
          { keyword: "type", value: "light" },
          { keyword: "type", value: "sensor" },
        ],
        freeText: "bed",
      }),
    ).toBe("type:light type:sensor bed");
  });
  it("preserves chips with empty value as `keyword:`", () => {
    expect(serialize({ chips: [{ keyword: "type", value: "" }], freeText: "" })).toBe("type:");
  });
});

describe("parseQuery", () => {
  it("returns an empty state for an empty query", () => {
    expect(parseQuery("", KEYWORDS)).toEqual(emptySearchState());
  });
  it("collapses whitespace-only input to empty state", () => {
    expect(parseQuery("   ", KEYWORDS)).toEqual(emptySearchState());
  });
  it("extracts a single chip", () => {
    expect(parseQuery("type:light", KEYWORDS)).toEqual({
      chips: [{ keyword: "type", value: "light" }],
      freeText: "",
    });
  });
  it("puts unknown keywords into free text verbatim", () => {
    expect(parseQuery("foo:bar baz", KEYWORDS)).toEqual({ chips: [], freeText: "foo:bar baz" });
  });
  it("mixes chips and free text", () => {
    expect(parseQuery("type:light bed", KEYWORDS)).toEqual({
      chips: [{ keyword: "type", value: "light" }],
      freeText: "bed",
    });
  });
  it("OR-groups multiple chips with the same keyword", () => {
    const state = parseQuery("type:light type:sensor", KEYWORDS);
    expect(state.chips).toEqual([
      { keyword: "type", value: "light" },
      { keyword: "type", value: "sensor" },
    ]);
    expect(state.freeText).toBe("");
  });
  it("preserves chip order across keywords", () => {
    const state = parseQuery("type:light room:kitchen type:sensor", KEYWORDS);
    expect(state.chips).toEqual([
      { keyword: "type", value: "light" },
      { keyword: "room", value: "kitchen" },
      { keyword: "type", value: "sensor" },
    ]);
  });
  it("keeps empty-value chips", () => {
    expect(parseQuery("type:", KEYWORDS)).toEqual({
      chips: [{ keyword: "type", value: "" }],
      freeText: "",
    });
  });
  it("preserves colons inside values (splits on the first colon only)", () => {
    expect(parseQuery("type:a:b", KEYWORDS)).toEqual({
      chips: [{ keyword: "type", value: "a:b" }],
      freeText: "",
    });
  });
  it("collapses internal double-spaces in free text", () => {
    expect(parseQuery("bed  room", KEYWORDS)).toEqual({ chips: [], freeText: "bed room" });
  });
});

describe("round-trip: parseQuery(serialize(state))", () => {
  const cases: { name: string; state: SearchState }[] = [
    { name: "empty", state: emptySearchState() },
    { name: "free text only", state: { chips: [], freeText: "hello world" } },
    { name: "single chip", state: { chips: [{ keyword: "type", value: "light" }], freeText: "" } },
    {
      name: "multiple same-keyword chips",
      state: {
        chips: [
          { keyword: "type", value: "light" },
          { keyword: "type", value: "sensor" },
        ],
        freeText: "",
      },
    },
    {
      name: "mixed chips and free text",
      state: {
        chips: [
          { keyword: "type", value: "light" },
          { keyword: "room", value: "kitchen" },
        ],
        freeText: "bedside",
      },
    },
    { name: "empty-value chip", state: { chips: [{ keyword: "type", value: "" }], freeText: "" } },
    {
      name: "value with colon",
      state: { chips: [{ keyword: "type", value: "a:b" }], freeText: "" },
    },
  ];
  for (const { name, state } of cases) {
    it(`round-trips: ${name}`, () => {
      expect(parseQuery(serialize(state), KEYWORDS)).toEqual(state);
    });
  }
});

describe("stateToTokens", () => {
  it("returns only the trailing live token for an empty state", () => {
    expect(stateToTokens(emptySearchState())).toEqual([""]);
  });
  it("emits one token per chip, then free text, then an empty live token", () => {
    const state: SearchState = {
      chips: [
        { keyword: "type", value: "light" },
        { keyword: "room", value: "kitchen" },
      ],
      freeText: "bedside",
    };
    expect(stateToTokens(state)).toEqual(["type:light", "room:kitchen", "bedside", ""]);
  });
  it("preserves spaces inside multi-word chip values and free text", () => {
    const state: SearchState = {
      chips: [{ keyword: "device", value: "Bedside Lamp" }],
      freeText: "bedroom lamp",
    };
    expect(stateToTokens(state)).toEqual(["device:Bedside Lamp", "bedroom lamp", ""]);
  });
  it("omits the free-text token when freeText is empty", () => {
    const state: SearchState = { chips: [{ keyword: "type", value: "light" }], freeText: "" };
    expect(stateToTokens(state)).toEqual(["type:light", ""]);
  });
});

describe("tokensToState", () => {
  it("returns empty state when every token is empty", () => {
    expect(tokensToState(["", ""], KEYWORDS)).toEqual(emptySearchState());
  });
  it("extracts chips and joins free text with a single space", () => {
    expect(tokensToState(["type:light", "bedroom", "lamp"], KEYWORDS)).toEqual({
      chips: [{ keyword: "type", value: "light" }],
      freeText: "bedroom lamp",
    });
  });
  it("preserves multi-word chip values verbatim", () => {
    expect(tokensToState(["device:Bedside Lamp"], ["device", "room"])).toEqual({
      chips: [{ keyword: "device", value: "Bedside Lamp" }],
      freeText: "",
    });
  });
  it("is the inverse of stateToTokens (sans trailing empty) for multi-word values", () => {
    const state: SearchState = {
      chips: [
        { keyword: "device", value: "Bedside Lamp" },
        { keyword: "device", value: "Ceiling" },
      ],
      freeText: "bedroom living room",
    };
    expect(tokensToState(stateToTokens(state), ["device"])).toEqual(state);
  });
});
