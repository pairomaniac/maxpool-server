# Hosting a Maximum Pool room

Run `ultra_server.exe` under Wine, point it at a meta server, and it registers
itself. Players see your room in the in-game list.

Ask whoever runs the meta server for its address. Below it's `<master>`.

## 1. Ports

| Port | Proto | Direction | For |
|---|---|---|---|
| 35000 | UDP | inbound | the game itself |
| 15101 | TCP | outbound | registering |

Open them in the OS firewall and in any provider firewall.

```bash
sudo ufw allow proto udp from any to any port 35000
```

Behind NAT, forward UDP 35000 to the machine. The meta server registers the
address your connection comes from, so a static IP isn't needed.

## 2. Wine

```bash
sudo dpkg --add-architecture i386        # Debian/Ubuntu
sudo apt update
sudo apt install --install-recommends wine wine32:i386

export WINEPREFIX=$HOME/maxpool/wine WINEARCH=win32
wine wineboot -u
```

The prefix must be 32-bit. `wineboot` isn't a standalone command; run it
through `wine`.

## 3. ultra_server.scs

```
game_guid    = "E6666EA0-DBB2-11D2-A771-006097C3E986";
port         = 35000;
meta_server1 = "TCP:<master>:15101";
```

The GUID has to be exact or the server won't start. Delete `meta_server2`. If
you set `titan_root`, it must be `CoolPool`: that's the directory consoles
query.

Comments are `//`, but a line starting with `//` that contains a second `//`
is a syntax error, so delete lines rather than commenting them out. Line
endings must stay CRLF. On `Parse error line N`, check that line with `cat -A`.

## 4. Run it

```bash
cd ~/maxpool/game
wine ultra_server.exe -run
```

`Could not find host: coolpool01.no-ip.info` means `meta_server1` still points
at the dead WON hosts, so you aren't registered.

For a service, use `systemd/maxpool-game.service.example`. `ExecStopPost` is
the part not to drop: wineserver outlives the exe, and without killing it a
restart can come up half-broken.

## 5. Check you're listed

```bash
python3 -c "
import socket,struct
req=bytes([5,2,0,0x66,0])+struct.pack('<I',0x0400000A)+bytes([0,0])+struct.pack('<H',9)+'/CoolPool'.encode('utf-16-le')
s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);s.settimeout(3)
s.sendto(req,('MASTER_IP',6003));d,_=s.recvfrom(4096)
n=struct.unpack_from('<H',d,12)[0];o=14
for _ in range(n):
    l=d[o] or 6;o+=1
    print(socket.inet_ntoa(d[o+2:o+6]),struct.unpack_from('>H',d,o)[0]);o+=l"
```

You should appear within seconds of starting. If not, check the meta server is
reachable: `nc -vz <master> 15101`.

Registrations carry a one hour lease, so stopping your server drops it from the
list on its own. Whether the binary renews the lease is unconfirmed; if your
room vanishes after an hour of uptime, restart it and say so.

## 6. Players

Players need `coolpool.east.won.net` and `coolpool.west.won.net` resolving to
the meta server. See the README for the records; on a DreamPi they go in the
Pi's dnsmasq config.
