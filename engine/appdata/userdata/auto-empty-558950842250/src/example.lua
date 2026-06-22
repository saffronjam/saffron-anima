-- example.lua: attach to an entity's Script component, then press Play.
-- Orbits the entity in the x/y plane around where it was authored.
---@class Example : sa.ScriptSelf
local Example = {}

Example.properties = {
  speed = 1.0,  -- radians/second, editable in the Inspector
  radius = 2.0,
}

function Example:on_create()
  -- Center one radius left of the authored spot, so the orbit starts on the entity.
  self.center = self.entity:get_position() - sa.vec3(self.radius, 0, 0)
  self.angle = 0
end

function Example:on_update(dt)
  self.angle = self.angle + self.speed * dt
  local r = self.radius
  self.entity:set_position(self.center + sa.vec3(math.cos(self.angle) * r, math.sin(self.angle) * r, 0))
end

return Example
