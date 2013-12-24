#!/usr/bin/perl
#
# A very crude example how to turn status output from CWM in to something
# that can be interpreted by dzen2.
#
# Note that there is exactly one pipe per screen which CWM will sent output
# from, so that multiple status lines per screen is easier to handle.  To
# this end, this example script will fork off one instance per screen for
# however many named pipes exist.
#
# NO ATTEMPT has been made yet to support the placement of dzen bars on a
# machine with Xinerama installed; this currently seems to work OK though.
#
# Patches improving this script is welcome; I threw it together in about
# fifteen minutes, there's no support for SIGPIPE or anything like that yet.
#
# -- Thomas Adam, Monday 23rd December 2013

use strict;
use warnings;
use IO::Pipe;

$| = 1;

my @pipes = glob("/tmp/cwm-*.sock");

# If there are no pipes, that's OK.
exit unless @pipes;

my %dzen_options = (
    '-fg' => 'white',
    '-bg' => 'blue',
    '-ta' => 'l',
    '-w'  => '580',
    '-p'  => '',
);

sub send_to_dzen
{
    my ($data, $fh) = @_;
    my $msg;
    my %desks = (
        'nogroup'   => 0,
        'one'       => 1,
        'two'       => 2,
        'three'     => 3,
        'four'      => 4,
        'five'      => 5,
        'six'       => 6,
        'seven'     => 7,
        'eight'     => 8,
        'nine'      => 9,
    );

    foreach my $deskname (@{$data->{'desktops'}}) {
        if (grep /^$deskname$/, @{$data->{'active_desktops'}}) {
            $msg .= "|^bg(#007fff) $desks{$deskname} ^bg()";
        } elsif (grep /^$deskname$/, @{$data->{'urgency'}}) {
            $msg .= "|^bg(#7c8814) $desks{$deskname} ^bg()";
        } else {
            $msg .= "|^fg() $desks{$deskname} ";
        } 
    }

    if (defined $data->{'client'}) {
            $msg .= "|^bg(#7f00ff)^fg()".$data->{'client'};
    }

    print $fh $msg, "\n";
    $fh->flush();
}

sub process_line
{
    my ($fifo, $fh) = @_;
    my %data = ();

    open (my $pipe_fh, '<', $fifo) or die "Cannot open $fifo: $!";
    while (1) {
        while (my $line = <$pipe_fh>) {
            # Each element is pipe-separated.  Key/values are then
            # comma-separated, and multiple values for those are
            # comma-separated.
            %data = map {
                my ($k, $v) = split /:/, $_, 2;
		defined $v ? ($k => $v) : ($k => '');
            } split(/\|/, $line);

            # For those entries we know might contain more than one element,
            # convert the comma-separated list in to an array.
            exists $data{$_} and $data{$_} = [split /,/, $data{$_}]
                for (qw/urgency desktops active_desktops/);
            send_to_dzen(\%data, $fh);
        }
    }
    close ($pipe_fh);
}

foreach (@pipes) {
    if (fork()) {
        # XXX: Close certain filehandles here; STDERR, etc.
        my $dzen_cmd = "dzen2 " . join(" ", map { $_ . " $dzen_options{$_}" }
            keys(%dzen_options));
        my $dzen_pipe = IO::Pipe->new();
        $dzen_pipe->writer($dzen_cmd);
 
        process_line($_, $dzen_pipe);
    }
}