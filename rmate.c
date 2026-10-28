#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#define HOST_ENV "RMATE_HOST"
#define PORT_ENV "RMATE_PORT"
#define COLUMN_ENV "RMATE_COLUMN"
#define DEFAULT_HOST "localhost"
#define DEFAULT_PORT "52698"

#ifndef HOST_NAME_MAX
#define HOST_NAME_MAX 255
#endif

#define PROTO_BUF_SIZE (PATH_MAX + 256)

int verbose = 0;

enum CMD_STATE
{
    CMD_HEADER,
    CMD_CMD,
    CMD_VAR
};

struct cmd
{
    enum CMD_STATE state;
    char* filename;
    size_t file_len;
};

enum log_type
{
    INFO,
    WARNING,
    ERROR
};

static const struct
{
    const char* label;
    const char* color;
} log_styles[] = {
    [INFO] = { "Info:", "\033[32m" },
    [WARNING] = { "Warning:", "\033[33m" },
    [ERROR] = { "Error:", "\033[31m" },
};

void
log_msg(enum log_type type, const char* fmt, ...)
{
    if (type == INFO && !verbose) {
        return;
    }

    if (isatty(STDERR_FILENO)) {
        fprintf(stderr, "%s%s\033[0m ", log_styles[type].color, log_styles[type].label);
    } else {
        fprintf(stderr, "%s ", log_styles[type].label);
    }

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

char*
read_stdin(size_t* out_len)
{
    size_t cap = 4096, len = 0;
    char* buf = malloc(cap);

    if (!buf) {
        return NULL;
    }

    while (1) {
        ssize_t n = read(STDIN_FILENO, buf + len, cap - len);

        if (n == -1) {
            if (errno == EINTR) {
                continue;
            }
            free(buf);
            return NULL;
        }
        if (n == 0) {
            break;
        }
        len += n;
        if (len == cap) {
            char* tmp;
            size_t new_cap = cap * 2;

            /* Detect size_t overflow from doubling. */
            if (new_cap <= cap) {
                free(buf);
                errno = ENOMEM;
                return NULL;
            }
            cap = new_cap;
            tmp = realloc(buf, cap);
            if (!tmp) {
                free(buf);
                return NULL;
            }
            buf = tmp;
        }
    }

    *out_len = len;
    return buf;
}

int
write_all(int fd, const char* buf, size_t len)
{
    while (len > 0) {
        ssize_t n = write(fd, buf, len);

        if (n == -1) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        buf += n;
        len -= n;
    }
    return 0;
}

/*
 * If drain_on_write_fail is set, a write failure stops writing but keeps
 * reading until all len bytes are consumed, so the source stream stays in sync.
 */
int
stream_data(int from_fd, int to_fd, size_t len, int drain_on_write_fail)
{
    char tmp[BUFSIZ];
    int write_failed = 0;

    while (len > 0) {
        size_t to_read = len < sizeof(tmp) ? len : sizeof(tmp);
        ssize_t n = read(from_fd, tmp, to_read);

        if (n == -1) {
            if (errno == EINTR) {
                continue;
            }
            log_msg(ERROR, "Failed to read data: %s", strerror(errno));
            return -1;
        }
        if (n == 0) {
            log_msg(ERROR, "Unexpected EOF (%zu bytes remaining)", len);
            return -1;
        }
        if (to_fd >= 0 && write_all(to_fd, tmp, n) == -1) {
            log_msg(ERROR, "Failed to write data: %s", strerror(errno));
            if (!drain_on_write_fail) {
                return -1;
            }
            log_msg(WARNING, "Write failed, draining remaining data");
            to_fd = -1;
            write_failed = 1;
        }
        len -= n;
    }
    return write_failed;
}

int
send_open(int sockfd,
          const char* display_name,
          const char* real_path,
          const char* token,
          int data_fd,
          const char* data_buf,
          size_t data_len,
          int line,
          int col)
{
    char hdr[HOST_NAME_MAX + PATH_MAX * 3 + 256];

    log_msg(INFO, "Opening %s (%zu bytes)", display_name, data_len);

    int off = snprintf(hdr,
                       sizeof(hdr),
                       "open\n"
                       "display-name: %s\n"
                       "real-path: %s\n"
                       "data-on-save: yes\n"
                       "re-activate: yes\n"
                       "token: %s\n",
                       display_name,
                       real_path,
                       token);

    if (line > 0 && off >= 0 && (size_t)off < sizeof(hdr)) {
        if (col > 0) {
            off += snprintf(hdr + off, sizeof(hdr) - off, "selection: %d:%d\n", line, col);
        } else {
            off += snprintf(hdr + off, sizeof(hdr) - off, "selection: %d\n", line);
        }
    }

    if (off >= 0 && (size_t)off < sizeof(hdr)) {
        off += snprintf(hdr + off, sizeof(hdr) - off, "data: %zu\n", data_len);
    }

    if (off < 0 || (size_t)off >= sizeof(hdr)) {
        log_msg(ERROR, "Protocol header too long");
        return -1;
    }

    if (write_all(sockfd, hdr, off) == -1) {
        log_msg(ERROR, "Failed to send header to editor: %s", strerror(errno));
        return -1;
    }

    if (data_buf) {
        if (data_len > 0 && write_all(sockfd, data_buf, data_len) == -1) {
            log_msg(ERROR, "Failed to send file data to editor: %s", strerror(errno));
            return -1;
        }
    } else if (data_fd >= 0) {
        if (stream_data(data_fd, sockfd, data_len, 0) == -1) {
            log_msg(ERROR, "Failed to stream file data to editor");
            return -1;
        }
    }

    if (write_all(sockfd, "\n.\n", 3) == -1) {
        log_msg(ERROR, "Failed to send end-of-data to editor: %s", strerror(errno));
        return -1;
    }

    return 0;
}

int
send_stdin(int sockfd, const char* display_name, const char* data, size_t data_len, int line, int col)
{
    return send_open(sockfd, display_name, "-", "-", -1, data, data_len, line, col);
}

int
check_file(const char* filename, int force)
{
    struct stat st;

    if (strchr(filename, '\n') || strchr(filename, '\r')) {
        log_msg(ERROR, "Filename contains newline or carriage return, refusing to open");
        return -1;
    }

    if (stat(filename, &st) == -1) {
        if (errno != ENOENT) {
            log_msg(ERROR, "Failed to stat file %s: %s", filename, strerror(errno));
            return -1;
        }
        const char* colon = strrchr(filename, ':');
        if (colon && colon[1] != '\0' && strspn(colon + 1, "0123456789") == strlen(colon + 1)) {
            log_msg(WARNING,
                    "Creating new file %s; if :%s was meant as a line number, the file before it does not exist",
                    filename,
                    colon + 1);
        }
        log_msg(INFO, "Creating new file %s", filename);
        int fd = open(filename, O_CREAT | O_WRONLY | O_CLOEXEC, 0644);
        if (fd == -1) {
            log_msg(ERROR, "Failed to create file %s: %s", filename, strerror(errno));
            return -1;
        }
        close(fd);
        return 0;
    }

    if (S_ISDIR(st.st_mode)) {
        log_msg(ERROR, "%s is a directory, aborting", filename);
        return -1;
    }
    if (!S_ISREG(st.st_mode)) {
        log_msg(ERROR, "%s is not a regular file", filename);
        return -1;
    }
    if (access(filename, W_OK) == -1) {
        if (!force) {
            log_msg(ERROR, "File %s is not writable! Use -f to open anyway", filename);
            return -1;
        }
        log_msg(WARNING, "File %s is not writable, opening anyway", filename);
    }

    return 0;
}

int
send_file(int sockfd, const char* filename, const char* display_name, int line, int col)
{
    int fd = open(filename, O_RDONLY | O_CLOEXEC);
    if (fd == -1) {
        log_msg(ERROR, "Failed to open file %s: %s", filename, strerror(errno));
        return -1;
    }

    struct stat st;
    if (fstat(fd, &st) == -1) {
        log_msg(ERROR, "Failed to stat file %s: %s", filename, strerror(errno));
        close(fd);
        return -1;
    }

    char resolved[PATH_MAX];
    const char* rpath = realpath(filename, resolved);
    if (!rpath) {
        rpath = filename;
    }

    int ret = send_open(sockfd, display_name, rpath, filename, fd, NULL, (size_t)st.st_size, line, col);
    close(fd);
    return ret;
}

ssize_t
receive_save(int sockfd,
             char* buf_remaining,
             size_t buf_remaining_len,
             const char* filename,
             size_t file_size,
             int* save_errors)
{
    int fd;
    int write_failed = 0;

    if (!strcmp(filename, "-")) {
        log_msg(INFO, "Writing %zu bytes to stdout", file_size);
        fd = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 0);
    } else {
        log_msg(INFO, "Saving %s (%zu bytes)", filename, file_size);
        fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    }

    if (fd == -1) {
        log_msg(ERROR, "Failed to open %s for writing: %s", filename, strerror(errno));
        write_failed = 1;
    }

    /* buf_remaining holds file data already in the protocol read buffer. */
    size_t to_read = (buf_remaining_len < file_size) ? buf_remaining_len : file_size;

    if (fd >= 0 && to_read > 0 && write_all(fd, buf_remaining, to_read) == -1) {
        log_msg(ERROR, "Failed to write to %s: %s", filename, strerror(errno));
        close(fd);
        fd = -1;
        write_failed = 1;
    }

    ssize_t ret = (ssize_t)to_read;
    int drain_ret = stream_data(sockfd, fd, file_size - to_read, 1);
    if (drain_ret < 0) {
        ret = -1;
    } else if (drain_ret > 0) {
        write_failed = 1;
    }

    if (fd >= 0) {
        close(fd);
    }
    if (write_failed && ret >= 0) {
        (*save_errors)++;
    }
    return ret;
}

