-- A class table that omits the required on_update — load must reject it, and the slot
-- is a logged skip (the session continues without it).
local Broken = {}

Broken.properties = {}

function Broken:on_create() end

return Broken
