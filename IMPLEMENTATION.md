# Implementation Details

This document describes the internal structure of `rmate.c` to help new
contributors understand the code.

## File Layout

The source is a single `rmate.c` file with no external dependencies beyond
POSIX libc. Functions appear in bottom-up order: utilities first, then
protocol helpers, then option parsing and high-level orchestration, then
`main()`.

## Core Data Structures

### `struct cmd`

```c
struct cmd {
    enum CMD_STATE state;   /* CMD_CMD or CMD_VAR */
    char* filename;         /* token from the server (heap-allocated) */
    size_t file_len;        /* byte count from the "data:" field */
};
```

This is the state machine context for parsing server responses. A single
instance is reused across all commands on one connection.

### `enum CMD_STATE`

```
CMD_CMD --(command)--> CMD_VAR
   ^                      |
   |                      |
   +--(empty line)--------+
```

The server's greeting line is consumed by `read_greeting()` before any files
are sent, so the receive loop starts in CMD_CMD.

- **CMD_CMD**: Initial state. Waits for a command keyword (`save` or `close`).
- **CMD_VAR**: Parses `key: value` pairs. When `data:` is encountered, it
  triggers `receive_save()` to consume the file bytes. An empty line resets
  back to CMD_CMD.

## Protocol: Client → Server

### send\_open()

The central function for sending a file to the editor:

```c
int send_open(int sockfd,
              const char* display_name,
              const char* real_path,
              const char* token,
              int data_fd,          /* file descriptor, or -1 */
              const char* data_buf, /* in-memory buffer, or NULL */
              size_t data_len,
              int line,
              int col);             /* sent as LINE:COL when > 0 */
```

It builds the full protocol header in a stack buffer via `snprintf()`, sends
it with `write_all()`, then sends the file content using one of two paths:

1. **`data_buf != NULL`** (stdin mode): The data is already in memory; write
   it directly.
2. **`data_fd >= 0`** (file mode): Stream from file descriptor to socket via
   `stream_data()`, reading and writing in `BUFSIZ` chunks. The file is never
   loaded into memory.

Finally, `"\n.\n"` terminates the command.

Two convenience wrappers call `send_open()`:

- **`send_file()`**: Opens the file, `fstat()`s it for the size, resolves the
  real path, and calls `send_open()` with the file descriptor.
- **`send_stdin()`**: Calls `send_open()` with the pre-read stdin buffer,
  using `"-"` for both real-path and token.

## Protocol: Server → Client

### Buffer Management

TCP is a byte stream — a single `read()` may return a partial line, one
complete line, or multiple lines concatenated. The receive loop in
`receive_loop()` handles this:

```
+------------------------------------------+
|           buf[PROTO_BUF_SIZE]            |
|  [consumed data][unconsumed][  free  ]   |
|                  ^buf_used               |
+------------------------------------------+
```

1. `read()` appends new data after `buf_used`.
2. `handle_cmds()` processes as many complete `\n`-delimited lines as
   possible, returning the total bytes consumed.
3. If `consumed < buf_used`, `memmove()` shifts the leftover tail to the
   front. (`memmove` is required because source and destination overlap.)
4. The next `read()` appends after the leftover.

### read\_line()

Scans for `\n` in the buffer. If found, NUL-terminates the line (stripping
`\r\n` endings), and returns the number of bytes consumed (including the
newline). Returns 0 if no complete line is available yet.

### handle\_line()

A dispatch function driven by `cmd_state->state`:

- **CMD_CMD**: Read a command name. Reset state, then transition to CMD_VAR.
- **CMD_VAR**: Parse `key: value`. On `token:`, save the filename. On
  `data:`, call `receive_save()`. On empty line or parse error, transition
  back to CMD_CMD.

### receive\_save()

Writes incoming file data to disk:

```c
ssize_t receive_save(int sockfd,
                     char* buf_remaining,
                     size_t buf_remaining_len,
                     const char* filename,
                     size_t file_size,
                     int* save_errors);
```

When `handle_line()` encounters `data: N`, some of the N file bytes may
already be in the protocol buffer (read in the same TCP segment as the
`data:` header). `receive_save()` handles this in two steps:

1. Write `buf_remaining` (the bytes already in the buffer) to the file.
2. Call `stream_data()` to read the remaining bytes directly from the socket
   and write them to the file.

If the token is `"-"`, it writes to stdout instead of a file, enabling
`echo "text" | rmate -w - > edited.txt`.

**Drain mode**: If writing to disk fails (e.g., disk full), `stream_data()`
switches to drain-only mode — it continues reading from the socket but
discards the data. This keeps the protocol stream in sync so subsequent
commands on the same connection still parse correctly.

**Save error tracking**: A single rmate connection handles saves for all open
files. If `receive_save()` returned a fatal error on one file's save failure,
the receive loop would exit and subsequent saves for other files would never
be processed. To avoid this, save failures (open error, write error, or
`stream_data()` write failure) increment the caller's `*save_errors` counter
instead of returning -1. The data is drained to keep the protocol in sync,
and the loop continues. After the loop exits, the counter is checked and
the process exit code is set to 1 if any saves failed.

## I/O Helpers

### write\_all()

Retries `write()` in a loop until all bytes are sent, handling `EINTR` and
short writes. Returns 0 on success, -1 on error.

### stream\_data()

Transfers `len` bytes from `from_fd` to `to_fd` in `BUFSIZ` chunks:

```c
int stream_data(int from_fd, int to_fd, size_t len, int drain_on_write_fail);
```

