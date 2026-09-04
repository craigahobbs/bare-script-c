# loganalyze - web server log analysis (regular expressions, string splitting, hash maps)
#
# Generates n lines of an Apache-style access log with a deterministic PRNG, then parses every
# line with a regular expression and aggregates: unique client addresses, status code counts,
# requests per hour, bytes per path prefix, and the busiest hour and heaviest paths.

use strict;
use warnings;
use Time::HiRes qw(time);

my @WORDS = ('perf', 'bare', 'script', 'lua', 'ruby', 'perl', 'python', 'node');
my @SLUGS = ('hello-world', 'release-notes', 'faq', 'roadmap', 'benchmarks');
my $LINE_RE = qr/^(\S+) \S+ \S+ \[(\d+)\/(\w+)\/(\d+):(\d+):(\d+):(\d+) [^\]]+\] "(\w+) (\S+) [^"]+" (\d+) (\d+)$/;

my $seed = 42;

sub rand_int {
    my ($n) = @_;
    $seed = ($seed * 48271) % 2147483647;
    return $seed % $n;
}

sub pad2 {
    my ($n) = @_;
    return $n < 10 ? "0$n" : "$n";
}

sub generate_log {
    my ($count) = @_;
    my @lines;
    for (1 .. $count) {
        my $ip = '10.' . rand_int(8) . '.' . rand_int(256) . '.' . rand_int(256);
        my $day = 1 + rand_int(30);
        my $hour = rand_int(24);
        my $minute = rand_int(60);
        my $second = rand_int(60);
        my $method = rand_int(10) == 0 ? 'POST' : 'GET';
        my $kind = rand_int(12);
        my $path;
        if ($kind == 0) {
            $path = '/';
        } elsif ($kind == 1) {
            $path = '/index.html';
        } elsif ($kind == 2) {
            $path = '/api/users';
        } elsif ($kind == 3) {
            $path = '/api/users/' . rand_int(1000);
        } elsif ($kind == 4) {
            $path = '/api/orders';
        } elsif ($kind == 5) {
            $path = '/static/app.js';
        } elsif ($kind == 6) {
            $path = '/static/style.css';
        } elsif ($kind == 7) {
            $path = '/images/logo.png';
        } elsif ($kind == 8) {
            $path = '/login';
        } elsif ($kind == 9) {
            $path = '/logout';
        } elsif ($kind == 10) {
            $path = '/search?q=' . $WORDS[rand_int(8)];
        } else {
            $path = '/blog/' . $SLUGS[rand_int(5)];
        }
        my $pick = rand_int(100);
        my $status;
        if ($pick < 80) {
            $status = 200;
        } elsif ($pick < 88) {
            $status = 304;
        } elsif ($pick < 95) {
            $status = 404;
        } elsif ($pick < 98) {
            $status = 302;
        } else {
            $status = 500;
        }
        my $size = rand_int(50000);
        $size = 0 if $status == 304;
        push @lines, "$ip - - [" . pad2($day) . '/Sep/2026:' . pad2($hour) . ':' . pad2($minute) . ':' . pad2($second) .
                     " +0000] \"$method $path HTTP/1.1\" $status $size";
    }
    return join("\n", @lines);
}

sub path_key {
    my ($path) = @_;
    my $q = index($path, '?');
    $path = substr($path, 0, $q) if $q >= 0;
    my @parts = split(/\//, $path, -1);
    $path = "/$parts[1]/$parts[2]" if @parts > 3;
    return $path;
}

sub analyze {
    my ($text) = @_;
    my ($total, $bad, $posts, $errors, $total_bytes) = (0, 0, 0, 0, 0);
    my %ips;
    my %statuses;
    my @hours = (0) x 24;
    my %path_bytes;
    for my $line (split(/\n/, $text)) {
        if ($line !~ $LINE_RE) {
            $bad++;
            next;
        }
        my ($ip, $hour, $method, $path, $status, $size) = ($1, $5 + 0, $8, $9, $10 + 0, $11 + 0);
        $total++;
        $ips{$ip} = 1;
        $statuses{$status}++;
        $hours[$hour]++;
        $posts++ if $method eq 'POST';
        $errors++ if $status >= 500;
        $total_bytes += $size;
        my $key = path_key($path);
        $path_bytes{$key} += $size;
    }

    my $peak_hour = 0;
    for my $hour (0 .. 23) {
        $peak_hour = $hour if $hours[$hour] > $hours[$peak_hour];
    }
    my @top = sort { $path_bytes{$b} <=> $path_bytes{$a} || $a cmp $b } keys %path_bytes;
    @top = @top[0 .. 2] if @top > 3;
    my @parts = ($total, $bad, scalar(keys %ips), $posts, $errors, $total_bytes, $statuses{200} // 0, $statuses{304} // 0,
                 $statuses{404} // 0, "$peak_hour:$hours[$peak_hour]");
    push @parts, "$_:$path_bytes{$_}" for @top;
    return join(',', @parts);
}

sub main {
    my $n = @ARGV ? int($ARGV[0]) : 300000;
    my $t0 = time();
    my $text = generate_log($n);
    my $result = analyze($text);
    my $t1 = time();
    print "result: $result\n";
    printf "time: %.3f\n", ($t1 - $t0) * 1000;
}

main();
