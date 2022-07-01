#!/usr/bin/perl
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

use strict;
use warnings;
use File::Temp;

sub get_method_text {
    my $text = '';
    while (<>) {
        if (/^Class #/) {
            $text .= "    #\n";
            next;
        }
        next if /^Processing/;
        next if /^Opened/;
        next if /^  source_file_idx   :/;
        next if /line=/;
        s/^    #\d+/    #/;
        if (/^  Direct methods/ or /^  Virtual methods/) {
            $text .= "\n";
            next;
        }
        my $pipeidx = index($_, '|');
        if ($pipeidx != -1) {
            my $spaceidx = index($_, ' ', $pipeidx);
            $_ = substr($_, $spaceidx);
        }
        my $atidx = rindex($_, '@');
        if ($atidx != -1) {
            $_ = substr($_, 0, $atidx) . "\n";
        }
        $text .= $_;
    }
    my @methods = split(/    #/, $text);
    @methods = grep { / code / } @methods;
    my %method_text = ();
    for my $m (@methods) {
        my @mm = split(/\n/, $m);
        my $cls = substr($mm[0], index($mm[0], 'L'), -1);
        my $name = substr($mm[1], index($mm[1], "'") + 1, -1);
        my $type = substr($mm[2], index($mm[2], "'") + 1, -1);
        $method_text{$cls.$name.$type} = $m;
    }
    return %method_text;
}

my %basemethods = get_method_text();
for my $k (sort keys %basemethods) {
    my $v = $basemethods{$k};
    print "$k\n$v\n";
}
