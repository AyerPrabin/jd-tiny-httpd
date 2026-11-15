#include "http.h"

#include <stdio.h>
#include <string.h>

int http_parse_request(const char *buf, size_t len, struct http_request *req)
{
    memset(req, 0, sizeof *req);

    size_t eol = 0;
    while (eol < len && buf[eol] != '\n')
        eol++;

    if (eol == len) {
        /* No end of line in a full buffer usually means a huge target. */
        const char *sp = memchr(buf, ' ', len);
        if (sp && (size_t)(buf + len - (sp + 1)) > HTTP_MAX_TARGET)
            return 414;
        return 400;
    }

    const char *line = buf;
    const char *end = buf + eol;
    if (end > line && end[-1] == '\r')
        end--;

    /* Method: one or more upper-case letters. */
    const char *sp1 = memchr(line, ' ', (size_t)(end - line));
    if (!sp1)
        return 400;
    size_t mlen = (size_t)(sp1 - line);
    if (mlen == 0 || mlen >= sizeof req->method)
        return 400;
    for (size_t i = 0; i < mlen; i++) {
        if (line[i] < 'A' || line[i] > 'Z')
            return 400;
    }
    memcpy(req->method, line, mlen);

    /* Target: up to the next space, no control characters. */
    const char *t = sp1 + 1;
    const char *sp2 = memchr(t, ' ', (size_t)(end - t));
    if (!sp2)
        return 400;
    size_t tlen = (size_t)(sp2 - t);
    if (tlen == 0)
        return 400;
    if (tlen > HTTP_MAX_TARGET)
        return 414;
    for (size_t i = 0; i < tlen; i++) {
        unsigned char c = (unsigned char)t[i];
        if (c < 0x21 || c == 0x7f)
            return 400;
    }
    memcpy(req->target, t, tlen);

    /* Version: only HTTP/1.0 and HTTP/1.1. */
    const char *v = sp2 + 1;
    size_t vlen = (size_t)(end - v);
    if (vlen != 8 || (memcmp(v, "HTTP/1.0", 8) != 0 && memcmp(v, "HTTP/1.1", 8) != 0))
        return 400;
    memcpy(req->version, v, vlen);

    if (strcmp(req->method, "GET") == 0)
        return 0;
    if (strcmp(req->method, "HEAD") == 0) {
        req->is_head = 1;
        return 0;
    }
    return 405;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

int http_url_decode(const char *in, size_t inlen, char *out, size_t outlen)
{
    size_t o = 0;

    if (outlen == 0)
        return -1;

    for (size_t i = 0; i < inlen; i++) {
        char c = in[i];
        if (c == '%') {
            if (inlen - i < 3)
                return -1;
            int hi = hex_value(in[i + 1]);
            int lo = hex_value(in[i + 2]);
            if (hi < 0 || lo < 0)
                return -1;
            c = (char)(hi * 16 + lo);
            if (c == '\0')
                return -1;
            i += 2;
        }
        if (o + 1 >= outlen)
            return -1;
        out[o++] = c;
    }
    out[o] = '\0';
    return 0;
}

int http_resolve_path(const char *root, const char *target,
                      char *out, size_t outlen)
{
    char path[HTTP_MAX_TARGET + 1];

    /* The query string and fragment are not part of the file name. */
    size_t tlen = strcspn(target, "?#");
    if (tlen > HTTP_MAX_TARGET)
        return 414;

    /*
     * Only origin-form targets ("/something") are accepted. This turns away
     * absolute-form ("http://host/x"), "*" and relative targets.
     */
    if (tlen == 0 || target[0] != '/')
        return 400;

    /*
     * Decode first and check afterwards, so "%2e%2e" and "%2f" are seen
     * as the ".." and "/" they really are.
     */
    if (http_url_decode(target, tlen, path, sizeof path) != 0)
        return 400;
    if (strchr(path, '\\'))
        return 403;

    size_t rlen = strlen(root);
    if (rlen == 0)
        return 403;
    while (rlen > 1 && root[rlen - 1] == '/')
        rlen--;
    if (rlen + 1 > outlen)
        return 414;
    memcpy(out, root, rlen);
    size_t o = rlen;

    /*
     * Rebuild the path one segment at a time. Empty and "." segments are
     * dropped. Any segment starting with a dot is refused, which covers
     * ".." as well as hidden files such as .git or .env.
     */
    const char *p = path;
    while (*p) {
        while (*p == '/')
            p++;
        if (*p == '\0')
            break;
        size_t seg = strcspn(p, "/");
        if (seg == 1 && p[0] == '.') {
            p += seg;
            continue;
        }
        if (p[0] == '.')
            return 403;
        if (o + 1 + seg + 1 > outlen)
            return 414;
        if (out[o - 1] != '/')
            out[o++] = '/';
        memcpy(out + o, p, seg);
        o += seg;
        p += seg;
    }

    /* "/docs/" means the index page of that directory. */
    if (path[strlen(path) - 1] == '/') {
        const char *index = "index.html";
        size_t ilen = strlen(index);
        if (o + 1 + ilen + 1 > outlen)
            return 414;
        if (out[o - 1] != '/')
            out[o++] = '/';
        memcpy(out + o, index, ilen);
        o += ilen;
    }

    out[o] = '\0';
    return 0;
}

static int same_ignoring_case(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z')
            x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z')
            y = (char)(y - 'A' + 'a');
        if (x != y)
            return 0;
    }
    return *a == *b;
}

