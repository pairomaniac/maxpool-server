# Maximum Pool (Dreamcast) server hosting

Tools and notes for running your own Maximum Pool game server and meta server.

---

## Credit

The meta server here is based on **Shuouma's** Max Pool meta server, released
in 2017. He reverse-engineered the protocol and wrote the original — the packet
format, request validation and header layout are his work. The modification in
this repo only replaces a hardcoded hostname with a config file.

His original copyright header is preserved in `src/maxpool_meta_server.c`, and
the upstream license ships as `LICENSE.upstream`.

Original source: the Server Software page at <https://dreamcastlive.net/>

> **Licensing note:** the upstream release is internally inconsistent — it ships
> an MIT `LICENSE` file, while `maxpool_meta_server.c` itself carries a GPLv2
> header. Both are preserved here exactly as distributed and nothing has been
> relicensed. This repository's own `LICENSE` covers the material original to
> it; the meta server remains under whichever of the upstream terms applies.

`ultra_server.exe` is proprietary Sierra Entertainment software and is **not**
included here. Get it from the Dreamcast Live / Dreamcast-Talk server software
pages.

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

Required, and it must be this exact value — it's what distinguishes Maximum Pool from Cool Pool,
and the client sends it in every probe. A wrong GUID means the server receives packets and
silently ignores them; a blank one means it won't start at all.

```
game_guid = "E6666EA0-DBB2-11D2-A771-006097C3E986";
```

Config files floating around are usually blank templates with Cool Pool's leftovers in them.

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

Drop any `meta_server1` / `meta_server2` lines — they point at the dead WON Titan registry and
only produce `Could not find host` noise.

The `.scs` is a script that runs at startup, not an INI file. A parse error leaves everything
below it undefined; watch for `Input was processed up to line N.`

### Running

```
wine ultra_server.exe -run
```

`-run` is required. Without arguments it prints usage and then tries to attach to the Windows
service manager, which looks like it quit for no reason. Run it from its own directory — the
settings filename is hardcoded and resolved relative to the working directory.

Healthy startup:

```
Server "My Server" started at ...
        Using port 35000 and guid "E6666EA0-DBB2-11D2-A771-006097C3E986"
Game Server Version: 102
Max Players: 32 -- Max Games: 8
```

No `Creating transport ultra_server failed` means the socket bound. Confirm with
`ss -lunp | grep 35000`.

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

`servers.conf` — one server per line, `<host-or-ip> <port>`:

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

One entry = 21 bytes: 15-byte header, then big-endian port + IPv4. The last 6 bytes are the
address you're handing out — read them to catch a stale `servers.conf`, which otherwise fails
silently (client probes a dead address, list comes up empty, nothing logs an error).

---

## 3. DNS

Both names must resolve to the meta server:

- `coolpool.east.won.net`
- `coolpool.west.won.net`

**The answer needs a CNAME plus an A record, and a non-zero TTL.** A bare A record is rejected —
the client resolves the name and then never sends anything to port 6003, with nothing logged
anywhere. The original servers answer with `CNAME coolpool.east.` + A, and matching that shape
works.

(Untested which of the two the client actually cares about, the record shape or the TTL. Setting
both works.)

`auriga.segasoft.com` is queried too, gets NXDOMAIN, and the client carries on. Leave it.

Ideally these records live in the community DNS that DreamPi already points at, so players need
no client-side configuration at all. Until then, either run your own resolver or override locally.

### dnsmasq example

See `config/dnsmasq-maxpool.conf.example`:

```
local-ttl=300
cname=coolpool.east.won.net,coolpool.east
cname=coolpool.west.won.net,coolpool.west
host-record=coolpool.east,<meta server IP>
host-record=coolpool.west,<meta server IP>
```

On a DreamPi this goes in the Pi's dnsmasq config. On other setups, point the Dreamcast's DNS at
whatever resolver holds these records.

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
| Resolves, no 6003 packet | Bare A record — needs CNAME + non-zero TTL |
| 6003 request, no reply | Meta server not running, or port taken |
| Probes hit 35000, no reply | Wrong or missing `game_guid` |
| Client probes the wrong IP | Stale `servers.conf` |

The 29-byte probe carries the game GUID in its last 16 bytes, little-endian on the first three
fields. If another title ever needs a GUID nobody has, capture a probe and read it out:

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
