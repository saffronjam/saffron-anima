-- on_update destroys its own entity. The handle stays valid for the rest of the
-- handler (the destroy is deferred); the entity is gone after the loop, and one
-- relink_hierarchy ran.
local SelfDestruct = {}

SelfDestruct.properties = {}

function SelfDestruct:on_update(dt)
	self.entity:destroy()
	-- The handle is still valid this tick — deferred, not immediate.
	assert(self.entity:valid(), "handle must stay valid for the rest of the handler")
end

return SelfDestruct