static const struct {
    const char *ext;
    const char *type;
} mime_table[] = {
    { "html",  "text/html; charset=utf-8" },
    { "htm",   "text/html; charset=utf-8" },
    { "css",   "text/css; charset=utf-8" },
    { "js",    "text/javascript; charset=utf-8" },
    { "mjs",   "text/javascript; charset=utf-8" },
    { "json",  "application/json" },
    { "txt",   "text/plain; charset=utf-8" },
    { "md",    "text/plain; charset=utf-8" },
    { "xml",   "application/xml" },
    { "png",   "image/png" },
    { "jpg",   "image/jpeg" },
    { "jpeg",  "image/jpeg" },
    { "gif",   "image/gif" },
    { "webp",  "image/webp" },
    { "svg",   "image/svg+xml" },
    { "ico",   "image/x-icon" },
    { "pdf",   "application/pdf" },
    { "wasm",  "application/wasm" },
    { "woff",  "font/woff" },
    { "woff2", "font/woff2" },
    { "mp4",   "video/mp4" },
};

const char *http_mime_type(const char *path)
{
    const char *slash = strrchr(path, '/');
    const char *name = slash ? slash + 1 : path;
    const char *dot = strrchr(name, '.');

    if (dot && dot != name) {
        for (size_t i = 0; i < sizeof mime_table / sizeof mime_table[0]; i++) {
            if (same_ignoring_case(dot + 1, mime_table[i].ext))
                return mime_table[i].type;
        }
    }
    return "application/octet-stream";
}

const char *http_status_text(int code)
{
    switch (code) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 414: return "URI Too Long";
    default:  return "Internal Server Error";
    }
}

int http_format_headers(int code, const char *content_type,
                        unsigned long long length, char *out, size_t outlen)
{
    int n = snprintf(out, outlen,
                     "HTTP/1.1 %d %s\r\n"
                     "Server: jd-tiny-httpd\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %llu\r\n"
                     "X-Content-Type-Options: nosniff\r\n"
                     "%s"
                     "Connection: close\r\n"
                     "\r\n",
                     code, http_status_text(code), content_type, length,
                     code == 405 ? "Allow: GET, HEAD\r\n" : "");
    if (n < 0 || (size_t)n >= outlen)
        return -1;
    return n;
}

static const char *error_message(int code)
{
    switch (code) {
    case 400: return "The server could not understand that request.";
    case 403: return "That path is not allowed.";
    case 404: return "There is no file at that address.";
    case 405: return "Only GET and HEAD are supported here.";
    case 414: return "That address is longer than this server accepts.";
    default:  return "Something went wrong on the server.";
    }
}

/* Colours: #161C14 background, #DBA958 accent, #F2EEE3 text. */
static const char error_template[] =
    "<!doctype html>\n"
    "<html lang=\"en\">\n"
    "<head>\n"
    "<meta charset=\"utf-8\">\n"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
    "<title>%d %s</title>\n"
    "<style>\n"
    "body{margin:0;min-height:100vh;display:flex;align-items:center;"
    "justify-content:center;background:#161C14;color:#F2EEE3;"
    "font-family:Inter,system-ui,sans-serif}\n"
    "main{margin:24px;padding:48px 40px;max-width:420px;text-align:center;"
    "border:1px solid rgba(242,238,227,.1);border-radius:20px}\n"
    "h1{margin:0;font:600 4.5rem/1 \"Cormorant Garamond\",Georgia,serif;"
    "color:#DBA958}\n"
    "h2{margin:.75rem 0 0;font:500 1.6rem \"Cormorant Garamond\",Georgia,serif}\n"
    "p{margin:1rem 0 2rem;line-height:1.6;color:rgba(242,238,227,.7)}\n"
    "a{display:inline-block;padding:.7rem 1.6rem;border-radius:999px;"
    "background:#DBA958;color:#161C14;font-weight:600;text-decoration:none}\n"
    "</style>\n"
    "</head>\n"
    "<body>\n"
    "<main>\n"
    "<h1>%d</h1>\n"
    "<h2>%s</h2>\n"
    "<p>%s</p>\n"
    "<a href=\"/\">Back to the start</a>\n"
    "</main>\n"
    "</body>\n"
    "</html>\n";

int http_error_page(int code, char *out, size_t outlen)
{
    const char *text = http_status_text(code);
    int n = snprintf(out, outlen, error_template,
                     code, text, code, text, error_message(code));
    if (n < 0 || (size_t)n >= outlen)
        return -1;
    return n;
}
