# Hosting a Maximum Pool room

Point `ultra_server.exe` at a meta server and your room shows up in the in-game
browser. Runs natively on Windows, or under Wine elsewhere.

You need the server files (`ultra_server.exe`, `ultra_server.scs`,
`cleanwords.lst`, `dirtywords.lst`) from the Dreamcast Live or Dreamcast-Talk
server software pages.

Meta server used below: `shumania.ddns.net`.

## 1. ultra_server.scs

Replace the stock file with this, changing `server_name`:

```
protocol   = "UDP";
port       = 35000;
game_guid  = "E6666EA0-DBB2-11D2-A771-006097C3E986";

server_name  = "My Pool Room";
welcome      = "Welcome!";
service_name = "ultra_server";

meta_server1 = "TCP:shumania.ddns.net:15101";

titan_root      = "CoolPool";
titan_directory = "Cool Pool 1";

filter_style    = 2;
clean_word_list = "cleanwords.lst";
dirty_word_list = "dirtywords.lst";

max_players = 32;
max_games   = 8;

traceon ();
print ("Server \"" + server_name + "\" started on port " + istr(port) + ".\n");
```

Save it with CRLF line endings. If you get `Parse error line N`, that's usually
LF endings or an edited comment — see [scs-notes.md](scs-notes.md).

## 2. Windows

Open the port, from an admin prompt:

```
netsh advfirewall firewall add rule name="Max Pool" dir=in action=allow protocol=UDP localport=35000
```

Run it from the folder with the files:

```
ultra_server.exe -run
```

Nothing to install. `-run` is required — without it the binary prints usage and
tries to attach to the Service Control Manager, which looks like it quit for no
reason.

To keep it running across reboots, register it as a service with its own
built-in support, from the same folder:

```
ultra_server.exe -install
net start "ultra_server"
```

The service is named by `service_name` in the .scs, so change that first if you
run more than one on a machine. `ultra_server.exe -remove` unregisters it.

## 3. Linux and macOS

Wine needs 32-bit support.

```bash
# Debian / Ubuntu
sudo dpkg --add-architecture i386 && sudo apt update
sudo apt install --install-recommends wine wine32:i386

# Fedora
sudo dnf install wine wine-core.i686

# Arch: enable multilib, then
sudo pacman -S wine

# macOS
brew install --cask --no-quarantine wine-stable
```

Create a 32-bit prefix as the user that will run the server:

```bash
export WINEPREFIX=$HOME/maxpool/wine WINEARCH=win32
wine wineboot -u
```

Then:

```bash
sudo ufw allow proto udp from any to any port 35000
cd ~/maxpool/game
wine ultra_server.exe -run
```

To run it as a service, use `systemd/maxpool-game.service.example` from this
repository.

## 4. Check it worked

Behind NAT, forward UDP 35000 to the machine first. You don't need a static IP.

Your room should appear in the in-game browser within seconds of starting. To
check without a Dreamcast, ask the meta server what it's listing:

```bash
python3 -c "
import socket,struct
req=bytes([5,2,0,0x66,0])+struct.pack('<I',0x0400000A)+bytes([0,0])+struct.pack('<H',9)+'/CoolPool'.encode('utf-16-le')
s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);s.settimeout(3)
s.sendto(req,('shumania.ddns.net',6003));d,_=s.recvfrom(4096)
n=struct.unpack_from('<H',d,12)[0];o=14
for _ in range(n):
    l=d[o] or 6;o+=1
    print(socket.inet_ntoa(d[o+2:o+6]),struct.unpack_from('>H',d,o)[0]);o+=l"
```

If your address isn't there:

| Symptom | Cause |
|---|---|
| `Could not find host: coolpool01...` | `meta_server1` still points at the old WON hosts |
| Starts, never appears | `titan_root` or `titan_directory` missing — both are required |
| `Parse error line N` | see [scs-notes.md](scs-notes.md) |
| Nothing obvious | meta server unreachable on 15101: `nc -vz shumania.ddns.net 15101` |

Stopping your server removes it from the browser immediately.

## 5. Players

Players need `coolpool.east.won.net` and `coolpool.west.won.net` resolving to
the meta server — see the DNS section of the README.
