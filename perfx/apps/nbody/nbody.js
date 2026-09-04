// nbody - the five-body Jovian planet simulation (compute-heavy)
//
// Advances the solar system's outer planets n steps with the symplectic Euler integrator, then
// reports the system's energy before and after, scaled to an integer.

'use strict';

const PI = 3.141592653589793;
const SOLAR_MASS = 4 * PI * PI;
const DAYS_PER_YEAR = 365.24;

function body(x, y, z, vx, vy, vz, mass) {
    return {x, y, z, vx, vy, vz, mass};
}

function makeBodies() {
    return [
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
    ];
}

function offsetMomentum(bodies) {
    let px = 0.0;
    let py = 0.0;
    let pz = 0.0;
    for (const b of bodies) {
        px += b.vx * b.mass;
        py += b.vy * b.mass;
        pz += b.vz * b.mass;
    }
    const sun = bodies[0];
    sun.vx = -px / SOLAR_MASS;
    sun.vy = -py / SOLAR_MASS;
    sun.vz = -pz / SOLAR_MASS;
}

function energy(bodies) {
    let e = 0.0;
    const count = bodies.length;
    for (let i = 0; i < count; i++) {
        const b = bodies[i];
        e += 0.5 * b.mass * (b.vx * b.vx + b.vy * b.vy + b.vz * b.vz);
        for (let j = i + 1; j < count; j++) {
            const b2 = bodies[j];
            const dx = b.x - b2.x;
            const dy = b.y - b2.y;
            const dz = b.z - b2.z;
            e -= (b.mass * b2.mass) / Math.sqrt(dx * dx + dy * dy + dz * dz);
        }
    }
    return e;
}

function advance(bodies, dt) {
    const count = bodies.length;
    for (let i = 0; i < count; i++) {
        const b = bodies[i];
        for (let j = i + 1; j < count; j++) {
            const b2 = bodies[j];
            const dx = b.x - b2.x;
            const dy = b.y - b2.y;
            const dz = b.z - b2.z;
            const d2 = dx * dx + dy * dy + dz * dz;
            const mag = dt / (d2 * Math.sqrt(d2));
            b.vx -= dx * b2.mass * mag;
            b.vy -= dy * b2.mass * mag;
            b.vz -= dz * b2.mass * mag;
            b2.vx += dx * b.mass * mag;
            b2.vy += dy * b.mass * mag;
            b2.vz += dz * b.mass * mag;
        }
    }
    for (const b of bodies) {
        b.x += dt * b.vx;
        b.y += dt * b.vy;
        b.z += dt * b.vz;
    }
}

function main() {
    const n = process.argv.length > 2 ? parseInt(process.argv[2], 10) : 200000;
    const t0 = performance.now();
    const bodies = makeBodies();
    offsetMomentum(bodies);
    const e0 = energy(bodies);
    for (let i = 0; i < n; i++) {
        advance(bodies, 0.01);
    }
    const e1 = energy(bodies);
    const t1 = performance.now();
    console.log('result: ' + Math.floor(e0 * 1e9) + ',' + Math.floor(e1 * 1e9));
    console.log('time: ' + (t1 - t0).toFixed(3));
}

main();
