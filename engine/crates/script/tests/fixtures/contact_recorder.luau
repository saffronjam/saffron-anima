-- A contact recorder: each handler records its dispatch into the entity's Transform so
-- the test can read it back after the session ends. on_contact also stows the manifold
-- (point/normal) and the "other" entity's name into the scale, so the test can assert the
-- handler received the world-space point/normal as a Vec3 and the right other handle.
local Recorder = {}

function Recorder:on_create()
	self.entity:set_position(sa.vec3(0, 0, 0))
	self.entity:set_rotation(sa.vec3(0, 0, 0))
	self.entity:set_scale(sa.vec3(0, 0, 0))
end

function Recorder:on_update(dt) end

function Recorder:on_contact(other, point, normal)
	local p = self.entity:get_position()
	-- x counts solid contacts; y stows the point.x; z stows the normal.y (so the test
	-- can confirm the manifold crossed as a Vec3, not nil).
	local other_ok = 0
	if other ~= nil and other:valid() then
		other_ok = 1
	end
	self.entity:set_position(sa.vec3(p.x + 1, point.x, normal.y))
	-- scale.x records whether `other` resolved to a live handle this dispatch.
	self.entity:set_scale(sa.vec3(other_ok, 0, 0))
end

function Recorder:on_trigger_enter(other)
	local r = self.entity:get_rotation()
	self.entity:set_rotation(sa.vec3(r.x + 1, r.y, r.z))
end

function Recorder:on_trigger_exit(other)
	local r = self.entity:get_rotation()
	self.entity:set_rotation(sa.vec3(r.x, r.y + 1, r.z))
end

return Recorder
