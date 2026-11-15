/*
 * Unit tests for src/http.c. No framework, just a couple of macros that
 * count passes and failures and print where a check went wrong.
 */
#include "../src/http.h"

#include <stdio.h>
#include <string.h>

static int passed;
static int failed;

#define CHECK_INT(expr, want) check_int((expr), (want), #expr, __FILE__, __LINE__)
#define CHECK_STR(got, want) check_str((got), (want), #got, __FILE__, __LINE__)
#define CHECK(cond) check_int((cond) ? 1 : 0, 1, #cond, __FILE__, __LINE__)

static void check_int(long got, long want, const char *what,
                      const char *file, int line)
{
    if (got == want) {
        passed++;
    } else {
        failed++;
        printf("FAIL %s:%d: %s gave %ld, wanted %ld\n", file, line, what, got, want);
    }
}

static void check_str(const char *got, const char *want, const char *what,
                      const char *file, int line)
{
    if (strcmp(got, want) == 0) {
        passed++;
    } else {
        failed++;
        printf("FAIL %s:%d: %s gave \"%s\", wanted \"%s\"\n", file, line, what, got, want);
    }
}

static int parse(const char *raw, struct http_request *req)
{
    return http_parse_request(raw, strlen(raw), req);
}

static void test_parse(void)
{
    struct http_request req;

    CHECK_INT(parse("GET /index.html HTTP/1.1\r\nHost: x\r\n\r\n", &req), 0);
    CHECK_STR(req.method, "GET");
    CHECK_STR(req.target, "/index.html");
    CHECK_STR(req.version, "HTTP/1.1");
    CHECK_INT(req.is_head, 0);

    CHECK_INT(parse("HEAD / HTTP/1.0\r\n\r\n", &req), 0);
    CHECK_INT(req.is_head, 1);

    /* Bare \n line endings are tolerated. */
    CHECK_INT(parse("GET /a.txt HTTP/1.1\n\n", &req), 0);
    CHECK_STR(req.target, "/a.txt");

    CHECK_INT(parse("POST /form HTTP/1.1\r\n\r\n", &req), 405);
    CHECK_STR(req.method, "POST");
    CHECK_INT(parse("DELETE / HTTP/1.1\r\n\r\n", &req), 405);

    CHECK_INT(parse("get / HTTP/1.1\r\n\r\n", &req), 400);
    CHECK_INT(parse("GET / HTTP/2.0\r\n\r\n", &req), 400);
    CHECK_INT(parse("GET / FTP/1.1\r\n\r\n", &req), 400);
    CHECK_INT(parse("GET /\r\n\r\n", &req), 400);
    CHECK_INT(parse("GET  / HTTP/1.1\r\n\r\n", &req), 400);
    CHECK_INT(parse("GET / HTTP/1.1 extra\r\n\r\n", &req), 400);
    CHECK_INT(parse("\r\n", &req), 400);
    CHECK_INT(parse("", &req), 400);
    CHECK_INT(parse("GET /a\tb HTTP/1.1\r\n\r\n", &req), 400);
    CHECK_INT(parse("VERYLONGMETHODNAME / HTTP/1.1\r\n\r\n", &req), 400);

    /* A request with no line end yet is incomplete, so 400. */
    CHECK_INT(parse("GET / HTTP/1.1", &req), 400);
}

static void test_parse_long_target(void)
{
    static char buf[HTTP_MAX_REQUEST];
    struct http_request req;

    /* Exactly at the limit is fine. */
    strcpy(buf, "GET /");
    memset(buf + 5, 'a', HTTP_MAX_TARGET - 1);
    strcpy(buf + 4 + HTTP_MAX_TARGET, " HTTP/1.1\r\n\r\n");
    CHECK_INT(parse(buf, &req), 0);
    CHECK_INT((long)strlen(req.target), HTTP_MAX_TARGET);

    /* One byte over is a 414. */
    strcpy(buf, "GET /");
    memset(buf + 5, 'a', HTTP_MAX_TARGET);
    strcpy(buf + 5 + HTTP_MAX_TARGET, " HTTP/1.1\r\n\r\n");
    CHECK_INT(parse(buf, &req), 414);

    /* A buffer that filled up before the line ended. */
    memset(buf, 'a', sizeof buf);
    memcpy(buf, "GET /", 5);
    CHECK_INT(http_parse_request(buf, sizeof buf, &req), 414);
}

