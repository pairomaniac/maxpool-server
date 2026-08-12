# 3D Ultra Server (`ultra_server.exe`) reverse-engineered reference

Derived from static analysis of the shipped binary (PE32, MSVC 6, 3 sections, ~284 KB).
Internal version constant: **102** (printed at startup as `Game Server Version: 102`).
Everything below was read out of the binary's code and data, not from documentation.

---

## 1. Startup model: the `.scs` is a program, not an INI file

Nothing in the shipped files says this, and it explains a lot of the odd behaviour.

`main()` does, in order:

1. Construct a script interpreter.
2. Parse **and execute** `ultra_server.scs`.
3. Read `service_name` out of the resulting symbol table.
4. Parse `argv`.
5. On `-run`, call the server routine, which then **queries named globals from the symbol table** (`port`, `protocol`, `game_guid`, …).

Consequences:

- Top-level statements in the `.scs` run before the server does anything. `traceon()`, `openEventLog()`, `print(name)` in the stock file all execute at parse time, which is why you see the banner immediately.
- Config "keys" are just script globals. `port = 34000 + 1000;` is legal. So is computing a `server_name` from a function call.
- Function definitions in the `.scs` go into the same symbol table, which is why `commands.txt` splits "built-in" from "defined functions".
- A parse error partway through the file leaves everything after it undefined. The interpreter prints `Input was processed up to line %i.` That number is your first debugging tool.

The settings filename is **hardcoded** to `ultra_server.scs` and resolved relative to the current working directory. There is no command-line override. Launch it from its own directory or it dies with:

```
Could not load the settings file "ultra_server.scs"
```

…and returns −1.

---

## 2. Command line

Both `-` and `/` are accepted as the flag prefix, and comparison is case-insensitive.

