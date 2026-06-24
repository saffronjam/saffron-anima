# Networking & multiplayer

**Status:** PENDING IDEA

> Inspiration backlog — not yet implementable as written. Needs a codebase pass (a network-id↔entity map,
> registry-driven snapshot serialization, and a tick decoupled from render). This is a **program**, not a
> feature — treat it as its own multi-phase effort with an up-front architecture choice.

The architecture decision comes first: **authoritative server replication** vs. **rollback/lockstep**.
The latter is unusually feasible here — **deterministic Jolt + an ECS** are exactly what rollback needs —
and it differentiates against mainstream engines, but it constrains the whole engine to determinism.

## What it is

Multiplayer: moving game state between machines with consistency, authority, and latency hiding. Also the
session layer — matchmaking, lobbies, hosting, NAT traversal, deployment.

- **UE5:** the replication system (+ the newer Iris) — actor/property replication, RPCs, authority.
- **Unity:** Netcode for GameObjects.

## Core technique

- **Transport:** reliable-UDP (ordered/unordered channels, fragmentation, congestion).
- **Serialization + quantization:** delta-compress component state; quantize floats — **highest-leverage
  reuse** of the existing registry/codegen.
- **State replication:** a stable **network-id ↔ entity** map; relevancy/interest management; hierarchies
  need **scene-graph parenting**.
- **Authority/ownership:** server-authoritative or distributed; RPCs for events.
- **Client prediction + reconciliation:** the client simulates locally, the server corrects; needs world
  **snapshot/restore** + a deterministic tick decoupled from render.
- **Rollback/lockstep (the alternative):** only inputs cross the wire; each peer simulates
  deterministically and rolls back on a misprediction. Requires full engine determinism.
- **Session layer:** matchmaking, lobbies, hosting, NAT traversal (STUN/TURN/relay), deployment.

## Build size

- **L** transport (reliable-UDP) / **M** if wrapping `renet` or `quinn`.
- **M** serialization + quantization (reuse the registry — highest-leverage reuse).
- **L** state replication (needs the net-id↔entity map + parenting for hierarchies).
- **M** RPCs; **M** authority/ownership (client-server) / **L** distributed; **M** interest management.
- **XL** client prediction + reconciliation (snapshot/restore + deterministic tick).
- **L** lag compensation (Jolt historical raycast + a transform ring).
- **Session layer (matchmaking/lobbies/NAT/deploy):** L–XL, partly third-party (relay services).

## Dependencies (do these first)

- **Architecture choice up front:** authoritative replication vs. rollback/lockstep.
- **Stable entity GUIDs / a network-id map** + **scene-graph parenting** (hierarchies).
- A **deterministic tick decoupled from render** (rollback/prediction). Jolt is already
  cross-platform-deterministic — a genuine asset.

## What we reuse / what's missing

**Reuse:** the component registry + codegen (serialization — the big win), deterministic Jolt +
`CharacterVirtual` (prediction/rollback feasibility), hecs (state), the contact ring/signals (events), and
Luau/control plane.

**Missing:** the transport layer, a network-id↔entity map, snapshot/restore, a render-decoupled
deterministic tick, scene-graph parenting, and the whole session/matchmaking layer.

## Notes & references

- Glenn Fiedler, "Gaffer on Games" (networked physics, snapshot interpolation, reliable-UDP).
- GDC, "Overwatch Gameplay Architecture and Netcode" — prediction/reconciliation done well.
- `renet` / `quinn` (Rust) as transport candidates; UE5 Iris docs for a modern replication design.
