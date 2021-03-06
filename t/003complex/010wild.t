
# This tests wildcard labels

use _GDT ();
use FindBin ();
use File::Spec ();
use Test::More tests => 9;

my $standard_soa = 'example.org 43200 SOA ns1.example.org r00t.example.net 1 7200 1800 259200 120';

my $pid = _GDT->test_spawn_daemon(File::Spec->catfile($FindBin::Bin, 'gdnsd.conf'));

_GDT->test_dns(
    qname => 'xxx.example.org', qtype => 'AAAA',
    answer => 'xxx.example.org 43201 AAAA ::1',
);

_GDT->test_dns(
    qname => 'xxx.example.org', qtype => 'A',
    auth => $standard_soa,
    addtl => 'xxx.example.org 43201 AAAA ::1',
);

_GDT->test_dns(
    qname => 'xxx.xxx.example.org', qtype => 'A',
    auth => $standard_soa,
    addtl => 'xxx.xxx.example.org 43201 AAAA ::1',
);

_GDT->test_dns(
    qname => 'xxx.foo.example.org', qtype => 'AAAA',
    header => { rcode => 'NXDOMAIN' },
    auth => $standard_soa,
    stats => [qw/udp_reqs nxdomain/],
);

_GDT->test_dns(
    qname => 'xxx.*.example.org', qtype => 'AAAA',
    header => { rcode => 'NXDOMAIN' },
    auth => $standard_soa,
    stats => [qw/udp_reqs nxdomain/],
);

_GDT->test_dns(
    qname => 'bar.example.org', qtype => 'AAAA',
    answer => [
        'bar.example.org 43201 CNAME bar.baz.fox.example.org',
        'bar.baz.fox.example.org 43201 AAAA ::1'
    ],
);

_GDT->test_dns(
    qname => 'barmx.example.org', qtype => 'MX',
    answer => 'barmx.example.org 43201 MX 0 barmx.xmrab.xxx.fox.example.org',
    addtl => 'barmx.xmrab.xxx.fox.example.org 43201 AAAA ::1',
);

_GDT->test_kill_daemon($pid);
