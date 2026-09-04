# salesreport - CSV parsing, per-group statistics, and a fixed-width text report
#
# Generates n rows of sales records as CSV text (with a quoted field), parses it with a
# character-level CSV reader, computes revenue, mean, standard deviation, and the top product and
# sales rep per region, and renders an aligned text report with formatted money amounts.

use strict;
use warnings;
use POSIX qw(floor);
use Time::HiRes qw(time);

my @REGIONS = ('North', 'South', 'East', 'West', 'Central', 'Overseas');
my @PRODUCTS = ('Anvil', 'Bolt Kit', 'Cable Tie', 'Drill', 'Epoxy', 'Fan Belt', 'Gasket', 'Hinge', 'Impeller', 'Jack',
                'Kettle', 'Lamp', 'Motor', 'Nozzle', 'O-Ring', 'Pump', 'Quill', 'Rivet', 'Spring', 'Valve');
my @FIRSTS = ('Ada', 'Ben', 'Cleo', 'Dev', 'Eve', 'Finn', 'Gus', 'Hana');
my @LASTS = ('Ng', 'Ortiz', 'Patel', 'Quinn', 'Rossi');
my @REPS = map { $LASTS[$_ % 5] . ', ' . $FIRSTS[int($_ / 5)] } 0 .. 39;

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

sub generate_csv {
    my ($count) = @_;
    my @lines = ('id,date,region,product,units,price_cents,rep');
    for my $i (1 .. $count) {
        my $month = 1 + rand_int(12);
        my $day = 1 + rand_int(28);
        my $region = $REGIONS[rand_int(6)];
        my $product = $PRODUCTS[rand_int(20)];
        my $units = 1 + rand_int(50);
        my $price_cents = 500 + rand_int(49500);
        my $rep = $REPS[rand_int(40)];
        push @lines, "$i,2026-" . pad2($month) . '-' . pad2($day) . ",$region,$product,$units,$price_cents,\"$rep\"";
    }
    return join("\n", @lines);
}

sub parse_csv {
    my ($text) = @_;
    my @rows;
    my @fields;
    my $field = '';
    my $in_quotes = 0;
    my $size = length($text);
    my $i = 0;
    while ($i < $size) {
        my $ch = substr($text, $i, 1);
        if ($in_quotes) {
            if ($ch eq '"') {
                if ($i + 1 < $size && substr($text, $i + 1, 1) eq '"') {
                    $field .= '"';
                    $i++;
                } else {
                    $in_quotes = 0;
                }
            } else {
                $field .= $ch;
            }
        } elsif ($ch eq '"') {
            $in_quotes = 1;
        } elsif ($ch eq ',') {
            push @fields, $field;
            $field = '';
        } elsif ($ch eq "\n") {
            push @fields, $field;
            push @rows, [@fields];
            @fields = ();
            $field = '';
        } else {
            $field .= $ch;
        }
        $i++;
    }
    if ($field ne '' || @fields) {
        push @fields, $field;
        push @rows, [@fields];
    }
    return \@rows;
}

sub pad_left {
    my ($text, $width) = @_;
    $text = "$text";
    return length($text) < $width ? (' ' x ($width - length($text))) . $text : $text;
}

sub pad_right {
    my ($text, $width) = @_;
    $text = "$text";
    return length($text) < $width ? $text . (' ' x ($width - length($text))) : $text;
}

sub money {
    my ($cents) = @_;
    my $dollars = '' . int($cents / 100);
    my $out = '';
    my $count = 0;
    for (my $i = length($dollars) - 1; $i >= 0; $i--) {
        $out = substr($dollars, $i, 1) . $out;
        $count++;
        $out = ',' . $out if $count % 3 == 0 && $i > 0;
    }
    return '$' . $out . '.' . pad2($cents % 100);
}

sub top_entry {
    my ($counts) = @_;
    my $best;
    my $best_value = -1;
    for my $key (keys %$counts) {
        my $value = $counts->{$key};
        if ($value > $best_value || ($value == $best_value && $key lt $best)) {
            $best = $key;
            $best_value = $value;
        }
    }
    return $best;
}

sub report {
    my ($rows) = @_;
    my %stats;
    for my $region (@REGIONS) {
        $stats{$region} = {rows => 0, units => 0, revenue => 0, revenues => [], products => {}, reps => {}};
    }
    my $data_rows = 0;
    for my $ix (1 .. $#$rows) {
        my $fields = $rows->[$ix];
        my $region = $fields->[2];
        my $product = $fields->[3];
        my $units = $fields->[4] + 0;
        my $price_cents = $fields->[5] + 0;
        my $rep = $fields->[6];
        my $revenue = $units * $price_cents;
        my $s = $stats{$region};
        $s->{rows}++;
        $s->{units} += $units;
        $s->{revenue} += $revenue;
        push @{$s->{revenues}}, $revenue;
        $s->{products}{$product} += $revenue;
        $s->{reps}{$rep} += $revenue;
        $data_rows++;
    }

    my @lines = ("Sales report: $data_rows rows", '',
                 pad_right('Region', 10) . pad_left('Rows', 7) . pad_left('Units', 8) . pad_left('Revenue', 16) .
                 pad_left('Mean', 12) . pad_left('Std dev', 12) . '  ' . pad_right('Top product', 12) . 'Top rep');
    my ($total_rows, $total_units, $total_revenue) = (0, 0, 0);
    for my $region (@REGIONS) {
        my $s = $stats{$region};
        next if $s->{rows} == 0;
        my $mean_f = $s->{revenue} / $s->{rows};
        my $variance = 0.0;
        for my $revenue (@{$s->{revenues}}) {
            my $diff = $revenue - $mean_f;
            $variance += $diff * $diff;
        }
        $variance = $variance / $s->{rows};
        my $std = floor(sqrt($variance));
        my $mean = int($s->{revenue} / $s->{rows});
        push @lines, pad_right($region, 10) . pad_left($s->{rows}, 7) . pad_left($s->{units}, 8) .
                     pad_left(money($s->{revenue}), 16) . pad_left(money($mean), 12) . pad_left(money($std), 12) . '  ' .
                     pad_right(top_entry($s->{products}), 12) . top_entry($s->{reps});
        $total_rows += $s->{rows};
        $total_units += $s->{units};
        $total_revenue += $s->{revenue};
    }
    push @lines, pad_right('Total', 10) . pad_left($total_rows, 7) . pad_left($total_units, 8) . pad_left(money($total_revenue), 16);
    return join("\n", @lines);
}

sub hash_text {
    my ($text) = @_;
    my $h = 5381;
    for my $code (unpack('C*', $text)) {
        $h = ($h * 33 + $code) % 4294967296;
    }
    return $h;
}

sub main {
    my $n = @ARGV ? int($ARGV[0]) : 100000;
    my $t0 = time();
    my $text = generate_csv($n);
    my $rows = parse_csv($text);
    my $output = report($rows);
    my $t1 = time();
    print 'result: ' . (scalar(@$rows) - 1) . ',' . hash_text($output) . ',' . length($output) . "\n";
    printf "time: %.3f\n", ($t1 - $t0) * 1000;
}

main();