static void test_decode(void)
{
    char out[64];

    CHECK_INT(http_url_decode("/plain", 6, out, sizeof out), 0);
    CHECK_STR(out, "/plain");

    CHECK_INT(http_url_decode("/a%20b", 6, out, sizeof out), 0);
    CHECK_STR(out, "/a b");

    CHECK_INT(http_url_decode("%2e%2E%2f", 9, out, sizeof out), 0);
    CHECK_STR(out, "../");

    /* '+' stays a plus sign in a path. */
    CHECK_INT(http_url_decode("/c++.txt", 8, out, sizeof out), 0);
    CHECK_STR(out, "/c++.txt");

    /* Only the given length is read. */
    CHECK_INT(http_url_decode("/abc?x", 4, out, sizeof out), 0);
    CHECK_STR(out, "/abc");

    CHECK_INT(http_url_decode("/%zz", 4, out, sizeof out), -1);
    CHECK_INT(http_url_decode("/%4", 3, out, sizeof out), -1);
    CHECK_INT(http_url_decode("/%", 2, out, sizeof out), -1);
    CHECK_INT(http_url_decode("/a%00b", 6, out, sizeof out), -1);

    /* Output buffer too small. */
    CHECK_INT(http_url_decode("/abcdef", 7, out, 4), -1);
    CHECK_INT(http_url_decode("/", 1, out, 0), -1);
}

static int resolve(const char *target, char *out)
{
    return http_resolve_path("/srv/www", target, out, HTTP_MAX_PATH);
}

static void test_resolve(void)
{
    char out[HTTP_MAX_PATH];

    CHECK_INT(resolve("/hello.txt", out), 0);
    CHECK_STR(out, "/srv/www/hello.txt");

    CHECK_INT(resolve("/", out), 0);
    CHECK_STR(out, "/srv/www/index.html");

    CHECK_INT(resolve("/docs/", out), 0);
    CHECK_STR(out, "/srv/www/docs/index.html");

    CHECK_INT(resolve("/docs", out), 0);
    CHECK_STR(out, "/srv/www/docs");

    CHECK_INT(resolve("/my%20file.html?v=2#top", out), 0);
    CHECK_STR(out, "/srv/www/my file.html");

    CHECK_INT(resolve("/a/./b//c.txt", out), 0);
    CHECK_STR(out, "/srv/www/a/b/c.txt");

    /* A trailing slash on the root is ignored. */
    CHECK_INT(http_resolve_path("www/", "/x.css", out, sizeof out), 0);
    CHECK_STR(out, "www/x.css");

    CHECK_INT(http_resolve_path("/", "/x.css", out, sizeof out), 0);
    CHECK_STR(out, "/x.css");
}

static void test_traversal(void)
{
    char out[HTTP_MAX_PATH];

    CHECK_INT(resolve("/../etc/passwd", out), 403);
    CHECK_INT(resolve("/..", out), 403);
    CHECK_INT(resolve("/docs/../../etc/passwd", out), 403);
    CHECK_INT(resolve("/docs/..", out), 403);

    /* Encoded forms are decoded before the check. */
    CHECK_INT(resolve("/%2e%2e/etc/passwd", out), 403);
    CHECK_INT(resolve("/%2E%2E/etc/passwd", out), 403);
    CHECK_INT(resolve("/.%2e/etc/passwd", out), 403);
    CHECK_INT(resolve("/docs%2f..%2f..%2fetc%2fpasswd", out), 403);

    /* Windows separators and hidden files. */
    CHECK_INT(resolve("/..\\etc\\passwd", out), 403);
    CHECK_INT(resolve("/%5c..%5cboot.ini", out), 403);
    CHECK_INT(resolve("/.git/config", out), 403);
    CHECK_INT(resolve("/.env", out), 403);

    /* Encoded NUL and broken escapes. */
    CHECK_INT(resolve("/index.html%00.png", out), 400);
    CHECK_INT(resolve("/%zz", out), 400);

    /* Absolute-form and other targets that do not start with '/'. */
    CHECK_INT(resolve("http://example.com/etc/passwd", out), 400);
    CHECK_INT(resolve("etc/passwd", out), 400);
    CHECK_INT(resolve("*", out), 400);
    CHECK_INT(resolve("?x=1", out), 400);

    /* A double slash is just an empty segment, it stays under the root. */
    CHECK_INT(resolve("//etc/passwd", out), 0);
    CHECK_STR(out, "/srv/www/etc/passwd");

    /* Dots inside a name are fine; the check is per segment. */
    CHECK_INT(resolve("/a..b.txt", out), 0);
    CHECK_STR(out, "/srv/www/a..b.txt");
    CHECK_INT(resolve("/notes../x.txt", out), 0);
    CHECK_STR(out, "/srv/www/notes../x.txt");

    CHECK_INT(http_resolve_path("", "/x", out, sizeof out), 403);
}

