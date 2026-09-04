# nbody - the five-body Jovian planet simulation (compute-heavy)
#
# Advances the solar system's outer planets n steps with the symplectic Euler integrator, then
# reports the system's energy before and after, scaled to an integer.

use strict;
use warnings;
use POSIX qw(floor);
use Time::HiRes qw(time);

my $PI = 3.141592653589793;
my $SOLAR_MASS = 4 * $PI * $PI;
my $DAYS_PER_YEAR = 365.24;

sub body {
    my ($x, $y, $z, $vx, $vy, $vz, $mass) = @_;
    return {x => $x, y => $y, z => $z, vx => $vx, vy => $vy, vz => $vz, mass => $mass};
}

sub make_bodies {
    return [
        body(0.0, 0.0, 0.0, 0.0, 0.0, 0.0, $SOLAR_MASS),
        body(4.84143144246472090e+00, -1.16032004402742839e+00, -1.03622044471123109e-01,
             1.66007664274403694e-03 * $DAYS_PER_YEAR, 7.69901118419740425e-03 * $DAYS_PER_YEAR,
             -6.90460016972063023e-05 * $DAYS_PER_YEAR, 9.54791938424326609e-04 * $SOLAR_MASS),
        body(8.34336671824457987e+00, 4.12479856412430479e+00, -4.03523417114321381e-01,
             -2.76742510726862411e-03 * $DAYS_PER_YEAR, 4.99852801234917238e-03 * $DAYS_PER_YEAR,
             2.30417297573763929e-05 * $DAYS_PER_YEAR, 2.85885980666130812e-04 * $SOLAR_MASS),
        body(1.28943695621391310e+01, -1.51111514016986312e+01, -2.23307578892655734e-01,
             2.96460137564761618e-03 * $DAYS_PER_YEAR, 2.37847173959480950e-03 * $DAYS_PER_YEAR,
             -2.96589568540237556e-05 * $DAYS_PER_YEAR, 4.36624404335156298e-05 * $SOLAR_MASS),
        body(1.53796971148509165e+01, -2.59193146099879641e+01, 1.79258772950371181e-01,
             2.68067772490389322e-03 * $DAYS_PER_YEAR, 1.62824170038242295e-03 * $DAYS_PER_YEAR,
             -9.51592254519715870e-05 * $DAYS_PER_YEAR, 5.15138902046611451e-05 * $SOLAR_MASS)
    ];
}

sub offset_momentum {
    my ($bodies) = @_;
    my ($px, $py, $pz) = (0.0, 0.0, 0.0);
    for my $b (@$bodies) {
        $px += $b->{vx} * $b->{mass};
        $py += $b->{vy} * $b->{mass};
        $pz += $b->{vz} * $b->{mass};
    }
    my $sun = $bodies->[0];
    $sun->{vx} = -$px / $SOLAR_MASS;
    $sun->{vy} = -$py / $SOLAR_MASS;
    $sun->{vz} = -$pz / $SOLAR_MASS;
}

sub energy {
    my ($bodies) = @_;
    my $e = 0.0;
    my $count = scalar @$bodies;
    for my $i (0 .. $count - 1) {
        my $b = $bodies->[$i];
        $e += 0.5 * $b->{mass} * ($b->{vx} * $b->{vx} + $b->{vy} * $b->{vy} + $b->{vz} * $b->{vz});
        for my $j ($i + 1 .. $count - 1) {
            my $b2 = $bodies->[$j];
            my $dx = $b->{x} - $b2->{x};
            my $dy = $b->{y} - $b2->{y};
            my $dz = $b->{z} - $b2->{z};
            $e -= ($b->{mass} * $b2->{mass}) / sqrt($dx * $dx + $dy * $dy + $dz * $dz);
        }
    }
    return $e;
}

sub advance {
    my ($bodies, $dt) = @_;
    my $count = scalar @$bodies;
    for my $i (0 .. $count - 1) {
        my $b = $bodies->[$i];
        for my $j ($i + 1 .. $count - 1) {
            my $b2 = $bodies->[$j];
            my $dx = $b->{x} - $b2->{x};
            my $dy = $b->{y} - $b2->{y};
            my $dz = $b->{z} - $b2->{z};
            my $d2 = $dx * $dx + $dy * $dy + $dz * $dz;
            my $mag = $dt / ($d2 * sqrt($d2));
            $b->{vx} -= $dx * $b2->{mass} * $mag;
            $b->{vy} -= $dy * $b2->{mass} * $mag;
            $b->{vz} -= $dz * $b2->{mass} * $mag;
            $b2->{vx} += $dx * $b->{mass} * $mag;
            $b2->{vy} += $dy * $b->{mass} * $mag;
            $b2->{vz} += $dz * $b->{mass} * $mag;
        }
    }
    for my $b (@$bodies) {
        $b->{x} += $dt * $b->{vx};
        $b->{y} += $dt * $b->{vy};
        $b->{z} += $dt * $b->{vz};
    }
}

sub main {
    my $n = @ARGV ? int($ARGV[0]) : 200000;
    my $t0 = time();
    my $bodies = make_bodies();
    offset_momentum($bodies);
    my $e0 = energy($bodies);
    for (1 .. $n) {
        advance($bodies, 0.01);
    }
    my $e1 = energy($bodies);
    my $t1 = time();
    printf "result: %d,%d\n", floor($e0 * 1e9), floor($e1 * 1e9);
    printf "time: %.3f\n", ($t1 - $t0) * 1000;
}

main();
