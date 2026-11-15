/*
 * http.h - request parsing, path resolution and MIME lookup.
 *
 * Nothing in here touches sockets or the filesystem, so all of it can be
 * unit tested on any machine with a C11 compiler.
 */
#ifndef JD_HTTP_H
#define JD_HTTP_H

#include <stddef.h>

/* Largest request head we will read before giving up. */
#define HTTP_MAX_REQUEST 8192

/* Longest request target we accept. Anything longer gets a 414. */
#define HTTP_MAX_TARGET 2048

/* Room for root + target + "/index.html". */
#define HTTP_MAX_PATH 4096

struct http_request {
    char method[16];
    char target[HTTP_MAX_TARGET + 1];
    char version[16];
    int is_head;
};

/*
 * Parse the request line at the start of buf. The header lines that follow
 * are ignored. Returns 0 on success, or 400, 405 or 414. Whatever could be
 * parsed (for example the method on a 405) is left in req for logging.
 */
int http_parse_request(const char *buf, size_t len, struct http_request *req);

/*
 * Percent-decode in[0..inlen) into out. Fails on bad escapes such as "%zz",
 * a truncated "%4", and on "%00". '+' is left alone because it only means
 * a space in form data, not in a path. Returns 0 or -1.
 */
int http_url_decode(const char *in, size_t inlen, char *out, size_t outlen);

/*
 * Turn a request target into a filesystem path under root.
 * Drops any query string, decodes the path and refuses anything that could
 * leave the root. Returns 0, 400 (malformed), 403 (refused) or 414 (too long).
 */
int http_resolve_path(const char *root, const char *target,
                      char *out, size_t outlen);

/* Content-Type for a file name, based on its extension. */
const char *http_mime_type(const char *path);

/* Reason phrase for a status code, e.g. "Not Found". */
const char *http_status_text(int code);

/*
 * Write the status line and headers (ending with the blank line) into out.
 * Returns the number of bytes written, or -1 if out is too small.
 */
int http_format_headers(int code, const char *content_type,
                        unsigned long long length, char *out, size_t outlen);

/* Small HTML page for an error status. Returns length or -1. */
int http_error_page(int code, char *out, size_t outlen);

#endif
