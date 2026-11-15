/*
 * main.c - the socket side of jd-tiny-httpd.
 *
 * Accepts one connection at a time, reads a single request, sends a single
 * response and closes the connection. All the parsing and path checks live
 * in http.c.
 */
#define _POSIX_C_SOURCE 200809L

#include "http.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define READ_TIMEOUT_SECONDS 5

static char root_real[HTTP_MAX_PATH];
static size_t root_len;

static int write_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

/* Read until the end of the request head, a full buffer, or EOF. */
static size_t read_request(int fd, char *buf, size_t cap)
{
    size_t used = 0;

    while (used < cap) {
        ssize_t n = read(fd, buf + used, cap - used);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        used += (size_t)n;
        buf[used] = '\0';
        if (strstr(buf, "\r\n\r\n") || strstr(buf, "\n\n"))
            break;
    }
    return used;
}

static void log_request(const struct sockaddr_in *peer,
                        const struct http_request *req, int code,
                        unsigned long long bytes)
{
    char addr[INET_ADDRSTRLEN] = "-";
    char when[32] = "-";
    time_t now = time(NULL);
    struct tm tm;

    inet_ntop(AF_INET, &peer->sin_addr, addr, sizeof addr);
    if (gmtime_r(&now, &tm))
        strftime(when, sizeof when, "%Y-%m-%dT%H:%M:%SZ", &tm);

    printf("%s [%s] \"%s %s %s\" %d %llu\n", addr, when,
           req->method[0] ? req->method : "-",
           req->target[0] ? req->target : "-",
           req->version[0] ? req->version : "-",
           code, bytes);
}

/*
 * Open the file behind path. realpath() follows any symlinks, and the result
 * must still be inside the root, so a link pointing at /etc is refused.
 * Returns 0 and fills fd and size, or an HTTP error code.
 */
static int open_file(const char *path, int *fd, off_t *size,
                     const char **type, int follow_dir)
{
    char *real = realpath(path, NULL);
    if (!real)
        return errno == EACCES ? 403 : 404;

    if (strncmp(real, root_real, root_len) != 0 ||
        (real[root_len] != '/' && real[root_len] != '\0' &&
         !(root_len == 1 && root_real[0] == '/'))) {
        free(real);
        return 403;
    }

    struct stat st;
    if (stat(real, &st) != 0) {
        free(real);
        return 404;
    }

    if (S_ISDIR(st.st_mode)) {
        char index[HTTP_MAX_PATH];
        int n = snprintf(index, sizeof index, "%s/index.html", real);
        free(real);
        if (!follow_dir || n < 0 || (size_t)n >= sizeof index)
            return 404;
        return open_file(index, fd, size, type, 0);
    }

    if (!S_ISREG(st.st_mode)) {
        free(real);
        return 403;
    }

    *fd = open(real, O_RDONLY);
    if (*fd < 0) {
        int code = errno == EACCES ? 403 : 404;
        free(real);
        return code;
    }

    /* Check again on the open file in case it changed after stat(). */
    if (fstat(*fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(*fd);
        free(real);
        return 403;
    }

    *size = st.st_size;
    *type = http_mime_type(real);
    free(real);
    return 0;
}

static unsigned long long send_error(int client, int code, int is_head)
{
    char body[4096];
    char head[512];
    int blen = http_error_page(code, body, sizeof body);
    if (blen < 0)
        blen = 0;

    int hlen = http_format_headers(code, "text/html; charset=utf-8",
                                   (unsigned long long)blen, head, sizeof head);
    if (hlen < 0)
        return 0;
    if (write_all(client, head, (size_t)hlen) != 0)
        return 0;
    if (is_head)
        return 0;
    if (write_all(client, body, (size_t)blen) != 0)
        return 0;
    return (unsigned long long)blen;
}

static unsigned long long send_file(int client, int fd, off_t size,
                                    const char *type, int is_head)
{
    char head[512];
    char chunk[16384];
    unsigned long long sent = 0;

    int hlen = http_format_headers(200, type, (unsigned long long)size,
                                   head, sizeof head);
    if (hlen < 0 || write_all(client, head, (size_t)hlen) != 0 || is_head)
        return 0;

    for (;;) {
        ssize_t n = read(fd, chunk, sizeof chunk);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        if (write_all(client, chunk, (size_t)n) != 0)
            break;
        sent += (unsigned long long)n;
    }
    return sent;
}

static void handle_client(int client, const struct sockaddr_in *peer)
{
    char buf[HTTP_MAX_REQUEST + 1];
    char path[HTTP_MAX_PATH];
    struct http_request req;
    unsigned long long bytes;

    struct timeval tv = { READ_TIMEOUT_SECONDS, 0 };
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    size_t len = read_request(client, buf, HTTP_MAX_REQUEST);
    if (len == 0)
        return;

    int code = http_parse_request(buf, len, &req);
    if (code == 0)
        code = http_resolve_path(root_real, req.target, path, sizeof path);

    int fd = -1;
    off_t size = 0;
    const char *type = NULL;
    if (code == 0)
        code = open_file(path, &fd, &size, &type, 1);

    if (code == 0) {
        bytes = send_file(client, fd, size, type, req.is_head);
        close(fd);
        code = 200;
    } else {
        bytes = send_error(client, code, req.is_head);
    }
    log_request(peer, &req, code, bytes);
}

static void usage(const char *prog)
{
    fprintf(stderr, "usage: %s [port] [root]\n", prog);
    fprintf(stderr, "  port  TCP port to listen on (default 8080)\n");
    fprintf(stderr, "  root  directory to serve (default ./www)\n");
}

int main(int argc, char **argv)
{
    long port = 8080;
    const char *root = "www";

    if (argc > 3) {
        usage(argv[0]);
        return 2;
    }
    if (argc >= 2) {
        char *end;
        port = strtol(argv[1], &end, 10);
        if (*argv[1] == '\0' || *end != '\0' || port < 1 || port > 65535) {
            usage(argv[0]);
            return 2;
        }
    }
    if (argc == 3)
        root = argv[2];

    char *real = realpath(root, NULL);
    if (!real) {
        perror(root);
        return 1;
    }
    if (strlen(real) >= sizeof root_real) {
        fprintf(stderr, "%s: path too long\n", root);
        free(real);
        return 1;
    }
    strcpy(root_real, real);
    root_len = strlen(root_real);
    free(real);

    struct stat st;
    if (stat(root_real, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "%s: not a directory\n", root_real);
        return 1;
    }

    /* A client that hangs up mid-response should not kill the server. */
    signal(SIGPIPE, SIG_IGN);
    /* Flush each log line even when stdout is a pipe or a file. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) {
        perror("socket");
        return 1;
    }

    int yes = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

    /* Loopback only. This is a learning project, not something to expose. */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((unsigned short)port);

    if (bind(server, (struct sockaddr *)&addr, sizeof addr) != 0) {
        perror("bind");
        close(server);
        return 1;
    }
    if (listen(server, 16) != 0) {
        perror("listen");
        close(server);
        return 1;
    }

    printf("serving %s on http://127.0.0.1:%ld/\n", root_real, port);

    for (;;) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof peer;
        int client = accept(server, (struct sockaddr *)&peer, &plen);
        if (client < 0) {
            if (errno == EINTR || errno == ECONNABORTED)
                continue;
            perror("accept");
            break;
        }
        handle_client(client, &peer);
        close(client);
    }

    close(server);
    return 1;
}
