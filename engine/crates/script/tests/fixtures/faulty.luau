-- on_update raises an error every tick. The runtime's pause-on-error halts the tick
-- at this instance and returns a ScriptRunError carrying the message + traceback; the
-- VM survives a subsequent tick.
local Faulty = {}

Faulty.properties = {}

function Faulty:on_update(dt)
	error("deliberate update failure")
end

return Faulty
