# Maximum Pool (Dreamcast) server hosting

Tools and notes for running your own Maximum Pool game server and meta server.

---

## Credit

The meta server here is derived from Shuouma's, on the Server Software page at
<https://dreamcastlive.net/>. His copyright header and license ship as-is.

Packet formats in `docs/protocol.md` were checked against Sierra's own WON/Titan SDK sources,
archived at <https://github.com/drhax9908/world-opponent-network> (an export of the defunct
code.google.com/p/world-opponent-network project, and the root of the WON forks on GitHub). The
message classes used here are `SMsgDirG2MultiEntityReply`, `SMsgDirG2AddService` and `DirEntity`
under `TitanApi/msg/Dir/`. The SDK itself is Sierra's, redistributed by that project without a
stated license; nothing from it is copied into this repository.

Dreamcast-Talk runs the long-running Max Pool room and the fallback server IP list.

`ultra_server.exe` is Sierra's and isn't included. It's on the Dreamcast Live and Dreamcast-Talk
server software pages.

---

Two programs. Clients ask the **meta server** where the games are, then talk to the **game
server** directly.

| | Meta server | Game server |
|---|---|---|
| Binary | `maxpool_meta_server` (Linux, source included) | `ultra_server.exe` (Windows, runs under Wine) |
| Port | UDP 6003 | UDP 35000 |
| Job | Answers "where are the servers" | The gathering room / game itself |
| Also | Accepts self-registration on TCP 15101 | Registers itself at startup |

Clients reach the meta server by resolving `coolpool.east.won.net` and `coolpool.west.won.net`.

---

## 1. Game server

### game_guid

This exact value. It distinguishes Maximum Pool from Cool Pool and the client sends it in every
probe. Wrong and the server ignores everything it receives; blank and it won't start.

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
meta_server1 = "TCP:<meta server IP>:15101";

filter_style    = 2;
clean_word_list = "cleanwords.lst";
dirty_word_list = "dirtywords.lst";

max_players = 32;
max_games   = 8;
```

`meta_server1` points at the meta server, and the game server registers itself there at startup, so
it appears without editing `servers.conf`. Delete `meta_server2`. The stock values are WON hosts
that no longer resolve; `Could not find host` in the log means you aren't registered.

`titan_root` and `titan_directory` set the path it registers under, `/root/directory`. Consoles
query `/CoolPool`, so a different root puts the room somewhere nothing looks. This meta server
ignores the path and lists any registration it receives.

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

`Creating transport ultra_server failed` means the socket didn't bind. `ss -lunp | grep 35000`
confirms it did.

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

The original hardcodes one hostname in `create_server_list()`. This version reads a list from a
file, re-read on every request, accepts self-registration on TCP 15101, and fixes a framing bug
that capped the browser at one room regardless of how many servers were listed
(`docs/protocol.md`).

```
make
cp config/servers.conf.example servers.conf   # then edit it
./maxpool_meta_server -f servers.conf -v
```

`servers.conf`, one server per line, `<host-or-ip> <port>`:

```
pool.example.com 35000
```

Flags: `-p` listen port (default 6003), `-f` config path, `-r` registration port (default 15101,
`-r 0` disables), `-v` also log packets that fail the magic check. Single process, no threads, no
dependencies beyond libc.

Registered servers are listed first, then `servers.conf`, duplicates skipped. Registrations expire
after the lifespan the game server asks for, an hour in practice, so a room that goes away stops
being advertised. `ultra_server.exe` renews its lease before it expires, so a room stays listed for
as long as it's up. Registrations are held in memory, so restarting the meta server clears them
until each game server renews. `servers.conf` is for entries that must always be listed.

Check it without a client:

```bash
python3 -c "
import socket,struct
req=bytes([5,2,0,0x66,0])+struct.pack('<I',0x0400000A)+bytes([0,0])+struct.pack('<H',9)+'/CoolPool'.encode('utf-16-le')
s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);s.settimeout(3)
s.sendto(req,('127.0.0.1',6003));d,_=s.recvfrom(4096)
n=struct.unpack_from('<H',d,12)[0];o=14
for _ in range(n):
    l=d[o] or 6;o+=1
    print(socket.inet_ntoa(d[o+2:o+6]),struct.unpack_from('>H',d,o)[0]);o+=l"
```

Same `DirG2GetDirectory` request a Dreamcast sends. One entry is 21 bytes: a 14-byte header, then a
length byte, big-endian port and IPv4 per server. A stale `servers.conf` fails silently otherwise,
since the client probes a dead address and shows an empty list with nothing logged.

---

## 3. DNS

Both names must resolve to the meta server:

- `coolpool.east.won.net`
- `coolpool.west.won.net`

**The answer needs a CNAME plus an A record, and a non-zero TTL.** A bare A record gets rejected:
the client resolves the name and then never sends anything to port 6003, with nothing logged
anywhere to tell you why. The original servers answered with `CNAME coolpool.east.` + A, and
matching that works.

Untested which the client depends on, the record shape or the TTL. Setting both works.

`auriga.segasoft.com` gets queried too. It returns NXDOMAIN and the client carries on, so leave it
alone.

These records belong in the community DNS that DreamPi already points at, so players need no
client-side setup at all. Until then, run your own resolver or override on the Pi.

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
UDP  21 bytes  meta:6003 -> client        (28 for two rooms, +7 each after)
UDP  29 bytes  client -> game:35000     (plus broadcasts to 255.255.255.255:35000)
UDP 124 bytes  game:35000 -> client
```

| Chain stops at | Cause |
|---|---|
| No DNS query arrives | Client isn't using your resolver |
| Resolves, no 6003 packet | Bare A record, needs CNAME + non-zero TTL |
| 6003 request, no reply | Meta server not running, or port taken |
| Probes hit 35000, no reply | Wrong or missing `game_guid` |
| Client probes the wrong IP | Stale `servers.conf`, or a NAT host announced a private address |
| Only one room ever listed | Meta server predates the framing fix - see `docs/protocol.md` |

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
src/maxpool_meta_server.c             meta server
config/servers.conf.example           static game servers to advertise
config/ultra_server.scs.example       minimum game server settings
config/dnsmasq-maxpool.conf.example   DNS records clients need
scripts/maxpool-titan-listen          decode what a game server sends to 15101
systemd/maxpool-meta-server.service   meta server unit
systemd/maxpool-game.service.example  game server under Wine
docs/protocol.md                      wire formats, and the one-server bug
docs/hosting-a-room.md                short guide for people running a room
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

Open UDP 6003 and TCP 15101 on the meta server host, and UDP 35000 wherever the game server runs.

---

Built with LLM assistance, against real hardware and packet captures. Anything in `docs/protocol.md`
that wasn't verified on the wire says so. Corrections welcome.
