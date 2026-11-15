<img src="assets/jd-logo.png" width="96" alt="JD logo">

# jd-tiny-httpd

A small static file server written in C11 for Linux. It speaks just enough
HTTP/1.1 to serve a folder of files to a browser or to `curl`.

## Why I built it

Most of what I write day to day sits on top of a framework that hides the
network completely. I wanted to see what is actually going on underneath:
what a request looks like as raw bytes, what `accept()` gives you, how a
server decides which file to send, and how easy it is to get that last part
wrong. Writing one from scratch in C, with no libraries apart from the
standard ones, seemed like the most honest way to find out.

## What it does

- Serves files from a root directory over HTTP/1.1, one request per
  connection (`Connection: close`).
- Handles `GET` and `HEAD`. Anything else gets `405` with an `Allow` header.
- Responds with `200`, `400`, `403`, `404`, `405` or `414`.
- Sets `Content-Type` from the file extension and always sends
  `Content-Length`.
- Percent-decodes the path, so `/my%20notes.txt` works.
- Serves `index.html` when a directory is requested.
- Refuses path traversal, including encoded forms such as `%2e%2e`, and
  symlinks that point outside the root.
- Prints one access log line per request to stdout.
- Shows a small styled error page for 4xx responses.

## Building and running

You need a C compiler and `make` on Linux (or any POSIX system).

```sh
make                         # builds ./jd-tiny-httpd
make run                     # serves ./www on port 8080
./jd-tiny-httpd 9000 ~/site  # any port and folder
```

Then open <http://127.0.0.1:8080/>. Both arguments are optional; the
defaults are port 8080 and `./www`. The server only listens on 127.0.0.1.

A log line looks like this:

```
127.0.0.1 [2026-11-15T10:42:07Z] "GET /index.html HTTP/1.1" 200 2981
```

## Tests

```sh
make test
```

This builds `tests/test_http.c` against `src/http.c` and runs it. The tests
are a plain C file with a couple of checking macros, so there is nothing to
install. They cover request line parsing, percent-decoding, path resolution,
traversal attempts, MIME lookup, header formatting and the error page.

Because the parsing code has no socket calls in it, the tests also build on
Windows with MinGW:

```sh
gcc -std=c11 -Wall -Wextra -Werror -O2 src/http.c tests/test_http.c -o test.exe
./test.exe
```

GitHub Actions runs the build, the unit tests, the unit tests again under
Valgrind, and a smoke test that starts the real server against a temporary
folder and checks the status codes with `curl` (including the traversal
cases and a symlink that points out of the root).

## Design notes

### Parsing is kept away from the sockets

The code is split into two files:

- `src/http.c` takes bytes and strings and gives back a parsed request, a
  file path or a status code. It never reads from a socket or opens a file.
- `src/main.c` does the networking: `socket`, `bind`, `listen`, `accept`,
  `read` and `write`, plus opening and sending the file.

Everything that can go wrong with untrusted input happens in `http.c`, and
that is the part I wanted to test properly. Keeping it pure means the tests
can call it directly with nasty strings, with no server running and no
ports involved. It also meant I could work on it on my Windows laptop.

### How traversal is blocked

This was the part I spent the most time on, because a static file server
that can be talked into reading `/etc/passwd` is worse than no server at
all. A request goes through these steps in `http_resolve_path`:

1. Anything after `?` or `#` is dropped.
2. The target must start with `/`. Absolute-form targets such as
   `http://host/x`, and things like `*`, get a 400.
3. The path is percent-decoded exactly once. Bad escapes and `%00` give a
   400. Decoding happens *before* any checks, so `%2e%2e` and `%2f` are
   seen as the `..` and `/` they really are.
4. A backslash anywhere gives a 403.
5. The path is split on `/` and rebuilt under the root one segment at a
   time. Empty segments and `.` are dropped. Any segment that starts with a
   dot is refused with a 403. That covers `..`, and it also stops hidden
   files like `.git/` or `.env` being served by accident.

The check is per segment rather than a search for `..` in the string, so a
file called `a..b.txt` is still allowed. The tests check both sides of that.

There is a second check in `main.c`. Before opening a file, the server calls
`realpath()` on it and makes sure the result is still inside the real path
of the root. This catches symlinks inside the served folder that point
somewhere else.

### Other choices

- One connection is handled at a time. A slow client could hold things up,
  so each connection gets a five second read timeout.
- `SIGPIPE` is ignored, so a browser that closes the tab half way through a
  download does not kill the server.
- The request head is capped at 8 KB and the target at 2048 bytes. A longer
  target gets a 414.
- All the buffers are fixed size, apart from the string
  `realpath()` returns, which is freed straight away.

## Limitations

- No keep-alive, no chunked encoding, no range requests and no
  compression.
- Only one request is handled at a time.
- Header lines are read but ignored, including `Host`.
- No `Date` or `Last-Modified` headers, so browsers will not cache properly.
- Any file or folder starting with a dot is off limits, which also blocks
  things like `/.well-known/`.
- IPv4 and loopback only.
- It has not had a proper security review and should not be put on the
  public internet.

## What I would add next

- `poll()` or a small thread pool so slow clients do not block everyone.
- Keep-alive, then `Last-Modified` and `If-Modified-Since`.
- `sendfile()` on Linux instead of the read and write loop.
- Fuzzing `http_parse_request` and `http_resolve_path` with libFuzzer.
- A config option for the listen address and IPv6.

## Layout

```
src/http.h, src/http.c   parsing, path resolution, MIME types, headers
src/main.c               sockets, file handling, access log
tests/test_http.c        unit tests
www/index.html           example page served by `make run`
```

## Licence

MIT. See [LICENSE](LICENSE).
