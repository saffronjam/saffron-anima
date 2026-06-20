-- Exercises all three lifecycle hooks. on_create/on_update touch the entity; on_destroy
-- runs after the play scene is detached, so its entity access degrades to a no-op (it
-- must not error). The test asserts stop runs clean and the VM drops.
local Lifecycle = {}

Lifecycle.properties = {}

function Lifecycle:on_create()
	self.entity:set_position(sa.vec3(7, 0, 0))
end

function Lifecycle:on_update(dt) end

function Lifecycle:on_destroy()
	-- No scene is bound here; this must degrade to a no-op, never error.
	sa.log("lifecycle on_destroy: pos.x = " .. tostring(self.entity:get_position().x))
end

return Lifecycle
