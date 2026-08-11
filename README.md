# Maximum Pool (Dreamcast) server hosting

Tools and notes for running your own Maximum Pool game server and meta server.

---

## Credit

The meta server is Shuouma's. He reverse-engineered the protocol and wrote the original. This
version swaps the hardcoded hostname for a config file. Original on the Server Software page at
<https://dreamcastlive.net/>; his copyright header and license ship as-is.

`ultra_server.exe` is Sierra's, so it isn't included here. Grab it from the Dreamcast Live or
Dreamcast-Talk server software pages.

---

Two programs. Clients ask the **meta server** where the games are, then talk to the **game
server** directly.

| | Meta server | Game server |
|---|---|---|
| Binary | `maxpool_meta_server` (Linux, source included) | `ultra_server.exe` (Windows, runs under Wine) |
| Port | UDP 6003 | UDP 35000 |
| Job | Answers "where are the servers" | The gathering room / game itself |

Clients reach the meta server by resolving `coolpool.east.won.net` and `coolpool.west.won.net`.

---

## 1. Game server

### game_guid

Has to be this exact value. It's what tells Maximum Pool apart from Cool Pool, and the client
sends it in every probe. Wrong GUID and the server ignores everything it receives. Blank and it
won't start.

```
game_guid = "E6666EA0-DBB2-11D2-A771-006097C3E986";
```

Most .scs files you'll find are blank templates with Cool Pool's leftovers still in them.

### Minimum ultra_server.scs

```
protocol    = "UDP";
port        = 35000;
game_guid   = "E6666EA0-DBB2-11D2-A771-006097C3E986";
server_name = "My Server";
welcome     = "Welcome";

service_name = "ultra_server";

filter_style    = 2;
clean_word_list = "cleanwords.lst";
dirty_word_list = "dirtywords.lst";

max_players = 32;
max_games   = 8;
```

Delete any `meta_server1` / `meta_server2` lines. They point at the WON Titan registry, dead since
2007, and only produce `Could not find host` noise.

The `.scs` is a script that runs at startup, not an INI file. A parse error leaves everything below
it undefined. Watch for `Input was processed up to line N.`

### Running

```
wine ultra_server.exe -run
```

`-run` is required. With no arguments it prints usage and then tries to attach to the Windows
service manager, which just looks like it quit for no reason. Run it from its own directory, since
the settings filename is hardcoded and resolved against the working directory.

Healthy startup:

```
Server "My Server" started at ...
        Using port 35000 and guid "E6666EA0-DBB2-11D2-A771-006097C3E986"
Game Server Version: 102
Max Players: 32 -- Max Games: 8
```

If you don't see `Creating transport ultra_server failed`, the socket bound. `ss -lunp | grep 35000`
confirms it.

### Telnet console

```
enableTelnet("password", 47807);
```

Max 2 sessions, 60 min idle timeout, 3 failed logins locks out new connections for 60s.

| Command | Does |
|---|---|
| `dir()` | Live player and game tables |
| `sys("msg")` | Broadcast a system message |
| `spam_boot("name")` | Kick a player |
| `dumpAllSym()` | Every symbol with type and argument count |
| `transStats()` | Transport stats |
| `ver()`, `quit()` | Version, shutdown |

`GPF()` and `int3()` deliberately crash the process.

---

## 2. Meta server

Shuouma's original hardcodes one hostname in `create_server_list()`. The version here reads a
list from a file instead, re-read on every request. See **Credit** above.

```
make
cp config/servers.conf.example servers.conf   # then edit it
./maxpool_meta_server -f servers.conf -v
```

`servers.conf`, one server per line, `<host-or-ip> <port>`:

```
pool.example.com 35000
```

Flags: `-p` listen port (default 6003), `-f` config path, `-v` also log packets failing the magic
check.

Check it without a client:

```bash
python3 -c "
import socket,binascii
s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);s.settimeout(3)
s.sendto(bytes([5,2,0,0x66,0,0x0a,0,0,4]),('127.0.0.1',6003))
print(binascii.hexlify(s.recvfrom(1024)[0],' ').decode())"
```

One entry = 21 bytes: 15-byte header, then big-endian port + IPv4. The last 6 bytes are the address
you're handing out. Reading them catches a stale `servers.conf`, which otherwise fails silently:
the client probes a dead address, the list comes up empty, and nothing logs an error.

---

## 3. DNS

Both names must resolve to the meta server:

- `coolpool.east.won.net`
- `coolpool.west.won.net`

**The answer needs a CNAME plus an A record, and a non-zero TTL.** A bare A record gets rejected:
the client resolves the name and then never sends anything to port 6003, with nothing logged
anywhere to tell you why. The original servers answered with `CNAME coolpool.east.` + A, and
matching that works.

Untested which of the two the client actually cares about, the record shape or the TTL. Setting
both works.

`auriga.segasoft.com` gets queried too. It returns NXDOMAIN and the client carries on, so leave it
alone.

Best case these records live in the community DNS that DreamPi already points at, so players don't
have to configure anything. Until then, run your own resolver or override locally.

### dnsmasq example

See `config/dnsmasq-maxpool.conf.example`:

```
local-ttl=300
cname=coolpool.east.won.net,coolpool.east
cname=coolpool.west.won.net,coolpool.west
host-record=coolpool.east,<meta server IP>
host-record=coolpool.west,<meta server IP>
```

On a DreamPi this goes in the Pi's dnsmasq config. Otherwise point the Dreamcast's DNS at whatever
resolver holds the records.

---

## 4. Verifying

```bash
sudo tcpdump -ni any 'udp port 53 or udp port 6003 or udp port 35000'
```

Working sequence:

```
query[A] coolpool.east.won.net    -> meta server IP
UDP  31 bytes  client -> meta:6003
UDP  21 bytes  meta:6003 -> client
UDP  29 bytes  client -> game:35000     (plus broadcasts to 255.255.255.255:35000)
UDP 124 bytes  game:35000 -> client
```

| Chain stops at | Cause |
|---|---|
| No DNS query arrives | Client isn't using your resolver |
| Resolves, no 6003 packet | Bare A record, needs CNAME + non-zero TTL |
| 6003 request, no reply | Meta server not running, or port taken |
| Probes hit 35000, no reply | Wrong or missing `game_guid` |
| Client probes the wrong IP | Stale `servers.conf` |

The 29-byte probe carries the game GUID in its last 16 bytes, little-endian on the first three
fields. If another title ever needs a GUID nobody has written down, capture a probe and read it
straight off the wire:

```bash
sudo tcpdump -ni any -X 'udp port 35000' -c 4
```

---

## Repository layout

```
README.md                             this guide
LICENSE                               this repository's license
LICENSE.upstream                      license as shipped with Shuouma's release
Makefile                              build + install
src/maxpool_meta_server.c             meta server (modified from Shuouma's)
config/servers.conf.example           game servers to advertise
config/ultra_server.scs.example       minimum game server settings
config/dnsmasq-maxpool.conf.example   DNS records clients need
systemd/maxpool-meta-server.service   unit file
docs/ultra_server_reference.md        reverse-engineered notes on ultra_server.exe
```

## Install

```
make
sudo make install
sudo cp config/servers.conf.example /etc/maxpool/servers.conf
sudo $EDITOR /etc/maxpool/servers.conf
sudo systemctl enable --now maxpool-meta-server
```
