# nbody - the five-body Jovian planet simulation (compute-heavy)
#
# Advances the solar system's outer planets n steps with the symplectic Euler integrator, then
# reports the system's energy before and after, scaled to an integer.

PI = 3.141592653589793
SOLAR_MASS = 4 * PI * PI
DAYS_PER_YEAR = 365.24

class Body
  attr_accessor :x, :y, :z, :vx, :vy, :vz, :mass

  def initialize(x, y, z, vx, vy, vz, mass)
    @x = x
    @y = y
    @z = z
    @vx = vx
    @vy = vy
    @vz = vz
    @mass = mass
  end
end

def make_bodies
  [
    Body.new(0.0, 0.0, 0.0, 0.0, 0.0, 0.0, SOLAR_MASS),
    Body.new(4.84143144246472090e+00, -1.16032004402742839e+00, -1.03622044471123109e-01,
             1.66007664274403694e-03 * DAYS_PER_YEAR, 7.69901118419740425e-03 * DAYS_PER_YEAR,
             -6.90460016972063023e-05 * DAYS_PER_YEAR, 9.54791938424326609e-04 * SOLAR_MASS),
    Body.new(8.34336671824457987e+00, 4.12479856412430479e+00, -4.03523417114321381e-01,
             -2.76742510726862411e-03 * DAYS_PER_YEAR, 4.99852801234917238e-03 * DAYS_PER_YEAR,
             2.30417297573763929e-05 * DAYS_PER_YEAR, 2.85885980666130812e-04 * SOLAR_MASS),
    Body.new(1.28943695621391310e+01, -1.51111514016986312e+01, -2.23307578892655734e-01,
             2.96460137564761618e-03 * DAYS_PER_YEAR, 2.37847173959480950e-03 * DAYS_PER_YEAR,
             -2.96589568540237556e-05 * DAYS_PER_YEAR, 4.36624404335156298e-05 * SOLAR_MASS),
    Body.new(1.53796971148509165e+01, -2.59193146099879641e+01, 1.79258772950371181e-01,
             2.68067772490389322e-03 * DAYS_PER_YEAR, 1.62824170038242295e-03 * DAYS_PER_YEAR,
             -9.51592254519715870e-05 * DAYS_PER_YEAR, 5.15138902046611451e-05 * SOLAR_MASS)
  ]
end

def offset_momentum(bodies)
  px = 0.0
  py = 0.0
  pz = 0.0
  bodies.each do |b|
    px += b.vx * b.mass
    py += b.vy * b.mass
    pz += b.vz * b.mass
  end
  sun = bodies[0]
  sun.vx = -px / SOLAR_MASS
  sun.vy = -py / SOLAR_MASS
  sun.vz = -pz / SOLAR_MASS
end

def energy(bodies)
  e = 0.0
  count = bodies.length
  i = 0
  while i < count
    b = bodies[i]
    e += 0.5 * b.mass * (b.vx * b.vx + b.vy * b.vy + b.vz * b.vz)
    j = i + 1
    while j < count
      b2 = bodies[j]
      dx = b.x - b2.x
      dy = b.y - b2.y
      dz = b.z - b2.z
      e -= (b.mass * b2.mass) / Math.sqrt(dx * dx + dy * dy + dz * dz)
      j += 1
    end
    i += 1
  end
  e
end

def advance(bodies, dt)
  count = bodies.length
  i = 0
  while i < count
    b = bodies[i]
    j = i + 1
    while j < count
      b2 = bodies[j]
      dx = b.x - b2.x
      dy = b.y - b2.y
      dz = b.z - b2.z
      d2 = dx * dx + dy * dy + dz * dz
      mag = dt / (d2 * Math.sqrt(d2))
      b.vx -= dx * b2.mass * mag
      b.vy -= dy * b2.mass * mag
      b.vz -= dz * b2.mass * mag
      b2.vx += dx * b.mass * mag
      b2.vy += dy * b.mass * mag
      b2.vz += dz * b.mass * mag
      j += 1
    end
    i += 1
  end
  bodies.each do |b|
    b.x += dt * b.vx
    b.y += dt * b.vy
    b.z += dt * b.vz
  end
end

def main
  n = ARGV.length > 0 ? ARGV[0].to_i : 200000
  t0 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
  bodies = make_bodies
  offset_momentum(bodies)
  e0 = energy(bodies)
  n.times { advance(bodies, 0.01) }
  e1 = energy(bodies)
  t1 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
  puts "result: #{(e0 * 1e9).floor},#{(e1 * 1e9).floor}"
  puts format('time: %.3f', (t1 - t0) * 1000)
end

main