static void test_resolve_lengths(void)
{
    static char target[HTTP_MAX_TARGET + 2];
    char small[16];

    memset(target, 'a', sizeof target - 1);
    target[0] = '/';
    target[sizeof target - 1] = '\0';
    CHECK_INT(http_resolve_path("/srv", target, small, sizeof small), 414);

    CHECK_INT(http_resolve_path("/srv/www", "/a-long-name.html", small, sizeof small), 414);
    CHECK_INT(http_resolve_path("/srv/www", "/", small, sizeof small), 414);
    CHECK_INT(http_resolve_path("/srv", "/abc.txt", small, sizeof small), 0);
    CHECK_STR(small, "/srv/abc.txt");
}

static void test_mime(void)
{
    CHECK_STR(http_mime_type("index.html"), "text/html; charset=utf-8");
    CHECK_STR(http_mime_type("/srv/www/INDEX.HTM"), "text/html; charset=utf-8");
    CHECK_STR(http_mime_type("style.css"), "text/css; charset=utf-8");
    CHECK_STR(http_mime_type("app.js"), "text/javascript; charset=utf-8");
    CHECK_STR(http_mime_type("data.json"), "application/json");
    CHECK_STR(http_mime_type("logo.PNG"), "image/png");
    CHECK_STR(http_mime_type("photo.jpeg"), "image/jpeg");
    CHECK_STR(http_mime_type("icon.svg"), "image/svg+xml");
    CHECK_STR(http_mime_type("font.woff2"), "font/woff2");
    CHECK_STR(http_mime_type("archive.tar.gz"), "application/octet-stream");
    CHECK_STR(http_mime_type("Makefile"), "application/octet-stream");
    CHECK_STR(http_mime_type("/srv/v1.2/readme"), "application/octet-stream");
    CHECK_STR(http_mime_type("/srv/.hidden"), "application/octet-stream");
    CHECK_STR(http_mime_type("trailing."), "application/octet-stream");
}

static void test_headers(void)
{
    char out[512];
    int n;

    n = http_format_headers(200, "text/plain; charset=utf-8", 12, out, sizeof out);
    CHECK(n > 0);
    CHECK(strncmp(out, "HTTP/1.1 200 OK\r\n", 17) == 0);
    CHECK(strstr(out, "Content-Length: 12\r\n") != NULL);
    CHECK(strstr(out, "Connection: close\r\n") != NULL);
    CHECK(strstr(out, "Allow:") == NULL);
    CHECK_INT(n, (long)strlen(out));
    CHECK(n >= 4 && strcmp(out + n - 4, "\r\n\r\n") == 0);

    n = http_format_headers(405, "text/html; charset=utf-8", 0, out, sizeof out);
    CHECK(strncmp(out, "HTTP/1.1 405 Method Not Allowed\r\n", 33) == 0);
    CHECK(strstr(out, "Allow: GET, HEAD\r\n") != NULL);

    CHECK_INT(http_format_headers(200, "text/plain", 1, out, 10), -1);
}

static void test_error_page(void)
{
    char out[4096];
    int n;

    CHECK_STR(http_status_text(403), "Forbidden");
    CHECK_STR(http_status_text(414), "URI Too Long");
    CHECK_STR(http_status_text(999), "Internal Server Error");

    n = http_error_page(404, out, sizeof out);
    CHECK(n > 0);
    CHECK_INT(n, (long)strlen(out));
    CHECK(strstr(out, "<title>404 Not Found</title>") != NULL);
    CHECK(strstr(out, "#161C14") != NULL);

    CHECK_INT(http_error_page(404, out, 32), -1);
}

int main(void)
{
    test_parse();
    test_parse_long_target();
    test_decode();
    test_resolve();
    test_traversal();
    test_resolve_lengths();
    test_mime();
    test_headers();
    test_error_page();

    printf("%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
