/**
 * @file test_http_request.c
 * @brief The string handling on both sides of the socket: requests in, heads out.
 *
 * The translation unit is included rather than linked: the request-line parser, the
 * Content-Length scan and the head builder are all static, and the first two read a
 * static buffer. Reaching them through the socket layer would need a server, a port
 * and a scheduler to test string handling.
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

static void test_transfer_encoding_is_detected(void)
{
    /* Not implemented, and until now not refused either: the chunk framing was read
     * as though it were content, so "5\r\nhello\r\n0\r\n\r\n" reached a handler as a
     * body beginning "5" and the size prefix became part of a credential. */
    given_head("POST /join HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n");
    CHECK(declares_transfer_encoding());

    /* Any encoding, not only chunked. This server implements none of them. */
    given_head("POST /join HTTP/1.1\r\nTransfer-Encoding: gzip\r\n\r\n");
    CHECK(declares_transfer_encoding());

    /* Header names are case-insensitive, and a header smuggled past a case-sensitive
     * check is the whole trick. */
    given_head("POST /join HTTP/1.1\r\ntransfer-encoding: chunked\r\n\r\n");
    CHECK(declares_transfer_encoding());
    given_head("POST /join HTTP/1.1\r\nTRANSFER-ENCODING: chunked\r\n\r\n");
    CHECK(declares_transfer_encoding());

    /* Found wherever it sits among the headers, not only first. */
    given_head("POST /join HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n"
               "Content-Length: 5\r\n\r\n");
    CHECK(declares_transfer_encoding());

    given_head("POST /join HTTP/1.1\r\nContent-Length: 5\r\n\r\n");
    CHECK(!declares_transfer_encoding());
    given_head("GET /status HTTP/1.1\r\n\r\n");
    CHECK(!declares_transfer_encoding());

    /* The name has to be a header name, not text inside another header's value. */
    given_head("POST /join HTTP/1.1\r\nX-Note: Transfer-Encoding: chunked\r\n\r\n");
    CHECK(!declares_transfer_encoding());
}

/**
 * @brief Feed @p request to serve_connection over a socket pair and return the reply.
 *
 * A real socket, and the real function: lwIP's API is the BSD one, so the stub
 * points at the host's and serve_connection runs here unchanged. Everything above
 * this line tests a parser in isolation, which cannot show whether the server ever
 * consults it — removing the Transfer-Encoding refusal left all of them passing.
 *
 * @return Length of the reply, or -1 if the connection produced none.
 */
static ssize_t serve_once(const char *request, char *reply, size_t reply_size)
{
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
        check_fail(__FILE__, __LINE__, "socketpair failed");
        return -1;
    }

    const size_t length = strlen(request);
    const ssize_t sent = write(pair[1], request, length);
    CHECK(sent >= 0 && (size_t)sent == length);
    /* Half-close, so receive_head sees the end of the request rather than blocking
     * on a peer that is still notionally able to send more. */
    shutdown(pair[1], SHUT_WR);

    serve_connection(pair[0]);

    const ssize_t received = read(pair[1], reply, reply_size - 1u);
    reply[received > 0 ? (size_t)received : 0u] = '\0';

    close(pair[0]);
    close(pair[1]);
    return received;
}

/** @brief A handler that answers 200 and records the body it was given. */
static char s_seen_body[HTTP_REQUEST_BODY_MAX + 1u];

static void recording_handler(const http_request_t *request, http_response_t *response)
{
    snprintf(s_seen_body, sizeof(s_seen_body), "%s", request->body);
    response->status = 200;
    response->content_type = "text/plain";
    response->body_length = 0;
}

static void test_serving_a_request(void)
{
    char reply[512];
    s_handler = recording_handler;

    s_seen_body[0] = '\0';
    CHECK(serve_once("GET /status HTTP/1.1\r\nHost: x\r\n\r\n", reply, sizeof(reply)) > 0);
    CHECK(strncmp(reply, "HTTP/1.1 200 OK\r\n", 17) == 0);

    CHECK(serve_once("POST /join HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello", reply,
                     sizeof(reply)) > 0);
    CHECK(strncmp(reply, "HTTP/1.1 200 OK\r\n", 17) == 0);
    CHECK_EQ_STR(s_seen_body, "hello");
}

