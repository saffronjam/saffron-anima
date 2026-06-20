-- A class with a number, a string, and a vec3 declared property. on_create writes
-- the injected field values into the entity's Transform so the test can read them
-- back: position = (speed, offset.x, offset.y), scale.x = #label. The vec3 field is
-- mutated in place to prove per-instance isolation (no bleed across instances).
local Fields = {}

Fields.properties = {
	speed = 3,
	label = "ab",
	offset = sa.vec3(10, 20, 30),
}

function Fields:on_create()
	-- Mutate the per-instance vec3 in place; a shared default would bleed to the
	-- next instance's on_create.
	self.offset.x = self.offset.x + 1
	self.entity:set_position(sa.vec3(self.speed, self.offset.x, self.offset.y))
	self.entity:set_scale(sa.vec3(#self.label, 1, 1))
end

function Fields:on_update(dt) end

return Fields