| Flag | Effect |
|---|---|
| `-run` | Console mode. Remaining arguments after it are passed through to the server routine. |
| `-install` | Registers an NT service using `service_name`, plus an EventLog message-source key under `SYSTEM\CurrentControlSet\Services\EventLog\Application\`. NT only. |
| `-remove` | Stops and deletes that service, removes the registry key. NT only. |
| *(none)* | On NT: prints the usage text, then calls `StartServiceCtrlDispatcherA`. |

That last row catches people out. With no arguments on any NT-derived Windows, it prints usage and then tries to attach to the Service Control Manager, which fails with error 1063 when you launched it yourself. It looks like the program printed help and quit for no reason. So always pass `-run`. (On Win9x the no-argument path fell through to console mode; that branch is dead on anything you'd run today.)

`-install` / `-remove` on a non-NT kernel print `Can only install service under Windows NT.` and exit.

---

## 3. Configuration variables

Types are how the binary reads them: **int** = numeric global, **string** = string global with the stated buffer cap.

| Variable | Type | Default if absent | Notes |
|---|---|---|---|
| `game_guid` | string (48) | none | **Fatal if missing or malformed.** |
| `port` | int | `0` | 0 means the transport can't bind. |
| `protocol` | string (8) | `"UDP"` | |
| `server_name` | string (48) | `"Anon Server"` | Truncated at 47 chars. |
| `welcome` | string (1024) | none | Empty string is treated as absent. |
| `gameID` | int | `4` | Not present in the shipped `.scs` at all. |
| `service_name` | string (256) | `"3D Ultra Server"` | Read before argv parsing. |
| `meta_server1` … `meta_server15` | string | none | Loop runs 1–15, not just 1–2. Each hit logs `Read meta_server name "…"`. |
| `titan_root` | string (256) | `""` | Combined with the next as `/root/directory`. |
| `titan_directory` | string (256) | `""` | |
| `filter_style` | int | `0` | 0 = WON.net, 1 = Beep, 2 = Comic. Any other value creates **no filter object at all**. The shipped comment claiming you can't disable the filter is wrong; `filter_style = 99;` does it. |
| `clean_word_list` | string (256) | `"cleanwords.lst"` | |
| `dirty_word_list` | string (256) | `"dirtywords.lst"` | |
| `max_players` | int | `64` | Clamped to 255. |
| `max_games` | int | `16` | Clamped to 255. |
| `enable_spam_boot` | int | `0` | |
| `spam_chat_chars` | int | `200` | |
| `spam_chat_timeframe` | int | `3000` | Milliseconds. |

### `game_guid` is a hard startup gate

Two separate abort paths, both of which bail out of the whole run routine:

```
Variable game_guid was not set in  settings file "%s"     <- key missing
The value of game_guid could not be parsed as a guid.     <- key present, bad format
The guid should be formatted as XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX
```

The parser is strict: 8 hex, `-`, 4 hex, `-`, 4 hex, `-`, 4 hex, `-`, 12 hex. No braces, no omitted dashes.

A blank `game_guid = "";` hits the second path: the server prints the guid error and never opens a socket. No GUID is embedded anywhere in the executable, so it has to come from the config. Most `.scs` files in circulation are blank templates, with `server_name` and `welcome` empty as well. The correct value for Maximum Pool is `E6666EA0-DBB2-11D2-A771-006097C3E986`.

### `meta_serverN` format

`TCP:host:port`. `TCP:` and `UDP:` prefixes both exist in the binary. Registration is the WON Titan protocol, documented in `docs/protocol.md`: connect, push an update, log `Titan update -- successful: %s.` On failure you get `Failed to connect to: %s (err #%d)` or `Could not find host: %s` and the server carries on. Resolved addresses are cached (`using cached ip for %s = %s`). On shutdown it prints `Waiting to unregister with Titan...`.

None of this is required for clients to connect. It is purely the old directory listing.

---

## 4. Script language

Reconstructed from the parser's own error strings and the stock `.scs`:

- Statements terminated with `;`, `//` to end of line for comments.
- `function name (a, b) { … }`, with `return` and `break`.
- C-style `for (i = 0; i < 40; i = i + 1) { … }` and `if (…) { … }`.
- Two value types, reported as `string` and `double`.
- `+` concatenates. Escapes `\"`, `\n`, `\t` work inside string literals.
- Symbol table is flat and global; `clearSym("name")` removes an entry.

Parser diagnostics you may hit: `unmatched parenthesis`, `syntax error`, `expected ';'`, `expected ')'`, `comma expected, not seen`, `too many arguments in function definition`, `too many arguments for function call`, `could not parse function call`, `Error: illegal break.`

Runtime diagnostics: `Error: variable "%s" is undefined.`, `Error: function "%s" is undefined.`, `Error: function "%s" called with wrong number of arguments.`, `Error: symbol entry for name "%s" is function and not a variable.`

---

## 5. Built-in functions, complete list

Read directly from the interpreter's registration table. This is every builtin the binary registers; `commands.txt` is missing several.

**Conversion / math / misc**
`print` · `istr` · `dstr` · `toint` · `sqrt` · `time` · `pwd` · `ver` · `quit` · `load` · `match`

**Symbol table**
`clearSym` · `dumpSym` · `dumpAllSym` · `dumpSymStats`

**Tracing**
`traceon` · `traceoff` · `setTraceFlag` · `clearTraceFlag`

**Network / transport**
`setReceiveRate` · `setSendRate` · `setPacketSize` · `transParms` · `transStats` · `resetTransStats` · `sessionStats` · `resetSessionStats` · `isSession`

**Telnet**
`enableTelnet` · `disableTelnet` · `dumpTelnet`

**Event log**
`openEventLog` · `writeEventLog` · `closeEventLog` · `listLogFiles` · `dumpLogFile`

**Mail**
`sendMail`

**Debug**
`GPF` · `int3` deliberately crash the process (general protection fault / breakpoint trap). Don't call them on a live server.

**Game-specific, registered separately and absent from `commands.txt`:**

| Function | Args | Effect |
|---|---|---|
| `dir()` | 0 | Dumps the live player and game tables. |
| `sys(msg)` | 1 | Broadcasts a system message. Logs `Broadcast> %s`. |
| `spam_boot(name)` | 1 | Kicks a named player. `Booting player (%s) for spamming.` or `Player (%s) not found.` |

`dir()` output layout:

```
Players:
   id#       game      watch     update#   name
Games:
   id#       host      pass      update#   name
```

Player rows iterate to `max_players`, game rows to `max_games`.

### Getting exact argument counts

No guesswork needed. `dumpAllSym()` walks the symbol table and prints, for each entry:

```
"%s" is a built-in function with %i argument(s)
"%s" is a function with %i argument(s)
"%s" is variable with value "%s"
```

Run it once on your own server for an authoritative signature list, including anything listed above without an arity. `dumpSym("name")` does one symbol; `match("pattern")` filters and reports `Total of %i matches.`

---

## 6. Telnet console

`enableTelnet(password, port)`. The password is arbitrary. Hardcoded behaviour:

- **Maximum 2 concurrent sessions.** A third gets `Connection from %s refused becuase connection limit of 2 has been reached.` (typo is the binary's).
- **60-minute idle watchdog**: `Session inactive for too long.  Watchog logout triggered.`
- **3 consecutive login failures locks out new connections for 60 seconds**: `Too many login failures.  Connections temporarily disabled.` `dumpTelnet()` reports `Last time we had 3 consecutive login failures was: %s` and `There have been %i logins and %i failures.`
- There is also a password-entry timeout: `Connection from %s refused becuase of password timeout.`
- `logout` ends a session.

Once logged in you get the same interpreter as the console, so `dir()`, `transStats()`, `allSessStats()`, `sys("...")` all work remotely. It's the only real view into whether a Dreamcast is establishing a session.

---

## 7. Event log

`openEventLog(maxBytes, baseName)` opens `<baseName><letter>.log` and rotates through single-letter suffixes as files fill. A run of files like `evH.log` through `evP.log` is one per restart. Zero-byte logs across several restarts mean the server has been starting and rotating but never logging an event, consistent with a startup abort (see `game_guid` above) or with no client ever connecting.

- `writeEventLog(str)` appends.
- `listLogFiles()` prints `A total of %i log files of the form "%s" were found.` with name/date/size per file.
- `dumpLogFile(name)` prints contents.
- `closeEventLog()` closes the current one; reopening rotates to the next letter.

---

## 8. Checklist for a non-working server

1. **`game_guid` is blank** → fix this first; nothing else matters until it's a valid GUID.
2. Launch with `-run`, from the binary's own directory.
3. `port` must be non-zero, and must match what your meta server advertises (35000 in the stock config). A bad transport gives `Creating transport %s failed`.
4. `cleanwords.lst` / `dirtywords.lst` must exist unless you set `filter_style` to something outside 0–2.
5. Watch the banner: `Server "…" started at …` then `Using port … and guid "…"` then `Max Players: %d -- Max Games: %d` then `Transport on at address %s`. If you don't reach the transport line, the failure is in config, not networking.
6. Enable telnet and run `dir()` while a Dreamcast is trying to connect.
7. `meta_serverN` failures mean the server isn't registered with a meta server. Harmless if you
   list it by hand instead; see `docs/protocol.md` for the registration exchange.
