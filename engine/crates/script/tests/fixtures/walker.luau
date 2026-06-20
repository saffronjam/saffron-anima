-- Drives a CharacterController capsule: move_character is a pure Scene write (no host
-- bridge), so the test reads desired_velocity / vertical_velocity back off the component.
local Walker = {}

function Walker:on_update(dt)
	self.entity:move_character(sa.vec3(2, 99, 3), true)
end

return Walker
