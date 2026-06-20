-- A message receiver: the ping handler records the payload and whether the sender
-- resolved into the entity's Transform, so the test can read the dispatch back. The
-- handler runs after the instance loop, on the post-loop message drain.
local Receiver = {}

function Receiver:on_create()
	self.entity:set_position(sa.vec3(0, 0, 0))
end

function Receiver:on_update(dt) end

function Receiver:ping(sender, payload)
	local sender_ok = 0
	if sender ~= nil and sender:valid() and sender:name() == "sender" then
		sender_ok = 1
	end
	-- x = payload, y = sender-resolved flag, z accumulates the call count.
	local p = self.entity:get_position()
	self.entity:set_position(sa.vec3(payload or 0, sender_ok, p.z + 1))
end

return Receiver