static void test_a_transfer_encoded_request_is_refused(void)
{
    char reply[512];
    s_handler = recording_handler;

    /* The framing read as content: the handler used to be given a body starting
     * "5", so a chunk size became the first characters of a credential. */
    s_seen_body[0] = '\0';
    CHECK(serve_once("POST /join HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"
                     "5\r\nhello\r\n0\r\n\r\n",
                     reply, sizeof(reply)) > 0);
    CHECK(strncmp(reply, "HTTP/1.1 501 Not Implemented\r\n", 30) == 0);
    CHECK_EQ_STR(s_seen_body, "");

    /* Both headers present is the case where believing the wrong one smuggles a
     * second request past anything in front of this device. */
    s_seen_body[0] = '\0';
    CHECK(serve_once("POST /join HTTP/1.1\r\nTransfer-Encoding: chunked\r\n"
                     "Content-Length: 5\r\n\r\nhello",
                     reply, sizeof(reply)) > 0);
    CHECK(strncmp(reply, "HTTP/1.1 501 Not Implemented\r\n", 30) == 0);
    CHECK_EQ_STR(s_seen_body, "");
}

static void test_a_malformed_request_is_refused(void)
{
    char reply[512];
    s_handler = recording_handler;

    s_seen_body[0] = '\0';
    CHECK(serve_once("nonsense\r\n\r\n", reply, sizeof(reply)) > 0);
    CHECK(strncmp(reply, "HTTP/1.1 400 ", 13) == 0);
    CHECK_EQ_STR(s_seen_body, "");

    CHECK(serve_once("POST /join HTTP/1.1\r\nContent-Length: nope\r\n\r\n", reply,
                     sizeof(reply)) > 0);
    CHECK(strncmp(reply, "HTTP/1.1 400 ", 13) == 0);
}

static void test_response_heads(void)
{
    char head[224];

    const size_t length =
        build_head(head, sizeof(head), 200, "text/html; charset=utf-8", 7u);
    CHECK(length > 0u);
    CHECK_EQ(strlen(head), length);
    CHECK(strncmp(head, "HTTP/1.1 200 OK\r\n", 17u) == 0);
    CHECK(strstr(head, "\r\nContent-Type: text/html; charset=utf-8\r\n") != NULL);
    CHECK(strstr(head, "\r\nContent-Length: 7\r\n") != NULL);
    CHECK(strstr(head, "\r\nCache-Control: no-store\r\n") != NULL);
    CHECK(strstr(head, "\r\nConnection: close\r\n") != NULL);
    if (length >= 4u) {
        /* The blank line a client needs in order to know the head has ended. */
        CHECK_EQ_STR(head + length - 4u, "\r\n\r\n");
    }

    /* No content type from the handler is a default, not an absent header: a body sent
     * without one is a body the browser gets to guess at. */
    CHECK(build_head(head, sizeof(head), 500, NULL, 0u) > 0u);
    CHECK(strstr(head, "\r\nContent-Type: text/plain; charset=utf-8\r\n") != NULL);

    /*
     * The case that used to be answered with nothing at all: a handler naming a content
     * type longer than the head it has to fit inside. Refused here, which is what lets
     * respond() send its own 500 rather than close the connection in silence.
     */
    char huge[512];
    memset(huge, 'x', sizeof(huge) - 1u);
    huge[sizeof(huge) - 1u] = '\0';
    CHECK_EQ(build_head(head, sizeof(head), 200, huge, 1u), 0u);

    /* One byte short of the head it would have written, which is the same refusal
     * arrived at from the other side. */
    CHECK_EQ(build_head(head, length, 200, "text/html; charset=utf-8", 7u), 0u);
    CHECK_EQ(build_head(head, 1u, 200, NULL, 0u), 0u);
}

void test_http_request(void)
{
    check_begin("http request and response");

    test_request_line();
    test_header_lookup();
    test_content_length();
    test_transfer_encoding_is_detected();
    test_serving_a_request();
    test_a_transfer_encoded_request_is_refused();
    test_a_malformed_request_is_refused();
    test_response_heads();
}