- Handles `EINTR` on reads.
- Detects unexpected EOF.
- Returns -1 on read failure, 0 on success, or 1 if `drain_on_write_fail`
  was set and a write failed (data was fully drained but not written).
- If `drain_on_write_fail` is set and a write fails, sets `to_fd = -1` and
  continues reading until all `len` bytes are consumed.

### read\_stdin()

Reads all of stdin into a `malloc()`'d buffer using exponential growth
(starting at 4 KB, doubling each time). Detects `size_t` overflow from
doubling. Returns `NULL` on error with `errno` set.

## Option Parsing and Main Structure

### `struct options`

```c
struct options {
    const char* host;
    const char* port;
    char* auto_host;    /* owned, freed by free_options() */
    int need_wait;
    int force;
    int* lines;         /* owned, freed by free_options() */
    int n_lines;
    const char** names; /* owned, elements point into argv */
    int n_names;
};
```

Groups all command-line configuration. Passed by pointer to functions that
need it.

### parse\_options()

Handles `getopt()`, validates `-l` line numbers with `strtol()`, resolves
`-H auto` via `parse_ssh_host()`, and adjusts `argc`/`argv` past the options.
Returns 0 on success, 1 on error, or -1 for early exit (`-V`/`-h`).

### send\_files()

Iterates the positional arguments, builds display names, and calls
`send_file()` or `send_stdin()` for each. Extracted from `main()` to keep it
focused on orchestration.

### receive\_loop()

Owns the protocol buffer and `struct cmd` state machine. Reads from the
server socket, dispatches via `handle_cmds()`, and tracks `save_errors`.
Returns 0 on clean shutdown or 1 on error.

### close\_connection()

Performs graceful socket shutdown: sends FIN via `shutdown(SHUT_WR)`, drains
unread data with a short timeout, then closes the socket.

### check\_file()

Validates a filename before opening: rejects names containing `\n` or `\r`
(to prevent protocol injection), checks that the path is a regular file,
and verifies write permission (unless `-f` is set). A missing file passes
the check but is not created yet.

### create\_missing\_file()

Creates a file that `check_file()` found missing. `main()` calls it only after
the editor's greeting, so a missing editor or a typo'd name doesn't leave an
empty file behind.

## Connection Lifecycle

### connect\_mate()

Uses `getaddrinfo()` to resolve the host/port (supporting both IPv4 and
IPv6), then tries each address in order. Each attempt goes through
`connect_timeout()`, a non-blocking `connect()` with a 5 second `poll()`, so a
host that drops packets fails instead of blocking for the kernel's SYN retry
period. On success, enables `SO_KEEPALIVE` to detect dead connections. On
failure it reports the last address's error, plus a `RemoteForward` hint for
`ECONNREFUSED` when `SSH_CONNECTION` is set.

### read\_greeting()

Waits up to 10 seconds (`poll()`) for the server's greeting line and reads it
one byte at a time, so nothing after it is consumed. EOF, `ECONNRESET` or a
timeout means no editor answered: over ssh, `connect()` to a `RemoteForward`
port succeeds even when nothing listens on the client side. Doing this before
sending files and forking makes that a foreground error with exit status 1,
plus a hint when `SSH_CONNECTION` is set.

### Background Mode (default)

After sending all files, `main()` forks. The parent exits immediately
(returning the shell prompt), and the child calls `setsid()` to detach from
the controlling terminal before entering the receive loop. This prevents
`SIGHUP` or `SIGINT` from killing the background process. The child also
points stdin, and stdout unless a `-` argument writes there, at `/dev/null`,
so `out=$(rmate file)` or a pipe does not wait for the file to be closed. It
keeps stderr for save errors and prefixes its messages with `rmate: `, since
they appear after the shell prompt.

### Wait Mode (`-w`)

The process stays in the foreground and blocks in the receive loop until the
server closes the connection (all editor tabs closed).

### Clean Shutdown

When the receive loop ends:

1. `shutdown(sockfd, SHUT_WR)` sends a TCP FIN.
2. A short timed drain loop reads any remaining data from the socket.
3. `close(sockfd)` releases the socket.

Step 2 prevents a TCP RST on macOS/BSD, where closing a socket with unread
data in the receive buffer causes the kernel to send RST instead of FIN.

## Signal Handling

- **`SIGCHLD`**: Set to `SIG_IGN` so forked children are automatically
  reaped (no zombies).
- **`SIGPIPE`**: Set to `SIG_IGN` so writing to a broken socket returns
  `EPIPE` instead of killing the process with a signal. This lets the error
  handling code produce a meaningful message.

## File Descriptor Hygiene

All `open()` calls use `O_CLOEXEC`, `dup()` uses `F_DUPFD_CLOEXEC`, and
`socket()` is followed by `fcntl(fd, F_SETFD, FD_CLOEXEC)`. This prevents
file descriptor leaks across `fork()`/`exec()` boundaries.

## Error Handling Strategy

All functions return -1 on failure. `main()` uses a single `goto cleanup`
pattern:

```c
int main(...) {
    int ret = 0;
    ...
    if (something_failed) {
        ret = 1;
        goto cleanup;
    }
    ...
cleanup:
    close_connection(...);
    free_options(...);
    return ret;
}
```

This ensures deterministic cleanup of all resources (file descriptors, heap
allocations, socket) on every exit path.

`parse_options()` uses a three-way return (0 / 1 / -1) so `main()` can
distinguish success, error, and early exit (help/version).

Save failures during the receive loop are non-fatal: `receive_save()`
increments a `save_errors` counter and drains the data to keep the protocol
in sync. The exit code is set to 1 after the loop if any saves failed.
