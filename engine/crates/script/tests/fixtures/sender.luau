-- Sends one "ping" message to the receiver entity (resolved by name) on the first
-- tick only, with a numeric payload. The message dispatches after the instance loop.
local Sender = {}

function Sender:on_create()
	self.sent = false
end

function Sender:on_update(dt)
	if not self.sent then
		self.sent = true
		local target = sa.get_entity_by_name("receiver")
		if target:valid() then
			target:send("ping", 7)
		end
	end
end

return Sender
