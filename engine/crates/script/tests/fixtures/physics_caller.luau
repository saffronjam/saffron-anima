-- Exercises the physics-reaching bindings against a stub host bridge. on_update drives a
-- ray, the rigidbody pushes, a velocity read-back, and the ragdoll surface, recording the
-- observable results into the entity's Transform so the test can assert the routing.
local Caller = {}

function Caller:on_create() end

function Caller:on_update(dt)
	-- Push the body three ways (the stub records the last value + entity uuid).
	self.entity:apply_impulse(sa.vec3(1, 2, 3))
	self.entity:add_force(sa.vec3(4, 5, 6))
	self.entity:set_velocity(sa.vec3(7, 8, 9))
	-- Read the velocity back from the stub (it echoes a fixed value).
	local v = self.entity:get_velocity()
	self.entity:set_position(v)

	-- A ray through the stub returns a fixed hit; record point/distance + entity presence.
	local hit = sa.raycast(0, 0, 0, 0, 0, 1, 100)
	local entity_ok = 0
	if hit.entity ~= nil and hit.entity:valid() then
		entity_ok = 1
	end
	self.entity:set_rotation(sa.vec3(hit.hit and 1 or 0, hit.distance, entity_ok))

	-- Ragdoll: enable + blend + read state.
	local enabled = self.entity:enable_ragdoll()
	self.entity:set_ragdoll_blend(true, 0.5)
	local rs = self.entity:ragdoll_state()
	self.entity:set_scale(sa.vec3(enabled and 1 or 0, rs.present and 1 or 0, rs.bones))

	-- A log line routes to the sink (recorded by the stub).
	sa.log("hello from physics_caller")
end

return Caller