ssize_t
read_line(char* buf, size_t len)
{
    char* cmd_str = memchr(buf, '\n', len);
    if (!cmd_str) {
        /* No complete line yet; caller should read more data. */
        return 0;
    }

    ssize_t line_len = cmd_str - buf;
    /* Strip \r from \r\n endings. */
    if (line_len > 0 && cmd_str[-1] == '\r') {
        cmd_str[-1] = '\0';
    }
    cmd_str[0] = '\0';

    return line_len + 1;
}

ssize_t
handle_line(int sockfd, char* buf, size_t len, struct cmd* cmd_state, int* save_errors)
{
    ssize_t read_len = 0;
    char *name, *value, *sep;

    switch (cmd_state->state) {
        case CMD_HEADER:
            if ((read_len = read_line(buf, len)) > 0) {
                log_msg(INFO, "Server header: %s", buf);
                cmd_state->state = CMD_CMD;
            }
            break;

        case CMD_CMD:
            if ((read_len = read_line(buf, len)) > 0 && *buf != '\0') {
                free(cmd_state->filename);
                memset(cmd_state, 0, sizeof(*cmd_state));

                if (!strcmp(buf, "close")) {
                    log_msg(INFO, "Received close command");
                } else if (!strcmp(buf, "save")) {
                    log_msg(INFO, "Received save command");
                }

                cmd_state->state = CMD_VAR;
            }
            break;

        case CMD_VAR:
            if ((read_len = read_line(buf, len)) == 0) {
                goto reset_cmd;
            }

            if (*buf == '\0') {
                goto reset_cmd;
            }

            sep = strchr(buf, ':');
            if (!sep) {
                goto reset_cmd;
            }

            name = buf;
            *sep = '\0';
            value = sep + 1;
            while (*value == ' ') {
                value++;
            }

            if (!strcmp(name, "token")) {
                free(cmd_state->filename);
                cmd_state->filename = strdup(value);
                if (!cmd_state->filename) {
                    log_msg(ERROR, "Failed to allocate memory for token: %s", strerror(errno));
                    return -1;
                }
            } else if (!strcmp(name, "data")) {
                char* endp;
                errno = 0;
                cmd_state->file_len = strtoul(value, &endp, 10);
                if (errno || endp == value) {
                    log_msg(WARNING, "Invalid data length: %s, ignoring", value);
                    goto reset_cmd;
                }
                if (!cmd_state->filename) {
                    log_msg(WARNING, "Received data without token, ignoring");
                } else {
                    ssize_t data_consumed = receive_save(
                      sockfd, buf + read_len, len - read_len, cmd_state->filename, cmd_state->file_len, save_errors);
                    if (data_consumed < 0) {
                        return -1;
                    }
                    read_len += data_consumed;
                }
            }
            break;

        reset_cmd:
            cmd_state->state = CMD_CMD;
            break;

        default:
            break;
    }

    return read_len;
}

