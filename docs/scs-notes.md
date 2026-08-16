# ultra_server.scs notes

Detail behind the minimum config in [hosting-a-room.md](hosting-a-room.md).
Nothing here is needed to run a room.

## The parser

From 2000 and unforgiving.

- Comments are `//`, but a line starting with `//` that also contains a second
  `//` is a syntax error. The stock file ships with lines like
  `//meta_server1 = "..."; //don't change`, which is why re-commenting them
  breaks the parse. Delete lines instead of commenting them out.
- Line endings must be CRLF. An editor converting to LF breaks the parse.
- `Parse error line N` is accurate about the line. `cat -A` shows `^M$` for
  CRLF and `$` for LF, and makes trailing whitespace visible.

## Settings that matter

**`game_guid`** identifies Maximum Pool and appears in every client probe.
Wrong and the server ignores everything it receives; blank and it won't start.
`E6666EA0-DBB2-11D2-A771-006097C3E986`.

**`meta_server1`** takes `TCP:host:15101`, hostname or IP. The stock values
point at WON hosts that stopped resolving long ago. `meta_server2` is a second
registration target and can be left out.

**`titan_root` and `titan_directory`** must both be present or the server never
registers — but the values are arbitrary, since they only set the path the
entry is filed under. `CoolPool` is what consoles query, so there's no reason
to change it.

**`service_name`** is for the binary's own NT service support and must be
unique per server on a machine. It isn't the name of a systemd or NSSM service
wrapping it.

**`filter_style`** picks the chat filter (0 WON, 1 beep, 2 comic). There's no
way to disable filtering, and `clean_word_list` / `dirty_word_list` must point
at files that exist.

**`max_players` / `max_games`** default to 64 and 16. Values above 255 are
truncated.

## Optional pieces in the stock file

`enableTelnet("password", 47807)` opens a remote console. `openEventLog`,
`traceon`, `clearTraceFlag` and the helper functions (`resetAllStats`,
`allSessStats`, `evcycle`, `evfill`, `cls`) are debugging aids from Sierra's
own testing. The spam-boot block (`enable_spam_boot`, `spam_chat_chars`,
`spam_chat_timeframe`) kicks players for flooding chat.

The file is a script, not a config format: `print`, string concatenation,
`istr()`, `time()` and user-defined functions all work, which is how the stock
file builds its startup banner.

## Registration

What the server sends on startup, and how the meta server answers, is in
[protocol.md](protocol.md).
