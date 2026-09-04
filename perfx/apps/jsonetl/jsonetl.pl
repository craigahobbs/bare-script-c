# jsonetl - JSON extract, transform, load (JSON encode and decode, nested containers, grouping)
#
# Builds n customer orders with nested item lists, serializes them to JSON, parses the text back,
# then groups the parsed orders by customer and by SKU to rank revenue and demand.
#
# JSON::PP is the JSON codec in Perl's core distribution; it is written in Perl.

use strict;
use warnings;
use JSON::PP;
use Time::HiRes qw(time);

my @NOTES = ('', 'Leave at door', 'Gift wrap', 'Call on arrival');

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

sub pad3 {
    my ($n) = @_;
    return $n < 10 ? "00$n" : $n < 100 ? "0$n" : "$n";
}

sub generate_orders {
    my ($count) = @_;
    my @orders;
    for my $i (0 .. $count - 1) {
        my $customer = 'C' . pad3(rand_int(500));
        my $day = 1 + rand_int(30);
        my $item_count = 1 + rand_int(5);
        my @items;
        for (1 .. $item_count) {
            my $sku = 'SKU' . pad3(rand_int(200));
            my $qty = 1 + rand_int(5);
            my $price_cents = 100 + rand_int(9900);
            push @items, {sku => $sku, qty => $qty, priceCents => $price_cents};
        }
        my $shipping = rand_int(4) == 0 ? 'express' : 'standard';
        my $note = $NOTES[rand_int(4)];
        push @orders, {id => $i + 1, customer => $customer, date => '2026-09-' . pad2($day), items => \@items,
                       shipping => $shipping, note => $note};
    }
    return \@orders;
}

sub rank {
    my ($map, $count) = @_;
    my @keys = sort { $map->{$b} <=> $map->{$a} || $a cmp $b } keys %$map;
    @keys = @keys[0 .. $count - 1] if @keys > $count;
    return map { "$_:$map->{$_}" } @keys;
}

sub summarize {
    my ($orders) = @_;
    my %revenue_by_customer;
    my %qty_by_sku;
    my ($express, $total_revenue, $item_lines) = (0, 0, 0);
    for my $order (@$orders) {
        my $revenue = 0;
        for my $item (@{$order->{items}}) {
            $revenue += $item->{qty} * $item->{priceCents};
            $qty_by_sku{$item->{sku}} += $item->{qty};
            $item_lines++;
        }
        $revenue_by_customer{$order->{customer}} += $revenue;
        $total_revenue += $revenue;
        $express++ if $order->{shipping} eq 'express';
    }
    my @parts = (scalar(@$orders), $item_lines, $total_revenue, $express, scalar(keys %revenue_by_customer));
    push @parts, rank(\%revenue_by_customer, 5);
    push @parts, rank(\%qty_by_sku, 3);
    return join(',', @parts);
}

sub main {
    my $n = @ARGV ? int($ARGV[0]) : 50000;
    my $t0 = time();
    my $orders = generate_orders($n);
    my $json = JSON::PP->new;
    my $text = $json->encode($orders);
    my $parsed = $json->decode($text);
    my $result = summarize($parsed);
    my $t1 = time();
    print "result: $result\n";
    printf "time: %.3f\n", ($t1 - $t0) * 1000;
}

main();
