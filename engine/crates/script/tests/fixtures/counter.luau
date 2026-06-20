-- A minimal script class: on_create seeds the entity position, on_update advances
-- it. The test reads the entity's Transform back to assert call counts and order.
local Counter = {}

Counter.properties = {
	speed = 2,
}

function Counter:on_create()
	-- Mark "created once" by writing position.y = 1; on_update will count on x.
	self.entity:set_position(sa.vec3(0, 1, 0))
end

function Counter:on_update(dt)
	local p = self.entity:get_position()
	-- Advance x by speed each tick; the test asserts this accumulates per tick.
	self.entity:set_position(sa.vec3(p.x + self.speed, p.y, p.z))
end

return Counter
