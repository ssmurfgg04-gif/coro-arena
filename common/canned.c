/* canned.c - see canned.h. Routing logic mirrors the sink baseline exactly
 * so response bytes and pass/fail behaviour are identical across servers. */
#define _GNU_SOURCE
#include "canned.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char feed_resp[16384], post_resp[16384], like_resp[512], create_resp[16384], health_resp[512];
static int feed_len, post_len, like_len, create_len, health_len;
static int inited;

static char *file_read(const char *path, int *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc(n + 1);
    if (!b || fread(b, 1, n, f) != (size_t)n) { perror("fread"); exit(1); }
    fclose(f);
    b[n] = 0;
    *out_len = (int)n;
    return b;
}

int canned_init(const char *feed_path, const char *post_path)
{
    if (inited) return 0;
    int fl, pl;
    char *fb = file_read(feed_path, &fl);
    char *pb = file_read(post_path, &pl);

    feed_len   = snprintf(feed_resp, sizeof feed_resp,
                          "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                          "Content-Length: %d\r\n\r\n", fl);
    memcpy(feed_resp + feed_len, fb, fl);
    feed_len += fl;

    post_len  = snprintf(post_resp, sizeof post_resp,
                         "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                         "Content-Length: %d\r\n\r\n", pl);
    memcpy(post_resp + post_len, pb, pl);
    post_len += pl;

    like_len  = snprintf(like_resp, sizeof like_resp,
                         "HTTP/1.1 201 Created\r\nContent-Type: application/json\r\n"
                         "Content-Length: 28\r\n\r\n{\"post_id\":1,\"like_count\":1}");

    create_len = snprintf(create_resp, sizeof create_resp,
                          "HTTP/1.1 201 Created\r\nContent-Type: application/json\r\n"
                          "Content-Length: %d\r\n\r\n", pl);
    memcpy(create_resp + create_len, pb, pl);
    create_len += pl;

    health_len = snprintf(health_resp, sizeof health_resp,
                          "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                          "Content-Length: 15\r\n\r\n{\"status\":\"ok\"}");

    free(fb);
    free(pb);
    inited = 1;
    return 0;
}

const char *canned_route(const char *line, int len, int *rlen)
{
    if (len >= 11 && !memcmp(line, "GET /health", 11)) { *rlen = health_len; return health_resp; }
    if (len >= 9  && !memcmp(line, "GET /feed", 9))    { *rlen = feed_len;    return feed_resp; }
    if (len >= 10 && !memcmp(line, "GET /posts", 10))  { *rlen = post_len;    return post_resp; }
    if (len >= 5  && !memcmp(line, "POST ", 5)) {
        if (len >= 12 && !memcmp(line + 5, "/posts/", 7) && memmem(line, len, "/like", 5)) {
            *rlen = like_len; return like_resp;
        }
        *rlen = create_len; return create_resp;
    }
    *rlen = health_len; return health_resp; /* unknown: cheap answer */
}

const char *canned_health(int *out_len)
{
    *out_len = health_len;
    return health_resp;
}
