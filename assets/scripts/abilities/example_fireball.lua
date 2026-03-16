-- example_fireball.lua
-- Demonstrates the Lua ability scripting API for Glory engine.
-- Hook functions receive: casterEntity, targetEntity, posX, posY, posZ

function onCast(caster, target, px, py, pz)
    -- Emit cast VFX at the caster position
    local casterPos = entity.getPosition(caster)
    vfx.emit("fireball_projectile", casterPos[1], casterPos[2], casterPos[3],
             px - casterPos[1], py - casterPos[2], pz - casterPos[3])

    -- Spawn the projectile entity
    local dir_x = px - casterPos[1]
    local dir_z = pz - casterPos[3]
    ability.spawnProjectile("example_fireball",
        casterPos[1], casterPos[2], casterPos[3],
        dir_x, 0.0, dir_z, 12.0)
end

function onHit(caster, target, px, py, pz)
    -- Calculate damage: 0.8 * AP + 100 base
    local ap = stats.getAP(caster)
    local dmg = ap * 0.8 + 100

    -- Apply magic damage
    ability.dealDamage(target, dmg, "magic")

    -- Explosion VFX at impact point
    local targetPos = entity.getPosition(target)
    vfx.emit("fireball_explosion", targetPos[1], targetPos[2], targetPos[3],
             0.0, 1.0, 0.0)
end
