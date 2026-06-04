# Phase 4: Control-plane: parentId on list-entities, set-parent command, schema + contract test + se CLI

**Status:** NOT STARTED

<!-- Flip to COMPLETED when the Done-when checklist passes validation-clean (tools/ci/check.sh schema/contract stage green, the generated control outputs regenerated and committed). Delete this file only after COMPLETED + merged. -->


## Goal

Expose parenting over the wire, DTO-first. `list-entities` emits an optional `parentId`
(the entity's `RelationshipComponent.parent` uuid as a decimal string; absent/`"0"` == root)
per entry. A new cycle-guarded `set-parent {entity, parent?}` command reparents (and clears
to root via `parent` absent/`0`), bumps `sceneVersion`, and returns an `EntityRef`. The
contract test's u64-precision allowlist widens to cover `parentId` and drives an
add-entity → set-parent → list-entities round-trip. The `se` CLI keeps showing the tree
(`parentId` column) and reaches `set-parent` (default JSON dump is the minimum).

Depends on phases 1 (`RelationshipComponent` + `setParent`/cycle guard live in `Saffron.Scene`)
and 2 (serialize-by-uuid + the `sceneFromJson` resolve pass). This phase adds zero engine
*mechanism* — `setParent` and the cycle guard already exist; it only exposes them on the wire.
Architecture is settled in `plans/scene-hierarchy/phase-0-research-and-architecture.md` (flat
list + `parentId`, engine-authoritative reparent, `parentId` confined to the list-entry type) —
do not re-litigate it here.


## The DTO-first wire contract

The control wire is generated from DTO structs, not hand-authored per-command schemas. The
source of truth is the DTO structs in `engine/source/saffron/control/control_dto.cppm`
(`WireUuid` control_dto.cppm:21-24, `EntityRef` control_dto.cppm:36-40, `EntityList`
control_dto.cppm:421-424). The generator `tools/gen-control-dto/gen.ts` carries a hardcoded
`commands` array (gen.ts:90; the `list-entities` entry is gen.ts:132), a `commandFixtures`
map and a `commandSkips` map (gen.ts:307/:366), and emits five outputs:
`control_dto_serde.generated.cpp`, `scene_component_serde.generated.cpp`,
`editor/src/protocol/se-types.ts`, `schemas/control/openrpc.generated.json`, and
`schemas/control/command-manifest.generated.json`. The only hand-authored schema under
`schemas/control/` is `envelope.schema.json`; there are no per-command, `entity-ref`,
`entity-list`, or `uuid` schema files. Every generated DTO schema is closed
(`additionalProperties:false`, all declared fields required unless `std::optional` —
gen.ts `schemaFor` :1173). Regenerate with `bun run tools/gen-control-dto/gen.ts`
(`tools/ci/check.sh:23` runs it; the editor `bun run gen:protocol` is a shim spawning the
same generator; `editor/src/protocol/index.ts` is a hand-curated re-export over `se-types.ts`).

Adding a control command therefore means: declare the params/result structs in
`control_dto.cppm`, add an entry to the gen.ts `commands` array (with a `commandFixtures` entry
or a `commandSkips` reason for the manifest-driven contract test), `registerCommand<Params,
Result>` in C++, run the generator, and commit the generated outputs.


## Current state

- `list-entities` (`control_commands_scene.cpp:115-124`) is registered as
  `registerCommand<EmptyParams, EntityList>`; its body runs `forEach<IdComponent, NameComponent>`
  and pushes `EntityRef{ WireUuid{ id.id.value }, name.name }` per entity — flat, no parent. The
  result DTO is `EntityList { std::vector<EntityRef> entities; }` (control_dto.cppm:421-424); the
  list item IS the shared `EntityRef` (control_dto.cppm:36-40) today.
- `EntityRef` is built by the `entityRefDto` factory (`control_server.cpp:134`) and is returned by
  ~12 commands. Because the generator emits every DTO as a closed object, widening `EntityRef`
  ripples through every one of those commands' generated result schemas and serde at once.
- `resolveEntity` (`control_server.cpp:72`) hardcodes the `"entity"` key (an O(n) uuid-then-name
  scan). Resolving a `parent` selector needs a generalized resolver or an inline second resolve.
- `destroy-entity` (`control_commands_scene.cpp:147-164`) is the template for a mutating command
  that resolves an `EntitySelector`, bumps `ctx.sceneEdit.sceneVersion += 1` (:162), and returns a
  result DTO. It clears selection only for the destroyed root (:157).
- IDs cross the wire as `WireUuid`, serialized as decimal JSON strings (`type:string`,
  `pattern ^[0-9]+$` in the generated schema); a new id field is a `WireUuid` at the DTO boundary,
  never a bare integer.
- The contract test `tools/check-control-schema/check.ts` is manifest-driven: it loads
  `openrpc.generated.json` + `command-manifest.generated.json` (check.ts:15-37), validates each
  command's live result against the generated schema, and asserts a `help <-> manifest`
  completeness pair (check.ts:353-364) so every registered command has a manifest entry. Its
  `assertRawU64` (check.ts:132) scans the raw result bytes with the allowlist regex
  `/"(?:id|mesh|albedoTexture|skyTexture|texture|entity)"\s*:\s*([^,}\s]+)/g` (check.ts:134),
  asserting each token is a quoted decimal that round-trips as `BigInt` (check.ts:141-146). Each
  command's params come from a named fixture resolved in `paramsForFixture` (check.ts:225) or are
  skipped with a reason. `firstResultId` pulls an id string from raw bytes for chaining.
- `se` CLI discovers commands from the live `help` command (the reflective registry,
  `control_commands_render.cpp:117`); `printResult` (`tools/se/source/main.cpp:112`) has a
  `list-entities` formatter (:137) printing a two-column `id name`, and unhandled commands fall
  through to the UTF-8 `dump(2)` default (:258). Bare positionals map to `params["args"]`
  (consumed by `positionalOr` at index 0/1); `--flag value` maps to `params[flag]`.
- The Tauri Rust bridge is one generic `control(cmd, params)` passthrough — a new command needs
  zero Rust changes. `editor/src/state/store.ts` calls `listEntities` on `sceneVersion` change
  (store.ts:284) inside the 50 ms (~20 Hz) fast lane gating heavy refreshes (store.ts:201).


## Implementation

### 1. DTO: a dedicated list-entry type carrying optional `parentId`

`parentId` lands on a NEW dedicated list-entry DTO, never on the shared `EntityRef`. In
`control_dto.cppm`, add an `EntityListEntry` next to `EntityRef` and switch `EntityList.entities`
to it:

```cpp
struct EntityListEntry
{
    WireUuid                  id;
    std::string               name;
    std::optional<WireUuid>   parentId;   // absent == root
};

struct EntityList
{
    std::vector<EntityListEntry> entities;
};
```

`std::optional<WireUuid>` is the same shape the codebase already uses for optional id fields
(`std::optional<WireUuid> albedoTexture` / `skyTexture` / `id` in control_dto.cppm:471/:519/:497) —
the generator renders it as a closed-schema optional property (omitted when absent) and as
`parentId?: string` in TypeScript. Leave `EntityRef` untouched: it stays the closed `{ id, name }`
returned by ~12 commands (the blast-radius rationale in Risks).

### 2. `list-entities`: emit `parentId` per entry

In `list-entities` (`control_commands_scene.cpp:115-124`), keep the flat array but read each
entity's `RelationshipComponent.parent` and set it on the entry. The
`forEach<IdComponent, NameComponent>` view does not include the relationship; fetch it inside the
body (every entity has one after phase 1's `createEntity` default). Set `parentId` to the parent
uuid, leaving it absent for roots — **omit when `parent == 0`** so the field is genuinely optional
and root entities carry no `parentId`:

```cpp
registerCommand<EmptyParams, EntityList>(
    reg, "list-entities", "list all entities",
    [](EngineContext& ctx, const EmptyParams&) -> Result<EntityList>
    {
        EntityList out;
        forEach<IdComponent, NameComponent>(
            ctx.sceneEdit.scene, [&](Entity e, IdComponent& id, NameComponent& name)
            {
                EntityListEntry entry{ WireUuid{ id.id.value }, name.name, std::nullopt };
                const u64 parent = getComponent<RelationshipComponent>(ctx.sceneEdit.scene, e).parent.value;
                if (parent != 0) { entry.parentId = WireUuid{ parent }; }
                out.entities.push_back(std::move(entry));
            });
        return out;
    });
```

This is read-only — no `sceneVersion` bump. `parentId` is a `WireUuid` (decimal string on the
wire), never a bare integer. Do NOT touch `entityRefDto` (`control_server.cpp:134`) — leave the
~12 other commands' DTO untouched (see Risks).

### 3. New `set-parent` command

Declare the params/result DTOs in `control_dto.cppm` and register the command. The result reuses
the shared `EntityRef`; the params are a child selector plus an optional parent selector:

```cpp
struct SetParentParams
{
    EntitySelector                  entity;
    std::optional<EntitySelector>   parent;   // absent/0 => detach to root
};
```

Add the entry to the gen.ts `commands` array (next to the other scene commands):

```ts
{ name: "set-parent", params: "SetParentParams", result: "EntityRef", summary: "set-parent {entity, parent?}" },
```

Register it in `control_commands_scene.cpp` near `destroy-entity`
(`control_commands_scene.cpp:147-164`), modeled on it. It resolves the child, resolves the parent
(absent/`0` => root), calls `setParent` (phase 1), bumps `sceneVersion`, and returns the child's
`EntityRef`:

```cpp
registerCommand<SetParentParams, EntityRef>(
    reg, "set-parent", "set-parent {entity, parent?}",
    [](EngineContext& ctx, const SetParentParams& params) -> Result<EntityRef>
    {
        auto child = resolveEntity(ctx, params.entity);
        if (!child) { return Err(child.error()); }

        Entity newParent{ entt::null };  // absent/0 => detach to root
        if (params.parent && !isRootSelector(*params.parent))
        {
            auto resolved = resolveEntity(ctx, *params.parent);
            if (!resolved) { return Err(resolved.error()); }
            newParent = *resolved;
        }

        auto ok = setParent(ctx.sceneEdit.scene, *child, newParent);  // cycle guard inside
        if (!ok) { return Err(ok.error()); }
        ctx.sceneEdit.sceneVersion += 1;
        return entityRefDto(ctx.sceneEdit.scene, *child);
    });
```

**Parent resolution.** `resolveEntity` takes an `EntitySelector` (the uuid-then-name walk), so it
resolves the `parent` selector the same way it resolves `entity` — no special second resolver.
Treat an absent selector and a `0`/`"0"` selector as root before resolving (`isRootSelector`, a
small local helper inspecting the selector's `Json`), so a detach never tries to resolve entity 0.

**Cycle guard.** Lives in `setParent` (phase 1) — reject making an entity its own ancestor and
self-parent, returning `Err`. The command surfaces that `Err` as `ok:false`. Do NOT duplicate the
walk here.

**Selection.** `set-parent` MUST NOT clear `ctx.sceneEdit.selected` — a reparent keeps the
selection intact (the editor's optimistic relink relies on this). Only `sceneVersion` bumps;
`selectionVersion` does not.

### 4. Regenerate and commit the generated outputs

Run `bun run tools/gen-control-dto/gen.ts` (the editor `bun run gen:protocol` shim spawns the same
generator). Commit every regenerated output:

- `engine/source/saffron/control/control_dto_serde.generated.cpp` — parse/serialize for
  `EntityListEntry` (with the optional `parentId`) and `SetParentParams`.
- `editor/src/protocol/se-types.ts` — `EntityList.entities` becomes `EntityListEntry[]` with
  `parentId?: string`; `SetParentParams` appears.
- `schemas/control/openrpc.generated.json` — gains the `EntityListEntry` schema with optional
  `parentId`, the `set-parent` method, and a `parentId` token in `EntityList`'s item schema.
- `schemas/control/command-manifest.generated.json` — gains the `set-parent` row with its fixture
  (or skip reason).

Never hand-edit the generated files. `editor/src/protocol/index.ts` is hand-curated — re-export the
new types if a later editor phase consumes them.

### 5. Contract test: widen the u64 allowlist + drive a reparent

In `tools/check-control-schema/check.ts`:

- **Widen `assertRawU64`** (check.ts:134): add `parentId` to the allowlist regex →
  `/"(?:id|mesh|albedoTexture|skyTexture|texture|entity|parentId)"\s*:\s*([^,}\s]+)/g`. Without this
  the new field is not precision-checked (and if it is ever emitted unquoted, the regression goes
  unseen). It still asserts the quoted-decimal `BigInt` round-trip (check.ts:141-146).
- **Give `set-parent` a manifest fixture.** The manifest-driven loop validates one happy-path result
  per command, so add a `set-parent` entry to the gen.ts `commandFixtures` map and a matching case in
  `paramsForFixture` (check.ts:225) that reparents an entity under the seeded cube, e.g. add
  `["set-parent", "set-parent-under-cube"]` and a case that mints a fresh entity and returns
  `{ entity: childId, parent: state.cubeId }`. The validated result is the child's `EntityRef`. The
  alternative, a `commandSkips` reason, is acceptable only if a happy-path fixture proves awkward —
  prefer the fixture so the result schema is actually exercised.
- **Drive the round-trip explicitly.** Alongside the manifest loop, add bespoke `call()`-based
  assertions (check.ts `call()`): `add-entity` a second entity (capture its id via `firstResultId`),
  `set-parent { entity: childId, parent: cubeId }`, then `list-entities` and assert the child entry
  carries `parentId == cubeId` and that the result still validates against the generated `EntityList`
  schema (so `assertRawU64` covers the `parentId` token).
- Add a **negative assertion**: `set-parent { entity: cubeId, parent: childId }` (parent onto its own
  descendant) must return `ok:false` with a non-empty `error` (cycle rejected server-side). Use the
  raw `call()`, not the manifest validator (which expects `ok===true`).
- Add a **detach assertion**: `set-parent { entity: childId, parent: '0' }` returns ok; a following
  `list-entities` has the child entry with no `parentId` (root).

The test loads the generated manifest/OpenRPC at startup, so once the regenerated outputs land and
the engine emits `parentId`, the validator walks the new `EntityListEntry` item type and
`assertRawU64` covers the `parentId` token with no further wiring beyond the regex and the fixture.

### 6. `se` CLI: show parent + reach set-parent

In `tools/se/source/main.cpp`:

- **Extend the `list-entities` formatter** (:137): add a `parentId` column so the tree is visible
  from a shell. Print `id`, `name`, and `parent` (empty for roots):

```cpp
std::printf("  %-20s  %-24s  %s\n",
    e.value("id", std::string{}).c_str(),
    e.value("name", "").c_str(),
    e.value("parentId", std::string{}).c_str());
```

- **`set-parent`** needs no dedicated branch — it discovers via `help` (the reflective registry) and
  falls through to the UTF-8 `dump(2)` default (:258), which prints the returned `EntityRef`. That
  satisfies the keep-current minimum (the command is reachable and inspectable from `se`). A one-line
  `name id parent` formatter is optional polish; skip it unless trivial. Argument coercion already
  maps `se set-parent <child> <parent>` (bare positionals → `params["args"]`, consumed by
  `positionalOr` at index 0/1) and `--parent 0` for detach.

### 7. Editor (light touch — engine-authoritative, ids-as-strings)

The flat-list-with-`parentId` shape lands in `editor/src/protocol/se-types.ts` via step 4's regen.
The React tree build, expand-state, and drag-reparent are owned by the editor phase of this plan
(`plans/scene-hierarchy/phase-5-*`, the React tree) — this phase only guarantees the wire and a typed
client wrapper:

- Add `client.setParent(id: string, parentId: string | null)` to `editor/src/control/client.ts`,
  routing `control('set-parent', { entity: id, parent: parentId ?? 0 })`. `parentId` stays a **string
  or null** end-to-end — never `Number()` it (u64 precision). Use the `callRaw` pattern; add to the
  typed result map only if a typed `EntityRef` return is wanted.
- Respect the reconcile fast lane (`editor/src/state/store.ts`): `parentId` rides the existing
  `sceneVersion`-keyed `listEntities` lane already in place (store.ts:284) — no new fan-out call. Do
  NOT add a poll for hierarchy.
- The X11-child viewport overlay constraint binds the React tree (drop indicators must stay in the
  sidebar DOM), but that is the React phase's concern; nothing in this phase paints over the viewport.

Keep the editor changes here minimal (wrapper + regenerated types); the tree UI is its own phase.


## Done when

- [ ] `EntityListEntry` exists in `control_dto.cppm` with optional `parentId` and `EntityList.entities`
      is `std::vector<EntityListEntry>`; `EntityRef` is untouched.
- [ ] `list-entities` returns an optional `parentId` (decimal string) per entry — present for
      parented entities, absent for roots.
- [ ] `set-parent {entity, parent?}` is declared (`SetParentParams`), added to the gen.ts `commands`
      array with a fixture (or skip reason), registered in `control_commands_scene.cpp`, and returns
      the child's `EntityRef`.
- [ ] `se set-parent <child> <parent>` then `se list-entities` shows the child under its parent
      (`parentId` column populated); `se set-parent <child> --parent 0` then `se list-entities` shows
      it back at root (no `parentId`).
- [ ] `se inspect <child>` after a reparent reflects the new parent via the serialized
      `RelationshipComponent` (round-trips through `inspect`).
- [ ] `set-parent` bumps `sceneVersion` (a following `get-selection`/`list-entities` poll sees the new
      `sceneVersion`) and leaves `selectionVersion`/`selected` unchanged.
- [ ] `set-parent` onto a descendant returns `ok:false` with a non-empty error (cycle rejected
      server-side, asserted by the contract test).
- [ ] A parented scene round-trips through `writeScene`/`readScene` (or the
      `runSceneSerializationSelfTest` path) with the parent link preserved — verifiable headless via
      `save-project` then `open-project` then `list-entities` showing the same `parentId`.
- [ ] `assertRawU64` covers `parentId` (the allowlist regex includes it; u64 precision preserved as a
      quoted decimal string).
- [ ] `bun run tools/gen-control-dto/gen.ts` regenerates the five outputs; `se-types.ts` carries the
      `EntityListEntry` type with optional `parentId: string`; `bun run check` typechecks clean.
- [ ] `openrpc.generated.json` gains the `EntityListEntry` schema with optional `parentId` and the
      `set-parent` method; `command-manifest.generated.json` gains the `set-parent` row.
- [ ] `tools/ci/check.sh` schema/contract stage is green (the manifest-driven test validates the new
      shape, the bespoke round-trip drives add-entity → set-parent → list-entities, and the engine log
      is validation-clean).


## Risks / seams

- **`EntityRef` blast radius.** `EntityRef` is the closed DTO returned by ~12 commands (built by
  `entityRefDto`, control_server.cpp:134); the generator emits every DTO as a closed object, so adding
  `parentId` there ripples through every one of those generated result schemas at once. Confine
  `parentId` to the new `EntityListEntry` type and leave `EntityRef` untouched.
- **`assertRawU64` will FAIL `parentId`** unless its allowlist regex (check.ts:134) is widened in the
  same change — the new u64 string field is not precision-checked otherwise, and an unquoted regression
  slips through.
- **Manifest completeness.** The contract test asserts `help <-> manifest` (check.ts:353-364): every
  registered command must appear in `command-manifest.generated.json`. A `set-parent` `registerCommand`
  without a matching gen.ts `commands` entry (and fixture or skip) fails that completeness check — add
  both in the same change and regenerate.
- **Set-parent MUST bump `sceneVersion`** or the editor tree never refreshes (the reconcile fast lane
  is `sceneVersion`-keyed). It must NOT bump `selectionVersion` or clear `selected` — a reparent keeps
  the selection.
- **Optional vs `"0"` for root.** Choosing *omit `parentId` when parent==0* keeps the field genuinely
  optional and the generated schema clean; the `se` formatter and the editor must both treat absent and
  `"0"` as root. Pick one convention (omit) and keep all three consumers (engine emit, `se` formatter,
  contract test) in agreement.
- **Generated-output sync obligation.** The DTOs, the generated serde/TS/OpenRPC/manifest, the contract
  test, and the `se` CLI must stay in lockstep (root `AGENTS.md` keep-current). There is no `dump-schema`
  command; live introspection is the `help` command plus the generated manifest/OpenRPC. Regenerating and
  committing the five outputs, widening the contract test, and updating the `se` formatter are part of
  "done", not follow-ups.
