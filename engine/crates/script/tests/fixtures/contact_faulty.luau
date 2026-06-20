-- A contact handler that raises: dispatch_contact must surface the first failing handler
-- as a ScriptRunError (pause-on-error, like a faulting on_update), and the VM survives.
local Faulty = {}

function Faulty:on_update(dt) end

function Faulty:on_contact(other, point, normal)
	error("deliberate contact failure")
end

return Faulty
