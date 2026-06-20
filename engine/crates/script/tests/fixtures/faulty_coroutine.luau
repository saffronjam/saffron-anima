-- on_create spawns a task that waits one threshold, wakes, and then errors. The
-- scheduler must log the coroutine fault and keep the VM alive — on_update keeps
-- advancing the entity position across ticks regardless of the dead coroutine.
local Faulty = {}

function Faulty:on_create()
	self.entity:set_position(sa.vec3(0, 0, 0))
	sa.spawn_task(function()
		sa.wait(0.05)
		error("deliberate coroutine fault")
	end)
end

function Faulty:on_update(dt)
	local p = self.entity:get_position()
	-- Each surviving tick advances x; the test asserts this keeps climbing even
	-- after the coroutine faults.
	self.entity:set_position(sa.vec3(p.x + 1, p.y, p.z))
end

return Faulty
