# rmate.c

A lightweight C implementation of [rmate](https://github.com/textmate/rmate),
the remote editing client for TextMate, VS Code (via
[Remote VSCode](https://marketplace.visualstudio.com/items?itemName=rafaelmaiolla.remote-vscode)),
Sublime Text (via
[RemoteSubl](https://github.com/randy3k/RemoteSubl)), and other editors that speak the rmate protocol.

`rmate` lets you open files on a remote server in your local editor over an SSH
tunnel. The C version is a drop-in replacement for the
[Bash](https://github.com/aurora/rmate),
[Python](https://github.com/sclukey/rmate-python), and
[Ruby](https://github.com/textmate/rmate) versions, with significantly faster
startup, lower memory usage, and zero runtime dependencies.

## Quick Start

```sh
# Build
make

# Install (default: /usr/local/bin; override with PREFIX=/usr)
sudo make install

# Open a file
rmate myfile.txt

# Read from stdin, edit, write result to stdout
echo "hello" | rmate -w -
```

The editor must be listening on the rmate port (default 52698). For SSH usage,
set up a reverse tunnel:

```sh
ssh -R 52698:localhost:52698 user@remote
```

Or add to `~/.ssh/config`:

```
Host myserver
  RemoteForward 52698 localhost:52698
```

## Usage

```
Usage: rmate [options] file [file ...]
       rmate [options] -      (read from stdin and write the edit to stdout)
       rmate [options] - >out (read from stdin and save the edit to file 'out')

Options:
  -H HOST  Connect to host. Use 'auto' to detect from SSH.
           Defaults to $RMATE_HOST or localhost.
  -p PORT  Port number to use for connection.
           Defaults to $RMATE_PORT or 52698.
  -w       Wait for file to be closed by TextMate.
  -l LINE[:COL]
           Place cursor on line number. Repeat for each file,
           e.g. -l 5 -l 10 file1 file2 (5 for file1, 10 for file2).
           A position in the file argument (file:LINE[:COL], or a
           preceding +LINE) wins over -l for that file.
  -c       Send column numbers (selection: LINE:COL). Only for editor
           servers that accept a column; also enabled by $RMATE_COLUMN=1.
  -m NAME  The display name shown in TextMate.
  -f       Open even if file is not writable.
  -v       Verbose logging messages.
  -V       Print version information.
  -h       Print this help.
```

## Environment Variables

| Variable     | Description                          | Default     |
|--------------|--------------------------------------|-------------|
| `RMATE_HOST` | Editor host to connect to            | `localhost` |
| `RMATE_PORT` | Editor port to connect to            | `52698`     |
| `RMATE_COLUMN` | `1` sends `selection: LINE:COL` (same as `-c`) | unset |

Setting `RMATE_HOST=auto` detects the client IP from `SSH_CONNECTION`.

## Cursor positions

Besides `-l`, a position can be given in the file argument, so rmate works as `$EDITOR` for tools that pass Sublime-style `file:LINE[:COL]` or vi-style `+LINE file`:

```sh
rmate notes.md:12        # line 12
rmate notes.md:12:4      # line 12, column 4 (column sent only with -c)
rmate +12 notes.md       # line 12
```

`file:LINE[:COL]` is only split when the full argument does not exist and the part before the position does, so a real file named `notes.md:12` still opens as itself. A column of `0` (`notes.md:12:0`, as `subl` writes it) means no column. Creating a new file whose name ends in `:N` prints a warning, since it usually means a line number was attached to a path that does not exist.

Columns are off by default: RemoteSubl opens files at `<path>:<selection>:0`, so `selection: 12:4` would make it open a new file named `<path>:12`. Use `-c` or `RMATE_COLUMN=1` only with a server that accepts `LINE:COL`.

## Enhancements over the Original C Version

This is a rewrite of the original
[rmate.c by Mael Clerambault](https://github.com/hanklords/rmate.c). The following
is a summary of changes compared to the upstream `origin/master`.

**NOTE**: The code changes are made with the help of [Cursor CLI](https://cursor.com/cli),
but I spent a lot of time reviewing and revising them very carefully. So don't
panic, it's not just trivial AI generated code :-).

### New Features

- **Stdin support (`-`)**: Read from stdin, send to the editor, and write the
  edited result back to stdout. The original only supported named files.
- **Multiple files**: Open several files in a single invocation
  (`rmate a.txt b.txt`). The original was limited to one file.
- **Per-file line selection (`-l`)**: Jump to a specific line number in each
  file. Supports multiple `-l` flags, one per file.
- **Per-file display names (`-m`)**: Override the tab name shown in the editor
  for each file.
- **Force open (`-f`)**: Open read-only files with a warning instead of
  refusing.
- **Verbose logging (`-v`)**: Detailed protocol-level logging to stderr for
  debugging connection issues.
- **Auto host detection (`-H auto`)**: Automatically extract the client IP
  from the `SSH_CONNECTION` environment variable.

### Robustness Improvements

- **Complete rewrite of I/O handling**: All `read()`/`write()` calls handle
  `EINTR`, partial reads, and partial writes correctly. The original used bare
  `read()`/`write()` without retry.
- **Streaming file send**: Files are streamed from the file descriptor directly
  to the socket in chunks (`stream_data`), instead of being `mmap()`'d into
  memory. This keeps RSS constant regardless of file size.
- **Proper protocol buffer management**: The receive loop uses `memmove()` to
  shift unconsumed bytes to the front of the buffer, correctly handling TCP
  fragmentation where a single `read()` may return partial lines. The original
  assumed each `read()` returned aligned protocol messages.
- **Safe saves**: A save is spooled to a temporary file and copied over the
  target only once the whole payload has arrived, so a dropped connection
  cannot leave a truncated file. The target keeps its inode and permissions,
  and symlinks are written through. If writing fails, `receive_save` drains
  the rest of the payload to keep the protocol stream in sync. Save failures
  are counted and reflected in the exit code without aborting saves for other
  files.
- **The editor is not trusted with paths**: only files named on the command
  line can be written back, and only by a `save` command with a well-formed
  length. Anything else is skipped and reported.
- **Resilient parsing**: a header line cut by a TCP segment or read boundary
  waits for the rest instead of being misread, so a save is never dropped.
- **Clean socket shutdown**: Uses `shutdown(SHUT_WR)` followed by a timed
  drain before `close()`, preventing RST packets on macOS/BSD.
- **Proper background detach**: Uses `fork()` + `setsid()` to fully detach
  from the controlling terminal, preventing `SIGHUP`/`SIGINT` from killing
  the background save handler.
- **SIGPIPE handling**: Ignores `SIGPIPE` so a broken editor connection
  produces a clean error message instead of a silent crash.
- **Header validation**: Rejects filenames, display names (`-m`) and resolved
  paths containing `\n` or `\r` to prevent protocol injection.
- **File descriptor hygiene**: All file descriptors are opened with
  `O_CLOEXEC` (or equivalent) to prevent leaks across `fork()`.
- **Memory safety**: All allocations are checked, all paths go through
  `goto cleanup` for deterministic resource release, and `size_t` overflow
  is detected in the stdin buffer growth.

## Performance

See [BENCHMARK.md](BENCHMARK.md) for detailed measurements. Summary:

| Metric | C | Bash | Python | Ruby |
|--------|---|------|--------|------|
| Startup | **0.3 ms** | 2.2 ms | 34 ms | 44 ms |
| Memory (file mode) | **1.7 MB constant** | 3.7 MB constant | Scales with file | Scales with file |
| Memory (100 MB stdin) | **104 MB** | 496 MB | 215 MB | 120 MB |

## Protocol

See [PROTOCOL.md](PROTOCOL.md) for a description of the rmate wire protocol.

## Implementation

See [IMPLEMENTATION.md](IMPLEMENTATION.md) for code walkthrough and design
details.

## License

Based on [rmate.c](https://github.com/hanklords/rmate.c) by Mael Clerambault.
