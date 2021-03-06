#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/errno.h>

#define SERVER_NAME "oHTTP"
#define SERVER_VERSION "1.0"

struct HTTPHeaderField {
    char *name;
    char *value;
    struct HTTPHeaderField *next;
};

struct HTTPRequest {
    int protocol_minor_version;
    char *method;
    char *path;
    struct HTTPHeaderField *header;
    char *body;
    long length;
};

struct FileInfo {
    char *path;
    long size;
    int ok;
};

// exit with an error message
static void
log_exit(char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

// memory allocation
static void*
xmalloc(size_t sz)
{
    void *p = malloc(sz);
    if (!p) log_exit("failed to allocate memory");
    return p;
}

static void
signal_exit(int sig)
{
    log_exit("exit by signal %d", sig);
}

static void
trap_signal(int sig, void (*handler)(int))
{
    struct sigaction act;
    act.sa_handler = handler;
    sigemptyset(&act.sa_mask);
    act.sa_flags = SA_RESTART;
    sigaction(sig, &act, NULL);
}

// handle socket errors
static void
install_signal_handler(void)
{
    trap_signal(SIGPIPE, signal_exit);
}

static struct HTTPRequest *
parse_request_line(FILE *in)
{
    struct HTTPRequest *req = xmalloc(sizeof(struct HTTPRequest));

    char buf[4096];
    if (!fgets(buf, sizeof(buf), in))
        log_exit("failed to read request line");

    // method
    char *p = strchr(buf, ' ');
    if (!p) log_exit("parse error on request line (1)");
    req->method = xmalloc(p - buf); strncpy(req->method, buf, p - buf);
    p++;

    // path
    char *path = p;
    p = strchr(path, ' ');
    if (!p) log_exit("parse error on request line (2)");
    req->path = xmalloc(p - path);
    strncpy(req->path, path, p - path);
    p++;

    // http version
    char *http_version_head = "HTTP/1.";
    if (strncmp(p, http_version_head, strlen(http_version_head)) != 0)
        log_exit("parse error on request line (3)");
    p += strlen(http_version_head);
    req->protocol_minor_version = atoi(p++);

    return req;
}

static struct HTTPHeaderField*
parse_header_fields(FILE *in)
{
    struct HTTPHeaderField head;
    struct HTTPHeaderField *h = &head;

    char buf[4096];
    for (;;) {
        if (!fgets(buf, sizeof(buf), in))
            log_exit("failed to read request headers");

        if (buf[0] == '\n' || strcmp(buf, "\r\n") == 0) break;

        h->next = xmalloc(sizeof(struct HTTPHeaderField));
        h = h->next;

        // name
        char *p = strchr(buf, ':');
        if (!p) log_exit("parse error on request headers (1)");
        h->name = xmalloc(p - buf);
        strncpy(h->name, buf, p - buf);
        p++;
        while (*p == ' ') p++;

        // value
        char *value = p;
        p = strchr(value, '\n');
        if (!p) log_exit("parse error on request headers (2)");
        h->value = xmalloc(p - value);
        strncpy(h->value, value, p - value);
    }

    return head.next;
}

static long
get_content_length(struct HTTPRequest *req) {
    char *headername = "Content-Length";
    for (struct HTTPHeaderField *h = req->header; h; h = h->next) {
        if (strncmp(h->name, headername, strlen(headername)) == 0)
            return atol(h->value);
    }
    return 0;
}

static struct HTTPRequest*
read_request(FILE *in)
{
    struct HTTPRequest *req = parse_request_line(in);
    req->header = parse_header_fields(in);
    req->length = get_content_length(req);

    if (req->length == 0) {
        req->body = NULL;
    } else {
        req->body = xmalloc(req->length);
        if (fread(req->body, req->length, 1, in) < 1)
            log_exit("failed to read request body");
    }

    return req;
}

static void
free_request(struct HTTPRequest *req)
{
    struct HTTPHeaderField *cur, *c;
    cur = req->header;
    while (cur) {
        c = cur;
        cur = c->next;
        free(c->name);
        free(c->value);
        free(c);
    }
    free(req->method);
    free(req->path);
    free(req->body);
    free(req);
}

static char*
build_fspath(char *docroot, char *urlpath)
{
    // +1 for trailing '\0'
    char *path = xmalloc(strlen(docroot) + strlen(urlpath) + 1);
    sprintf(path, "%s%s", docroot, urlpath);
    return path;
}

static struct FileInfo*
get_fileinfo(char *docroot, char *urlpath)
{
    struct FileInfo *info = xmalloc(sizeof(struct FileInfo));
    info->path = build_fspath(docroot, urlpath);
    info->ok = 0;

    struct stat st;
    if (lstat(info->path, &st) < 0) return info;
    if (!S_ISREG(st.st_mode)) return info;
    info->ok = 1;
    info->size = st.st_size;

    return info;
}

static void
free_fileinfo(struct FileInfo *info)
{
    free(info->path);
    free(info);
}

static void
output_common_header_fields(struct HTTPRequest *req,
                            FILE *out, char *status)
{
    // Make date string.
    char date[1024];
    time_t t = time(NULL);
    struct tm *tm = gmtime(&t);
    strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S GMT", tm);

    fprintf(out, "HTTP/1.%d %s\r\n", req->protocol_minor_version, status);
    fprintf(out, "Date: %s\r\n", date);
    fprintf(out, "Server: %s/%s\r\n", SERVER_NAME, SERVER_VERSION);
    fprintf(out, "Connection: close\r\n");
}

static void
not_found(struct HTTPRequest *req, FILE *out)
{
    output_common_header_fields(req, out, "404 Not Found");
    fprintf(out, "Content-Type: text/plain\r\n");
    fprintf(out, "\r\n");
    fprintf(out, "File not found.\r\n");
}

static void
method_not_allowed(struct HTTPRequest *req, FILE *out)
{
    output_common_header_fields(req, out, "405 Method Not Allowed");
    fprintf(out, "Content-Type: text/plain\r\n");
    fprintf(out, "\r\n");
    fprintf(out, "The request method %s is not allowed.\r\n", req->method);
}

static void
not_implemented(struct HTTPRequest *req, FILE *out)
{
    output_common_header_fields(req, out, "501 Not Implemented");
    fprintf(out, "Content-Type: text/plain\r\n");
    fprintf(out, "\r\n");
    fprintf(out, "The request method %s is not implemented.\r\n", req->method);
}

static char*
guess_content_type()
{
    return "text/plain";
}

static void
do_file_response(struct HTTPRequest *req, FILE *out, char *docroot)
{
    struct FileInfo *info = get_fileinfo(docroot, req->path);
    if (!info->ok) {
        free_fileinfo(info);
        not_found(req, out);
        return;
    }

    output_common_header_fields(req, out, "200 OK");
    fprintf(out, "Content-Length: %ld\r\n", info->size);
    fprintf(out, "Content-Type: %s\r\n", guess_content_type());
    fprintf(out, "\r\n");

    int fd = open(info->path, O_RDONLY);
    if (fd < 0)
        log_exit("failed to open %s: %s", info->path, strerror(errno));

    int n;
    char buf[1024];
    for (;;) {
        n = read(fd, buf, sizeof(buf));
        if (n < 0)
            log_exit("failed to read %s: %s", info->path, strerror(errno));
        if (n == 0)
            break;

        if (fwrite(buf, 1, n, out) < n)
            log_exit("failed to write to socket: %s", strerror(errno));
    }
    close(fd);

    // ref:
    //   Why to use fflush()
    //   https://pubs.opengroup.org/onlinepubs/007904975/functions/fflush.html
    fflush(out);
    free_fileinfo(info);
}

static void
respond_to(struct HTTPRequest *req, FILE *out, char *docroot)
{
    if (strcmp(req->method, "GET") == 0)
        do_file_response(req, out, docroot);
    else if (strcmp(req->method, "POST") == 0)
        method_not_allowed(req, out);
    else
        not_implemented(req, out);
}

static void
service(FILE *in, FILE *out, char *docroot)
{
    struct HTTPRequest *req = read_request(in);
    respond_to(req, out, docroot);
    free_request(req);
}

int
main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <docroot>\n", argv[0]);
        exit(1);
    }

    install_signal_handler();
    service(stdin, stdout, argv[1]);
    exit(0);
}

