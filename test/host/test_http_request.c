/**
 * @file test_http_request.c
 * @brief Request parsing, including the input a well-behaved client never sends.
 *
 * The translation unit is included rather than linked: the request-line parser and
 * the Content-Length scan are static, and they read a static buffer. Reaching them
 * through the socket layer would need a server, a port and a scheduler to test
 * string handling.
 *
 * No socket is opened here and no task is started.
 */

#include "check.h"

#include "../../components/http_server/http_server.c"

void test_http_request(void);

/** @brief Put @p text in the head buffer as if it had been received. */
static void given_head(const char *text)
{
    const size_t length = strlen(text);
    CHECK(length <= HTTP_REQUEST_HEAD_MAX);
    memcpy(s_head, text, length);
    s_head[length] = '\0';

    s_method[0] = '\0';
    s_target[0] = '\0';
}

static void test_request_line(void)
{
    given_head("GET /status HTTP/1.1\r\n\r\n");
    CHECK(parse_request_line());
    CHECK_EQ_STR(s_method, "GET");
    CHECK_EQ_STR(s_target, "/status");

    /* The query string is part of the target and is not separated here. */
    given_head("POST /v1/power?force=1 HTTP/1.0\r\n\r\n");
    CHECK(parse_request_line());
    CHECK_EQ_STR(s_method, "POST");
    CHECK_EQ_STR(s_target, "/v1/power?force=1");

    /* Truncated before the target, which is what a connection cut mid-line gives. */
    given_head("GET");
    CHECK(!parse_request_line());

    given_head("GET /status");
    CHECK(!parse_request_line());

    /* An empty method or target: two spaces where one belongs. */
    given_head(" /status HTTP/1.1\r\n\r\n");
    CHECK(!parse_request_line());

    given_head("GET  HTTP/1.1\r\n\r\n");
    CHECK(!parse_request_line());

    given_head("");
    CHECK(!parse_request_line());

    /* Longer than the buffers accept. Both limits are refused rather than
     * truncated: a truncated target would be answered as if it were a different,
     * shorter path. */
    {
        char line[HTTP_REQUEST_HEAD_MAX + 1u];
        int written = snprintf(line, sizeof(line), "%.*s /status HTTP/1.1\r\n\r\n",
                               (int)(METHOD_MAX + 1u), "AAAAAAAAAAAAAAAA");
        CHECK(written > 0);
        given_head(line);
        CHECK(!parse_request_line());
    }
    {
        char target[TARGET_MAX + 2u];
        memset(target, 'a', sizeof(target) - 1u);
        target[0] = '/';
        target[sizeof(target) - 1u] = '\0';

        char line[HTTP_REQUEST_HEAD_MAX + 1u];
        int written = snprintf(line, sizeof(line), "GET %s HTTP/1.1\r\n\r\n", target);
        CHECK(written > 0);
        given_head(line);
        CHECK(!parse_request_line());
    }

    /* Exactly at the limit is accepted — the boundary the test above brackets. */
    {
        char target[TARGET_MAX + 1u];
        memset(target, 'a', sizeof(target) - 1u);
        target[0] = '/';
        target[sizeof(target) - 1u] = '\0';

        char line[HTTP_REQUEST_HEAD_MAX + 1u];
        int written = snprintf(line, sizeof(line), "GET %s HTTP/1.1\r\n\r\n", target);
        CHECK(written > 0);
        given_head(line);
        CHECK(parse_request_line());
        CHECK_EQ(strlen(s_target), TARGET_MAX);
    }
}

