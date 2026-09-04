# pathfind - shortest path across a weighted grid (arrays, a binary heap, integer arithmetic)
#
# Builds an n x n grid of terrain costs with a fifth of the cells blocked, then runs Dijkstra's
# algorithm from the top-left corner to the bottom-right with a hand-written binary heap and
# reports the path cost, the number of settled cells, and the path length.

use strict;
use warnings;
use Time::HiRes qw(time);

my $INF = 1e15;

my $seed = 42;

sub rand_int {
    my ($n) = @_;
    $seed = ($seed * 48271) % 2147483647;
    return $seed % $n;
}

sub generate_grid {
    my ($width, $height) = @_;
    my $cells = $width * $height;
    my @grid;
    for my $i (0 .. $cells - 1) {
        my $cost = 1 + rand_int(9);
        $cost = -1 if rand_int(100) < 20;
        $grid[$i] = $cost;
    }
    # The corners and their neighbors stay open so the search always leaves the start
    for my $i (0, 1, $width, $cells - 1, $cells - 2, $cells - 1 - $width) {
        $grid[$i] = 1 if $grid[$i] < 0;
    }
    return \@grid;
}

sub heap_push {
    my ($hd, $hn, $d, $node) = @_;
    push @$hd, $d;
    push @$hn, $node;
    my $i = $#$hd;
    while ($i > 0) {
        my $p = int(($i - 1) / 2);
        last if $hd->[$p] <= $hd->[$i];
        @$hd[$p, $i] = @$hd[$i, $p];
        @$hn[$p, $i] = @$hn[$i, $p];
        $i = $p;
    }
}

sub heap_pop {
    my ($hd, $hn) = @_;
    my $top_d = $hd->[0];
    my $top_n = $hn->[0];
    my $last_d = pop @$hd;
    my $last_n = pop @$hn;
    my $size = scalar @$hd;
    if ($size > 0) {
        $hd->[0] = $last_d;
        $hn->[0] = $last_n;
        my $i = 0;
        while (1) {
            my $left = 2 * $i + 1;
            my $right = $left + 1;
            my $m = $i;
            $m = $left if $left < $size && $hd->[$left] < $hd->[$m];
            $m = $right if $right < $size && $hd->[$right] < $hd->[$m];
            last if $m == $i;
            @$hd[$m, $i] = @$hd[$i, $m];
            @$hn[$m, $i] = @$hn[$i, $m];
            $i = $m;
        }
    }
    return ($top_d, $top_n);
}

sub relax {
    my ($grid, $dist, $prev, $hd, $hn, $d, $u, $v) = @_;
    my $cost = $grid->[$v];
    return if $cost < 0;
    my $nd = $d + $cost;
    if ($nd < $dist->[$v]) {
        $dist->[$v] = $nd;
        $prev->[$v] = $u;
        heap_push($hd, $hn, $nd, $v);
    }
}

sub shortest_path {
    my ($grid, $width, $height) = @_;
    my $cells = $width * $height;
    my $goal = $cells - 1;
    my @dist = ($INF) x $cells;
    my @prev = (-1) x $cells;
    $dist[0] = 0;
    my @hd;
    my @hn;
    heap_push(\@hd, \@hn, 0, 0);
    my $settled = 0;
    while (@hd) {
        my ($d, $u) = heap_pop(\@hd, \@hn);
        next if $d > $dist[$u];
        $settled++;
        last if $u == $goal;
        my $x = $u % $width;
        my $y = int($u / $width);
        relax($grid, \@dist, \@prev, \@hd, \@hn, $d, $u, $u - $width) if $y > 0;
        relax($grid, \@dist, \@prev, \@hd, \@hn, $d, $u, $u + $width) if $y < $height - 1;
        relax($grid, \@dist, \@prev, \@hd, \@hn, $d, $u, $u - 1) if $x > 0;
        relax($grid, \@dist, \@prev, \@hd, \@hn, $d, $u, $u + 1) if $x < $width - 1;
    }
    return "unreachable,$settled" if $dist[$goal] == $INF;
    my $length = 0;
    my $node = $goal;
    while ($node >= 0) {
        $length++;
        $node = $prev[$node];
    }
    return "$dist[$goal],$settled,$length";
}

sub main {
    my $n = @ARGV ? int($ARGV[0]) : 800;
    my $t0 = time();
    my $grid = generate_grid($n, $n);
    my $result = shortest_path($grid, $n, $n);
    my $t1 = time();
    print "result: $result\n";
    printf "time: %.3f\n", ($t1 - $t0) * 1000;
}

main();
