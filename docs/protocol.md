# The Max Pool meta server protocol

Maximum Pool's server browser speaks WON Titan, the directory service Sierra
used across its 1999-2001 online titles. Formats below are checked against
Sierra's Titan SDK sources (see Credit in the README) and live captures.

Little-endian unless noted. Nothing is encrypted or authenticated.

## Ports

| Port | Proto | Between | Purpose |
|---|---|---|---|
| 6003 | UDP | console -> meta server | "where are the servers" |
| 15101 | TCP | game server -> meta server | self-registration |
| 35000 | UDP | console -> game server | the game itself |

## 1. The lookup (UDP 6003)

The console sends `DirG2GetDirectory` for the path `/CoolPool`:

```
05                 header type 5 = HeaderService2Message2 (plain SmallMessage)
02 00              service 2     = SmallDirServerG2
66 00              message 102   = DirG2GetDirectory
0a 00 00 04        flags = GF_DECOMPSERVICES | GF_DECOMPRECURSIVE
                         | GF_SERVADDNETADDR
00 00              (query fields)
09 00              string length in CHARACTERS, not bytes
2f 00 43 00 ...    "/CoolPool" as UTF-16LE
```

31 bytes total.

The reply is `SMsgDirG2MultiEntityReply`:

```
05                 header type
02 00              service 2
00 00              message type  (SDK says 3 = DirG2MultiEntityReply; every
                                 deployed server sends 0 and clients accept it)
00 00              status = StatusCommon_Success
00                 sequence byte, high bit = last reply
0a 00 00 04        flags, echoing the request
NN NN              entity count
```

That header is **14 bytes**. Then one entity per server, packed by
`DirEntity::Pack` with `GF_SERVADDNETADDR` set:

```
06                 netaddress length in bytes
88 b8              port, BIG-endian (35000)
5f d8 cb 7f        IPv4, big-endian
```

7 bytes per entity. One server is 14 + 7 = 21 bytes; two is 28.

### The one-server bug

The original treats the header as 15 bytes and appends 6 per server. That works
for exactly one server, because the fifteenth byte of a one-entry reply *is*
the first entity's length prefix. With two or more, every entity after the
first is 6 bytes wide instead of 7, so the client reads entity two a byte early
and stops.

Symptom: one room listed regardless of how many are configured, no error
anywhere. It survived because the community only ever ran one room.

## 2. Self-registration (TCP 15101)

`meta_server1` / `meta_server2` in `ultra_server.scs` point here. The stock
values are the dead WON hosts, so the path went unused; the game server speaks
it in the clear.

Frames are prefixed with a 4-byte little-endian length **which counts itself**.
Strings are `PW_STRING`: a 16-bit character count, then UTF-16LE, so byte
length is double the count.

The sequence, each message on its own connection, closed after the reply:

1. An empty connection - a reachability probe.
2. `DirG2AddDirectory` (200), creating `/CoolPool`:

```
05 02 00 c8 00     header, service 2, message 200
02                 entity flags
09 00 "/CoolPool"          path
0b 00 "Cool Pool 1"        name
0b 00 "Cool Pool 1"        display name
00 00 00 00                lifespan
```

3. The server waits for a `DirG2StatusReply` before continuing. Without one,
   registration stops here.

```
0b 00 00 00        frame length, counts itself
05 02 00           header, service 2
01 00              message 1 = DirG2StatusReply
00 00              StatusCommon_Success
```

4. `DirG2AddService` (202), which carries the actual address:

```
05 02 00 ca 00     header, service 2, message 202
08                 entity flags
15 00 "/CoolPool/Cool Pool 1"    path      \
0b 00 "SEGA Online"              name       > the entity key
06 88 b8 5f d8 cb 7f             netaddress/
0b 00 "SEGA Online"              display name
10 0e 00 00                      lifespan = 3600s
```

Field order: for a service the netaddress is part of the key, so it sits
between the name and the display name, not after the lifespan.

The announced address comes from the `.scs`, not the socket, so a host behind
NAT announces an unreachable private address. Prefer the connection's peer
address, falling back to the announced one.

Whether the binary sends `DirG2RenewService` (205) at the one hour mark is
unconfirmed.

## Message types seen so far

| Value | Name | Direction |
|---|---|---|
| 1 | DirG2StatusReply | server -> game server |
| 3 | DirG2MultiEntityReply | server -> console (sent as 0 in practice) |
| 102 | DirG2GetDirectory | console -> server |
| 200 | DirG2AddDirectory | game server -> server |
| 202 | DirG2AddService | game server -> server |
| 205 | DirG2RenewService | expected, unconfirmed |
| 209 | DirG2RemoveService | expected, unconfirmed |

## 3. The game probe (UDP 35000)

29 bytes, ending in the 16-byte game GUID (little-endian on the first three
fields). The game server answers with 124 bytes carrying the room name and
player counts. A wrong or missing `game_guid` produces silence, which looks
identical to the server being down.

Consoles also broadcast the probe to `255.255.255.255:35000` for LAN discovery.

## Tools

`scripts/maxpool-titan-listen --reply` stands in for a meta server on TCP 15101
and decodes what a game server sends. The README has a one-liner for querying
UDP 6003 and decoding the reply.