ssize_t
handle_cmds(int sockfd, char* buf, size_t len, struct cmd* cmd_state, int* save_errors)
{
    size_t total_read_len = 0;

    while (total_read_len < len) {
        ssize_t read_len = handle_line(sockfd, buf, len - total_read_len, cmd_state, save_errors);

        if (read_len < 0) {
            return -1;
        }
        if (read_len == 0) {
            break;
        }

        buf += read_len;
        total_read_len += read_len;
    }

    return total_read_len;
}

char*
parse_ssh_host(void)
{
    const char* ssh = getenv("SSH_CONNECTION");
    if (!ssh || !*ssh) {
        return NULL;
    }

    const char* end = strchr(ssh, ' ');
    /* No space means SSH_CONNECTION has only the client IP (unusual). */
    size_t len = end ? (size_t)(end - ssh) : strlen(ssh);
    char* result = malloc(len + 1);
    if (!result) {
        return NULL;
    }
    memcpy(result, ssh, len);
    result[len] = '\0';
    return result;
}

int
connect_mate(const char* host, const char* port)
{
    int sockfd = -1;
    struct addrinfo hints, *servinfo;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int rv = getaddrinfo(host, port, &hints, &servinfo);
    if (rv != 0) {
        log_msg(ERROR, "Failed to resolve %s:%s: %s", host, port, gai_strerror(rv));
        return -1;
    }

    for (struct addrinfo* p = servinfo; p != NULL; p = p->ai_next) {
        if ((sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1) {
            continue;
        }
        fcntl(sockfd, F_SETFD, FD_CLOEXEC);

        if (connect(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
            close(sockfd);
            sockfd = -1;
            continue;
        }

        break;
    }

    freeaddrinfo(servinfo);

    if (sockfd != -1) {
        int keepalive = 1;
        if (setsockopt(sockfd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive)) == -1) {
            log_msg(WARNING, "Failed to set SO_KEEPALIVE: %s", strerror(errno));
        }
        log_msg(INFO, "Connected to %s:%s", host, port);
    }

    return sockfd;
}

const char*
make_display_name(const char* display_name, const char* hostname, const char* filename, char* buf, size_t buf_len)
{
    if (display_name) {
        return display_name;
    }
    snprintf(buf, buf_len, "%s:%s", hostname, filename);
    return buf;
}

void
version(void)
{
#ifdef BUILD_ID
    const char* build_id = BUILD_ID;
#else
    const char* build_id = "";
#endif
    fprintf(stderr,
            "rmate 2.0.0%s (%s)\n"
            "Copyright (c) 2014 Mael Clerambault\n"
            "Copyright (c) 2026 Jianrong Zhao\n",
            build_id,
            __DATE__);
}

void
usage(void)
{
    fprintf(stderr,
            "Usage: rmate [options] file [file ...]\n"
            "       rmate [options] -      (read from stdin and write the edit to stdout)\n"
            "       rmate [options] - >out (read from stdin and save the edit to file 'out')\n"
            "\n"
            "Options:\n"
            "  -H HOST  Connect to host. Use 'auto' to detect from SSH.\n"
            "           Defaults to $%s or %s.\n"
            "  -p PORT  Port number to use for connection.\n"
            "           Defaults to $%s or %s.\n"
            "  -w       Wait for file to be closed by TextMate.\n"
            "  -l LINE[:COL]\n"
            "           Place cursor on line number. Repeat for each file,\n"
            "           e.g. -l 5 -l 10 file1 file2 (5 for file1, 10 for file2).\n"
            "           A position in the file argument (file:LINE[:COL], or a\n"
            "           preceding +LINE) wins over -l for that file.\n"
            "  -c       Send column numbers (selection: LINE:COL). Only for editor\n"
            "           servers that accept a column; also enabled by $%s=1.\n"
            "  -m NAME  The display name shown in TextMate.\n"
            "  -f       Open even if file is not writable.\n"
            "  -v       Verbose logging messages.\n"
            "  -V       Print version information.\n"
            "  -h       Print this help.\n",
            HOST_ENV,
            DEFAULT_HOST,
            PORT_ENV,
            DEFAULT_PORT,
            COLUMN_ENV);
}

struct options
{
    const char* host;
    const char* port;
    char* auto_host;
    int need_wait;
    int force;
    int send_column;
    int* lines;
    int* cols;
    int n_lines;
    const char** names;
    int n_names;
    /* Files after position parsing, with any position taken from the argument. */
    char** files;
    char** owned_files;
    int* file_lines;
    int* file_cols;
};

/* Parse "LINE" or "LINE:COL", both positive. Returns 0 on success, -1 otherwise. */
int
parse_position(const char* s, int* line, int* col)
{
    char* endp;
    long l, c = 0;

    if (*s < '0' || *s > '9') {
        return -1;
    }
    errno = 0;
    l = strtol(s, &endp, 10);
    if (errno || l <= 0 || l > INT_MAX) {
        return -1;
    }
    if (*endp == ':') {
        const char* cs = endp + 1;
        if (*cs < '0' || *cs > '9') {
            return -1;
        }
        errno = 0;
        c = strtol(cs, &endp, 10);
        if (errno || c <= 0 || c > INT_MAX) {
            return -1;
        }
    }
    if (*endp != '\0') {
        return -1;
    }
    *line = (int)l;
    *col = (int)c;
    return 0;
}

/*
 * Editors such as subl take "file:LINE[:COL]", and callers that think rmate is
 * one (Claude Code picks the argument style from $EDITOR's name) pass it on.
 * When arg itself does not exist but the part before a trailing ":LINE:COL" or
 * ":LINE" does, split the position off. Returns 1 with a malloc'd *path on a
 * split, 0 to use arg as is, -1 on allocation failure.
 */
int
split_file_position(const char* arg, char** path, int* line, int* col)
{
    struct stat st;

    if (stat(arg, &st) == 0) {
        return 0;
    }

    /* "path:LINE:COL" first, then "path:LINE". */
    for (int parts = 2; parts >= 1; parts--) {
        const char* sep = arg + strlen(arg);
        int ok = 1;

        for (int k = 0; k < parts && ok; k++) {
            while (sep > arg && sep[-1] != ':') {
                sep--;
            }
            if (sep == arg) {
                ok = 0;
            } else {
                sep--;
            }
        }
        if (!ok || sep == arg || parse_position(sep + 1, line, col) != 0) {
            continue;
        }

        char* candidate = strndup(arg, sep - arg);
        if (!candidate) {
            return -1;
        }
        if (stat(candidate, &st) == 0) {
            *path = candidate;
            return 1;
        }
        free(candidate);
    }

    *line = 0;
    *col = 0;
    return 0;
}

/*
 * Returns:
 *   0  success, opts populated, *out_argc / *out_argv set to remaining args
 *   1  error (message already logged)
 *  -1  early exit (version/help already printed, caller should exit 0)
 */
int
parse_options(int orig_argc, char* orig_argv[], int* out_argc, char*** out_argv, struct options* opts)
{
    opts->host = getenv(HOST_ENV);
    opts->port = getenv(PORT_ENV);
    if (!opts->host) {
        opts->host = DEFAULT_HOST;
    }
    if (!opts->port) {
        opts->port = DEFAULT_PORT;
    }

    const char* column_env = getenv(COLUMN_ENV);
    opts->send_column = column_env && *column_env && strcmp(column_env, "0") != 0;

    opts->lines = calloc(orig_argc, sizeof(*opts->lines));
    opts->cols = calloc(orig_argc, sizeof(*opts->cols));
    opts->names = calloc(orig_argc, sizeof(*opts->names));
    opts->files = calloc(orig_argc + 1, sizeof(*opts->files));
    opts->owned_files = calloc(orig_argc + 1, sizeof(*opts->owned_files));
    opts->file_lines = calloc(orig_argc + 1, sizeof(*opts->file_lines));
    opts->file_cols = calloc(orig_argc + 1, sizeof(*opts->file_cols));
    if (!opts->lines || !opts->cols || !opts->names || !opts->files || !opts->owned_files || !opts->file_lines ||
        !opts->file_cols) {
        log_msg(ERROR, "Failed to allocate memory: %s", strerror(errno));
        return 1;
    }

    int ch;
    while ((ch = getopt(orig_argc, orig_argv, "whvfcVH:p:l:m:")) != -1) {
        switch (ch) {
            case 'w':
                opts->need_wait = 1;
                break;
            case 'H':
                opts->host = optarg;
                break;
            case 'p':
                opts->port = optarg;
                break;
            case 'l': {
                int line, col;
                if (parse_position(optarg, &line, &col) != 0) {
                    log_msg(ERROR, "Invalid line number: %s", optarg);
                    return 1;
                }
                opts->lines[opts->n_lines] = line;
                opts->cols[opts->n_lines] = col;
                opts->n_lines++;
                break;
            }
            case 'c':
                opts->send_column = 1;
                break;
            case 'm':
                opts->names[opts->n_names++] = optarg;
                break;
            case 'f':
                opts->force = 1;
                break;
            case 'v':
                verbose = 1;
                break;
            case 'V':
                version();
                return -1;
            case 'h':
            default:
                usage();
                return -1;
        }
    }

    /*
     * Resolve per-file positions: a vi-style "+LINE" applies to the next file,
     * and "file:LINE[:COL]" is split when only the stripped path exists.
     */
    int n_files = 0, pending_line = 0, pending_col = 0;
    for (int i = optind; i < orig_argc; i++) {
        const char* arg = orig_argv[i];
        struct stat st;
        int line, col;

        if (arg[0] == '+' && i + 1 < orig_argc && stat(arg, &st) == -1 && parse_position(arg + 1, &line, &col) == 0) {
            pending_line = line;
            pending_col = col;
            continue;
        }

        opts->files[n_files] = orig_argv[i];
        opts->file_lines[n_files] = pending_line;
        opts->file_cols[n_files] = pending_col;
        pending_line = pending_col = 0;

        if (strcmp(arg, "-") != 0) {
            char* path;
            int r = split_file_position(arg, &path, &line, &col);
            if (r == -1) {
                log_msg(ERROR, "Failed to allocate memory: %s", strerror(errno));
                return 1;
            }
            if (r == 1) {
                log_msg(INFO, "Treating %s as %s at line %d", arg, path, line);
                opts->files[n_files] = opts->owned_files[n_files] = path;
                opts->file_lines[n_files] = line;
                opts->file_cols[n_files] = col;
            }
        }
        n_files++;
    }

    *out_argc = n_files;
    *out_argv = opts->files;

    if (*out_argc < 1) {
        usage();
        return -1;
    }

    if (!strcmp(opts->host, "auto")) {
        opts->auto_host = parse_ssh_host();
        if (opts->auto_host) {
            opts->host = opts->auto_host;
            log_msg(INFO, "Detected host from SSH_CONNECTION: %s", opts->host);
        } else {
            opts->host = DEFAULT_HOST;
            log_msg(INFO, "SSH_CONNECTION not set, falling back to %s", opts->host);
        }
    }

    return 0;
}

void
free_options(struct options* opts)
{
    if (opts->owned_files) {
        for (int i = 0; opts->files && opts->files[i]; i++) {
            free(opts->owned_files[i]);
        }
    }
    free(opts->lines);
    free(opts->cols);
    free(opts->names);
    free(opts->files);
    free(opts->owned_files);
    free(opts->file_lines);
    free(opts->file_cols);
    free(opts->auto_host);
}

int
send_files(int sockfd,
           int argc,
           char** argv,
           const char* hostname,
           const char* stdin_data,
           size_t stdin_len,
           const struct options* opts)
{
    char dname_buf[HOST_NAME_MAX + 1 + PATH_MAX + 1];

    for (int i = 0; i < argc; i++) {
        const char* per_name = (i < opts->n_names) ? opts->names[i] : NULL;
        int per_line = (i < opts->n_lines) ? opts->lines[i] : 0;
        int per_col = (i < opts->n_lines) ? opts->cols[i] : 0;
        const char* dname;

        if (opts->file_lines[i] > 0) {
            per_line = opts->file_lines[i];
            per_col = opts->file_cols[i];
        }
        /* Stock servers (e.g. RemoteSubl) misread "selection: LINE:COL", so columns are opt-in. */
        if (!opts->send_column) {
            per_col = 0;
        }

        if (!strcmp(argv[i], "-")) {
            dname = make_display_name(per_name, hostname, "untitled (stdin)", dname_buf, sizeof(dname_buf));
            if (send_stdin(sockfd, dname, stdin_data, stdin_len, per_line, per_col) == -1) {
                return -1;
            }
        } else {
            dname = make_display_name(per_name, hostname, argv[i], dname_buf, sizeof(dname_buf));
            if (send_file(sockfd, argv[i], dname, per_line, per_col) == -1) {
                return -1;
            }
        }
    }
    return 0;
}

int
receive_loop(int sockfd, int* save_errors)
{
    char buf[PROTO_BUF_SIZE];
    size_t buf_used = 0;
    struct cmd cmd_state = { 0 };
    int ret = 0;

    while (1) {
        if (buf_used >= sizeof(buf) - 1) {
            log_msg(ERROR, "Protocol line too long");
            ret = 1;
            break;
        }

        ssize_t num_bytes = read(sockfd, buf + buf_used, sizeof(buf) - 1 - buf_used);
        if (num_bytes == -1) {
            if (errno == EINTR) {
                continue;
            }
            log_msg(ERROR, "Failed to read from server: %s", strerror(errno));
            ret = 1;
            break;
        }
        if (num_bytes == 0) {
            break;
        }

        buf_used += num_bytes;
        buf[buf_used] = '\0';

        ssize_t consumed = handle_cmds(sockfd, buf, buf_used, &cmd_state, save_errors);
        if (consumed < 0) {
            ret = 1;
            break;
        }
        if (consumed > 0) {
            if ((size_t)consumed < buf_used) {
                memmove(buf, buf + consumed, buf_used - consumed);
                buf_used -= consumed;
            } else {
                buf_used = 0;
            }
        }
    }

    if (*save_errors > 0) {
        log_msg(WARNING, "%d save(s) failed", *save_errors);
        ret = 1;
    }

    free(cmd_state.filename);
    return ret;
}

void
close_connection(int sockfd)
{
    /* Send FIN so the server gets a clean EOF immediately. */
    shutdown(sockfd, SHUT_WR);
    /*
     * Drain unread data (e.g. server's protocol header) before close.
     * On macOS/BSD, close() on a socket with unread data sends RST
     * instead of FIN, causing "Connection reset by peer" on the server.
     */
    struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    char drain[BUFSIZ];
    while (read(sockfd, drain, sizeof(drain)) > 0)
        ;
    close(sockfd);
}

int
main(int argc, char* argv[])
{
    int sockfd = -1;
    int ret = 0;
    struct options opts = { 0 };
    char hostname[HOST_NAME_MAX + 1];
    char* stdin_data = NULL;
    size_t stdin_len = 0;
    int save_errors = 0;

    signal(SIGCHLD, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);

    int pr = parse_options(argc, argv, &argc, &argv, &opts);
    if (pr != 0) {
        ret = (pr > 0) ? 1 : 0;
        goto cleanup;
    }

    if (gethostname(hostname, sizeof(hostname)) == -1) {
        snprintf(hostname, sizeof(hostname), "unknown");
    }
    hostname[sizeof(hostname) - 1] = '\0';

    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "-")) {
            /* Only read stdin once; multiple "-" args reuse the same data. */
            if (!stdin_data) {
                if (isatty(STDIN_FILENO)) {
                    fprintf(stderr, "Reading from stdin, press ^D to stop\n");
                } else {
                    log_msg(INFO, "Reading from stdin");
                }
                stdin_data = read_stdin(&stdin_len);
                if (!stdin_data) {
                    log_msg(ERROR, "Failed to read from stdin: %s", strerror(errno));
                    ret = 1;
                    goto cleanup;
                }
            }
        } else if (check_file(argv[i], opts.force) == -1) {
            ret = 1;
            goto cleanup;
        }
    }

    if ((sockfd = connect_mate(opts.host, opts.port)) == -1) {
        log_msg(ERROR, "Could not connect to %s:%s", opts.host, opts.port);
        ret = 1;
        goto cleanup;
    }

    if (send_files(sockfd, argc, argv, hostname, stdin_data, stdin_len, &opts) == -1) {
        ret = 1;
        goto cleanup;
    }

    free(stdin_data);
    stdin_data = NULL;

    if (!opts.need_wait) {
        pid_t pid = fork();
        if (pid == -1) {
            log_msg(ERROR, "Failed to fork background process: %s", strerror(errno));
            ret = 1;
            goto cleanup;
        }
        if (pid > 0) {
            _exit(0);
        }
        /* Detach from the controlling terminal so SIGHUP/SIGINT won't reach us. */
        setsid();
    }

    ret = receive_loop(sockfd, &save_errors);

cleanup:
    if (sockfd != -1) {
        close_connection(sockfd);
    }
    free(stdin_data);
    free_options(&opts);
    log_msg(INFO, "Done");

    return ret;
}
