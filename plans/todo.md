# Todo

## Editor UX

- Drag and drop models from the asset browser to create entities in the scene.
- Let Inspector components have an explicit order, with add-at-bottom behavior, drag reordering, and a sort action.
- Fix browser UI quirks like drag-selecting elements so the editor feels like a normal desktop app.

## Rendering

- Improve PBR effect, seems a bit foggy.
- Improve lighting support for transparency, opacity, and self-shadowing.

## Physics and animation

- Physics 3D bodies for entities with wireframe representation.
- Physics-based two-way bound animations after physics.

## Scripting

- Swap the scripting VM from stock Lua 5.5 to Luau for a real in-language gradual type system, replacing the LuaLS-annotation overlay (`library/sa.lua` + the drift tripwire) with actual typed sources; evaluate the impact on the LuaBridge bindings, the sandbox, and cross-platform determinism.

## Game systems

- Research game UI and overlay authoring for health bars and HUDs, including how Unreal Engine 5 and Unity approach it.
- Research sound and music systems, including ambient audio, spatial playback, and whether Saffron needs its own sound asset container.

## Networking

- Research networking architecture for server/client games and playing with friends.
- Research replication, authority, prediction, reconciliation, and rollback models for multiplayer.
- Research matchmaking, lobbies, hosting, NAT traversal, and deployment options for multiplayer games.

## Assets and distribution

- Research online asset store integration, including an in-editor browser and available/common asset sources.
- Research production export packaging so the app can run standalone, including whether it should ship as a single binary.