/* examples:

gcc -Wall -o httpd httpd.c && ./httpd /app

GET /cat/cat.c HTTP/1.1
ACCEPT: text/html
Host: i.loveruby.net

---

GET /httpd/notfound HTTP/1.1
ACCEPT: text/html
Host: i.loveruby.net

---

POST /httpd/httpd.c HTTP/1.1
ACCEPT: text/html
Host: i.loveruby.net

---

PUT /httpd/httpd.c HTTP/1.1
ACCEPT: text/html
Host: i.loveruby.net

---

GET /httpd/httpd.c HTTP/1.1
ACCEPT: text/html
Host: i.loveruby.net
Content-Length: 26

Hello, this is yutarofire.

 */

/* DEBUG HTTPRequest */
// printf("===request line===\n");
// printf("method: %s\n", req->method);
// printf("path: %s\n", req->path);
// printf("version: %d\n", req->protocol_minor_version);
// printf("===headers===\n");
// for (struct HTTPHeaderField *h = req->header; h; h = h->next) {
//     printf("%s: %s\n", h->name, h->value);
// }
// printf("(length: %ld)\n", req->length);
// printf("===body===\n");
// if (req->body)
//     printf("%s\n", req->body);
// else
//     printf("body is NULL\n");

/* DEBUG FileInfo */
// printf("===file info===\n");
// printf("path: %s\n", info->path);
// printf("size: %ld\n", info->size);
// printf("ok: %d\n", info->ok);
