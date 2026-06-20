-- on_create spawns one task that waits, then writes a marker into the entity's
-- position. The test ticks until the accumulated dt crosses the wait threshold and
-- asserts the marker appears only then (dt-driven, deterministic timing).
local Waiter = {}

Waiter.properties = {
	delay = 0.1,
}

function Waiter:on_create()
	-- Seed a recognizable "not yet woken" position.
	self.entity:set_position(sa.vec3(0, 0, 0))
	local entity = self.entity
	local delay = self.delay
	sa.spawn_task(function()
		sa.wait(delay)
		-- The task woke: mark x = 1 so the test can read it back.
		entity:set_position(sa.vec3(1, 0, 0))
	end)
end

function Waiter:on_update(dt) end

return Waiter