static void test_header_lookup(void)
{
    const char *headers = "Host: hub\r\nContent-Length: 17\r\nX-Api-Key: abc\r\n\r\n";
    size_t length = 0;

    const char *host = http_request_header(headers, "Host", &length);
    CHECK(host != NULL);
    CHECK_EQ(length, 3);
    CHECK(host != NULL && strncmp(host, "hub", length) == 0);

    /* Field names are case-insensitive, and a client is free to use any case. */
    const char *key = http_request_header(headers, "x-api-key", &length);
    CHECK(key != NULL);
    CHECK_EQ(length, 3);
    CHECK(key != NULL && strncmp(key, "abc", length) == 0);

    CHECK(http_request_header(headers, "Authorization", &length) == NULL);

    /* Optional whitespace after the colon belongs to neither name nor value. */
    const char *padded = "Host: \t  hub \r\n\r\n";
    const char *value = http_request_header(padded, "Host", &length);
    CHECK(value != NULL);
    CHECK(value != NULL && value[0] == 'h');
    CHECK_EQ(length, 4); /* "hub " — trailing space is inside the value */

    /* A name that is a prefix of another must not match it: "Content" is not
     * "Content-Length", and answering as if it were reads the wrong field. */
    CHECK(http_request_header(headers, "Content", &length) == NULL);

    /* An unterminated final line still yields its value rather than running off
     * the end of the buffer. */
    const char *unterminated = "Host: hub";
    const char *tail = http_request_header(unterminated, "Host", &length);
    CHECK(tail != NULL);
    CHECK_EQ(length, 3);

    CHECK(http_request_header("\r\n", "Host", &length) == NULL);
    CHECK(http_request_header("", "Host", &length) == NULL);
}

static void test_content_length(void)
{
    size_t declared = 0;

    given_head("POST /v1/power HTTP/1.1\r\nHost: hub\r\n\r\n");
    CHECK_EQ(content_length(&declared), LENGTH_ABSENT);

    given_head("POST /v1/power HTTP/1.1\r\nContent-Length: 17\r\n\r\n");
    CHECK_EQ(content_length(&declared), LENGTH_PRESENT);
    CHECK_EQ(declared, 17);

    given_head("POST /v1/power HTTP/1.1\r\nContent-Length: 0\r\n\r\n");
    CHECK_EQ(content_length(&declared), LENGTH_PRESENT);
    CHECK_EQ(declared, 0);

    /* Not a number, negative, or trailing rubbish. Each of these has been a way
     * into a request-smuggling bug in some server or other. */
    given_head("POST /v1/power HTTP/1.1\r\nContent-Length: abc\r\n\r\n");
    CHECK_EQ(content_length(&declared), LENGTH_INVALID);

    given_head("POST /v1/power HTTP/1.1\r\nContent-Length: -1\r\n\r\n");
    CHECK_EQ(content_length(&declared), LENGTH_INVALID);

    given_head("POST /v1/power HTTP/1.1\r\nContent-Length: \r\n\r\n");
    CHECK_EQ(content_length(&declared), LENGTH_INVALID);

    /* Two of them are refused rather than resolved: picking one and hoping is the
     * shape of the bug, whichever one is picked. */
    given_head("POST /v1/power HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n");
    CHECK_EQ(content_length(&declared), LENGTH_INVALID);

    /*
     * Longer than the body buffer is reported faithfully rather than clamped.
     * The limit is serve_connection's to apply — it answers 413 — and a parser
     * that quietly reduced the number here would turn a request that must be
     * refused into one that looks acceptable and is then read short.
     */
    {
        char line[HTTP_REQUEST_HEAD_MAX + 1u];
        int written = snprintf(line, sizeof(line),
                               "POST /v1/power HTTP/1.1\r\nContent-Length: %u\r\n\r\n",
                               (unsigned)(HTTP_REQUEST_BODY_MAX + 1u));
        CHECK(written > 0);
        given_head(line);
        CHECK_EQ(content_length(&declared), LENGTH_PRESENT);
        CHECK_EQ(declared, HTTP_REQUEST_BODY_MAX + 1u);
    }
}

void test_http_request(void)
{
    check_begin("http request parsing");

    test_request_line();
    test_header_lookup();
    test_content_length();
}
