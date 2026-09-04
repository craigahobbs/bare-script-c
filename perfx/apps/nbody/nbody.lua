-- nbody - the five-body Jovian planet simulation (compute-heavy)
--
-- Advances the solar system's outer planets n steps with the symplectic Euler integrator, then
-- reports the system's energy before and after, scaled to an integer.

local PI = 3.141592653589793
local SOLAR_MASS = 4 * PI * PI
local DAYS_PER_YEAR = 365.24

local function body(x, y, z, vx, vy, vz, mass)
    return {x = x, y = y, z = z, vx = vx, vy = vy, vz = vz, mass = mass}
end

local function makeBodies()
    return {
        body(0.0, 0.0, 0.0, 0.0, 0.0, 0.0, SOLAR_MASS),
        body(4.84143144246472090e+00, -1.16032004402742839e+00, -1.03622044471123109e-01,
             1.66007664274403694e-03 * DAYS_PER_YEAR, 7.69901118419740425e-03 * DAYS_PER_YEAR,
             -6.90460016972063023e-05 * DAYS_PER_YEAR, 9.54791938424326609e-04 * SOLAR_MASS),
        body(8.34336671824457987e+00, 4.12479856412430479e+00, -4.03523417114321381e-01,
             -2.76742510726862411e-03 * DAYS_PER_YEAR, 4.99852801234917238e-03 * DAYS_PER_YEAR,
             2.30417297573763929e-05 * DAYS_PER_YEAR, 2.85885980666130812e-04 * SOLAR_MASS),
        body(1.28943695621391310e+01, -1.51111514016986312e+01, -2.23307578892655734e-01,
             2.96460137564761618e-03 * DAYS_PER_YEAR, 2.37847173959480950e-03 * DAYS_PER_YEAR,
             -2.96589568540237556e-05 * DAYS_PER_YEAR, 4.36624404335156298e-05 * SOLAR_MASS),
        body(1.53796971148509165e+01, -2.59193146099879641e+01, 1.79258772950371181e-01,
             2.68067772490389322e-03 * DAYS_PER_YEAR, 1.62824170038242295e-03 * DAYS_PER_YEAR,
             -9.51592254519715870e-05 * DAYS_PER_YEAR, 5.15138902046611451e-05 * SOLAR_MASS)
    }
end

local function offsetMomentum(bodies)
    local px, py, pz = 0.0, 0.0, 0.0
    for _, b in ipairs(bodies) do
        px = px + b.vx * b.mass
        py = py + b.vy * b.mass
        pz = pz + b.vz * b.mass
    end
    local sun = bodies[1]
    sun.vx = -px / SOLAR_MASS
    sun.vy = -py / SOLAR_MASS
    sun.vz = -pz / SOLAR_MASS
end

local function energy(bodies)
    local e = 0.0
    local count = #bodies
    for i = 1, count do
        local b = bodies[i]
        e = e + 0.5 * b.mass * (b.vx * b.vx + b.vy * b.vy + b.vz * b.vz)
        for j = i + 1, count do
            local b2 = bodies[j]
            local dx = b.x - b2.x
            local dy = b.y - b2.y
            local dz = b.z - b2.z
            e = e - (b.mass * b2.mass) / math.sqrt(dx * dx + dy * dy + dz * dz)
        end
    end
    return e
end

local function advance(bodies, dt)
    local count = #bodies
    for i = 1, count do
        local b = bodies[i]
        for j = i + 1, count do
            local b2 = bodies[j]
            local dx = b.x - b2.x
            local dy = b.y - b2.y
            local dz = b.z - b2.z
            local d2 = dx * dx + dy * dy + dz * dz
            local mag = dt / (d2 * math.sqrt(d2))
            b.vx = b.vx - dx * b2.mass * mag
            b.vy = b.vy - dy * b2.mass * mag
            b.vz = b.vz - dz * b2.mass * mag
            b2.vx = b2.vx + dx * b.mass * mag
            b2.vy = b2.vy + dy * b.mass * mag
            b2.vz = b2.vz + dz * b.mass * mag
        end
    end
    for _, b in ipairs(bodies) do
        b.x = b.x + dt * b.vx
        b.y = b.y + dt * b.vy
        b.z = b.z + dt * b.vz
    end
end

local function main()
    local n = tonumber(arg[1]) or 200000
    local t0 = os.clock()
    local bodies = makeBodies()
    offsetMomentum(bodies)
    local e0 = energy(bodies)
    for _ = 1, n do
        advance(bodies, 0.01)
    end
    local e1 = energy(bodies)
    local t1 = os.clock()
    print(string.format('result: %d,%d', math.floor(e0 * 1e9), math.floor(e1 * 1e9)))
    print(string.format('time: %.3f', (t1 - t0) * 1000))
end

main()
