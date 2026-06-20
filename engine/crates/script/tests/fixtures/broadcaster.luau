-- Broadcasts one "ping" to every script instance on the first tick. Every receiver
-- (and itself, if it had the handler) gets handler(self, sender, payload) after the
-- loop. The payload is a fixed number the receivers record.
local Broadcaster = {}

function Broadcaster:on_create()
	self.sent = false
end

function Broadcaster:on_update(dt)
	if not self.sent then
		self.sent = true
		sa.broadcast("ping", 3)
	end
end

return Broadcaster
