# rmate Protocol

rmate is a client that communicates with a TextMate/VS Code editor over a TCP
connection (default port 52698). The protocol has two phases: the client sends
file data to the editor, then listens for save/close commands back.

## Phase 1: Sending Files to the Editor

After connecting, the client sends one `open` block per file. The wire format:

```
open\n
display-name: hostname:myfile.txt\n
real-path: /home/user/myfile.txt\n
data-on-save: yes\n
re-activate: yes\n
token: /home/user/myfile.txt\n
selection: 5\n
data: 1234\n
<1234 bytes of raw file content>
.\n
```

### Fields

| Field            | Description                                                                 |
|------------------|-----------------------------------------------------------------------------|
| `display-name`   | The name shown in the editor tab                                            |
| `real-path`      | Absolute path on the remote machine                                         |
| `data-on-save`   | When `yes`, the server sends file contents back on save                     |
| `re-activate`    | When `yes`, the editor window is brought to the foreground                  |
| `token`          | Opaque string echoed back by the server to identify the file (set to the filename, or `"-"` for stdin) |
| `selection`      | Optional cursor position: `LINE`, or `LINE:COL` with `-c` / `RMATE_COLUMN=1` |
| `data`           | Byte count of raw file content that follows immediately after this line     |

After the raw file bytes, `\n.\n` terminates the command. Multiple files are
sent as consecutive `open` blocks on the same connection.

## Phase 2: Listening for Server Commands

After all files are sent, the client enters a read loop to process commands
from the server.

### Server Greeting

The first line the server sends is a greeting/identification line (e.g.
`220 rmate-server`). The client reads and discards it before processing
commands.

### State Machine

The client parses server responses using a three-state machine:

```
CMD_HEADER --(greeting line)--> CMD_CMD --(command name)--> CMD_VAR
                                  ^                            |
                                  |                            |
                                  +--(empty line / error)------+
```

- **CMD_HEADER**: Consumes the server greeting line, transitions to CMD_CMD.
- **CMD_CMD**: Waits for a command name (`save` or `close`), transitions to CMD_VAR.
- **CMD_VAR**: Parses `key: value` pairs. An empty line or parse error transitions back to CMD_CMD.

### Save Command

When the user saves a file in the editor, the server sends:

```
save\n
token: /home/user/myfile.txt\n
data: 5678\n
<5678 bytes of new file content>
\n
```

The client matches the `token` to the original filename, reads `data` bytes
(partly from the buffer, the rest directly from the socket), and writes them
to disk. After the data, an empty line ends the command.

### Close Command

When the user closes a tab, the server sends:

```
close\n
token: /home/user/myfile.txt\n
\n
```

No file data is included. The empty line ends the command.

### Connection Teardown

When the server closes the TCP connection (all tabs closed), the client's
`read()` returns 0 and the main loop exits.

## Buffer Management

The client accumulates data from the socket in a fixed-size buffer. TCP is a
byte stream with no message boundaries, so a single `read()` may return a
partial line, a complete line, or multiple lines.

After each `read()`:

1. `handle_cmds` parses as many complete lines (delimited by `\n`) as possible.
2. It returns the total number of bytes consumed.
3. `memmove` shifts any unconsumed trailing bytes (partial line) to the front
   of the buffer.
4. The next `read()` appends after the leftover data, eventually completing the
   partial line.

`memmove` is used instead of `memcpy` because the source and destination
regions can overlap when only a small portion of the buffer was consumed.

## Connection Modes

By default, the client forks into the background after reading stdin (if
applicable) so the shell prompt returns immediately. With `-w`, it stays in
the foreground and blocks until the server closes the connection.
