#include "http_server.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char *TAG = "http";

/** @brief Connections the kernel queues while one is being served. */
#define LISTEN_BACKLOG 2

/**
 * @brief How long a connection may send nothing before it is dropped.
 *
 * This is the one denial of service that matters to a server handling one connection at
 * a time: a client that connects and then stays silent would otherwise hold the only
 * slot for as long as it liked, and nothing else could be served. Three seconds is far
 * more than a browser on the same access point needs, and the fix is one socket option.
 */
#define RECEIVE_TIMEOUT_MS 3000

/** @brief How long a stalled send is given before the connection is abandoned. */
#define SEND_TIMEOUT_MS 3000

/** @brief Pause before rebuilding a listening socket that failed, so failure is not a spin. */
#define RETRY_DELAY_MS 1000

/** @brief Stack for the server task. The buffers are static, so this holds only frames. */
#define TASK_STACK 4096

/**
 * @brief Task priority.
 *
 * Below the Wi-Fi driver (23) and the TCP/IP stack (18) by a wide margin. Those two
 * must never wait on this task: starving the stack that delivers our packets in order
 * to answer a request faster is a trade with no upside.
 */
#define TASK_PRIORITY 4

/** @brief Longest request target accepted, query string included. */
#define TARGET_MAX 128u

/** @brief Longest method name accepted. The longest one that matters here is "DELETE". */
#define METHOD_MAX 8u

/*
 * One server, so one set of buffers, and they are static rather than automatic on
 * purpose: four kilobytes of response buffer inside a four-kilobyte task stack is a
 * stack overflow, and overflowing into whatever is below is a fault that presents as
 * something else entirely. Static means the cost is fixed, visible at link time, and
 * cannot depend on how deep the call stack happens to be.
 */
static char s_head[HTTP_REQUEST_HEAD_MAX + 1u];
static char s_body[HTTP_REQUEST_BODY_MAX + 1u];
static char s_response[HTTP_RESPONSE_BODY_MAX];
static char s_method[METHOD_MAX + 1u];
static char s_target[TARGET_MAX + 1u];

static http_handler_t s_handler;
static uint16_t s_port;

/** @brief Reason phrase for @p status, for the status line. */
static const char *status_text(int status)
{
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Content Too Large";
    case 414: return "URI Too Long";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    default:  return "Status";
    }
}

/**
 * @brief Send all of @p length bytes, or report failure.
 *
 * A single send() may accept fewer bytes than it is offered — the socket's buffer is
 * finite and a four-kilobyte page does not fit in one call. Treating a short write as a
 * complete one truncates the response, which a browser renders as a page that half
 * loaded, with nothing in any log to say why.
 */
static bool send_all(int sock, const char *data, size_t length)
{
    size_t sent = 0;

    while (sent < length) {
        const int written = send(sock, &data[sent], length - sent, 0);
        if (written <= 0) {
            return false;
        }
        sent += (size_t)written;
    }

    return true;
}

/** @brief Send a complete response. */
static void respond(int sock, int status, const char *content_type, const char *body,
                    size_t body_length)
{
    char head[224];

    /*
     * Content-Length is always sent, so the client knows where the body ends without
     * having to wait for the connection to close. Cache-Control matters more than it
     * looks: a browser that caches this page will happily show a stale network list, or
     * a stale device state, and the user has no way to tell that is what they are
     * looking at.
     */
    const int head_length =
        snprintf(head, sizeof(head),
                 "HTTP/1.1 %d %s\r\n"
                 "Content-Type: %s\r\n"
                 "Content-Length: %u\r\n"
                 "Cache-Control: no-store\r\n"
                 "Connection: close\r\n"
                 "\r\n",
                 status, status_text(status),
                 content_type != NULL ? content_type : "text/plain; charset=utf-8",
                 (unsigned)body_length);

    if (head_length <= 0 || (size_t)head_length >= sizeof(head)) {
        ESP_LOGE(TAG, "response head did not fit");
        return;
    }

    if (!send_all(sock, head, (size_t)head_length)) {
        return;
    }
    if (body_length > 0) {
        (void)send_all(sock, body, body_length);
    }
}

/** @brief Outcome of reading a request head. */
typedef enum {
    HEAD_OK,
    HEAD_CLOSED,   /**< Peer went away, or said nothing until the timeout. */
    HEAD_TOO_LARGE, /**< Headers did not fit, and will not be accumulated. */
} head_result_t;

/**
 * @brief Read the request head, plus whatever of the body arrived with it.
 *
 * TCP carries a stream, not messages: the head can arrive in any number of pieces, and
 * the first piece can already contain part of the body. So this reads until it finds the
 * blank line that ends the head, and reports how many bytes past it are already in hand.
 *
 * @param sock            Connected socket.
 * @param head_length     Receives the head's length, blank line included.
 * @param body_in_hand    Receives the number of body bytes already read.
 */
static head_result_t receive_head(int sock, size_t *head_length, size_t *body_in_hand)
{
    size_t filled = 0;

    for (;;) {
        const int received =
            recv(sock, &s_head[filled], HTTP_REQUEST_HEAD_MAX - filled, 0);
        if (received <= 0) {
            /* Zero means the peer closed; negative means the receive timeout expired or
             * the connection broke. Neither leaves a request worth parsing. */
            return HEAD_CLOSED;
        }

        filled += (size_t)received;
        s_head[filled] = '\0';

        /*
         * Searching the whole buffer each time rather than only the new bytes. It is
         * quadratic in principle and irrelevant in practice at this size, and it gets
         * for free the case that costs everyone else a bug: a blank line split across
         * two reads.
         */
        const char *end = strstr(s_head, "\r\n\r\n");
        if (end != NULL) {
            *head_length = (size_t)(end - s_head) + 4u;
            *body_in_hand = filled - *head_length;
            return HEAD_OK;
        }

        if (filled == HTTP_REQUEST_HEAD_MAX) {
            return HEAD_TOO_LARGE;
        }
    }
}

/**
 * @brief Copy the method and target out of the request line.
 *
 * Copied rather than carved out of the buffer in place, because the headers still have
 * to be parsed afterwards and writing terminators into the middle of them would make
 * the order of these two steps load-bearing for no benefit.
 *
 * The HTTP version is read past and ignored. There is nothing this server would do
 * differently for 1.0, and it closes every connection regardless.
 */
static bool parse_request_line(void)
{
    const char *method_end = strchr(s_head, ' ');
    if (method_end == NULL) {
        return false;
    }

    const size_t method_length = (size_t)(method_end - s_head);
    if (method_length == 0 || method_length > METHOD_MAX) {
        return false;
    }

    const char *target_start = method_end + 1;
    const char *target_end = strchr(target_start, ' ');
    if (target_end == NULL) {
        return false;
    }

    const size_t target_length = (size_t)(target_end - target_start);
    if (target_length == 0 || target_length > TARGET_MAX) {
        return false;
    }

    memcpy(s_method, s_head, method_length);
    s_method[method_length] = '\0';
    memcpy(s_target, target_start, target_length);
    s_target[target_length] = '\0';

    return true;
}

const char *http_request_header(const char *headers, const char *name, size_t *value_length)
{
    const size_t name_length = strlen(name);
    const char *line = headers;

    while (line[0] != '\0' && !(line[0] == '\r' && line[1] == '\n')) {
        if (strncasecmp(line, name, name_length) == 0 && line[name_length] == ':') {
            const char *value = line + name_length + 1u;
            while (*value == ' ' || *value == '\t') {
                value++;
            }

            const char *end = strstr(value, "\r\n");
            *value_length = end != NULL ? (size_t)(end - value) : strlen(value);
            return value;
        }

        const char *next = strstr(line, "\r\n");
        if (next == NULL) {
            break;
        }
        line = next + 2;
    }

    return NULL;
}

/** @brief Result of looking for Content-Length. */
typedef enum {
    LENGTH_ABSENT,
    LENGTH_PRESENT,
    LENGTH_INVALID, /**< Unparseable, negative, or given more than once. */
} length_result_t;

/**
 * @brief Find the declared body length among the headers.
 *
 * Two Content-Length headers are rejected rather than resolved. Nothing downstream here
 * could disagree with a choice between them, but "pick one and hope" is the shape of a
 * whole family of request-smuggling bugs, and refusing costs one comparison.
 */
/**
 * @brief Whether the request declares a Transfer-Encoding.
 *
 * Any value, not only "chunked": this server implements none of them, and the one
 * it will actually meet is chunked. Without this check a chunked body was read as
 * though its framing were content — so "5\r\nhello\r\n0\r\n\r\n" reached a handler
 * as a body beginning "5", and the size prefix became part of a credential.
 *
 * RFC 9112 says a recipient that does not understand the encoding answers 501, and
 * that a message with both Transfer-Encoding and Content-Length must not be
 * forwarded — refusing outright satisfies both without this having to arbitrate.
 */
static bool declares_transfer_encoding(void)
{
    static const char NAME[] = "Transfer-Encoding:";

    const char *line = strstr(s_head, "\r\n");
    if (line == NULL) {
        return false;
    }
    line += 2;

    while (line[0] != '\0' && !(line[0] == '\r' && line[1] == '\n')) {
        if (strncasecmp(line, NAME, sizeof(NAME) - 1u) == 0) {
            return true;
        }

        const char *next = strstr(line, "\r\n");
        if (next == NULL) {
            break;
        }
        line = next + 2;
    }

    return false;
}

static length_result_t content_length(size_t *out)
{
    static const char NAME[] = "Content-Length:";

    const char *line = strstr(s_head, "\r\n");
    if (line == NULL) {
        return LENGTH_ABSENT;
    }
    line += 2;

    length_result_t result = LENGTH_ABSENT;

    while (line[0] != '\0' && !(line[0] == '\r' && line[1] == '\n')) {
        if (strncasecmp(line, NAME, sizeof(NAME) - 1u) == 0) {
            if (result != LENGTH_ABSENT) {
                return LENGTH_INVALID;
            }

            const char *value = line + sizeof(NAME) - 1u;
            while (*value == ' ' || *value == '\t') {
                value++;
            }

            char *end = NULL;
            const long parsed = strtol(value, &end, 10);
            if (end == value || parsed < 0) {
                return LENGTH_INVALID;
            }

            *out = (size_t)parsed;
            result = LENGTH_PRESENT;
        }

        const char *next = strstr(line, "\r\n");
        if (next == NULL) {
            break;
        }
        line = next + 2;
    }

    return result;
}

/**
 * @brief Read the rest of a body whose first @p in_hand bytes are already buffered.
 */
static bool receive_body(int sock, size_t head_length, size_t in_hand, size_t declared)
{
    size_t have = in_hand < declared ? in_hand : declared;
    memcpy(s_body, &s_head[head_length], have);

    while (have < declared) {
        const int received = recv(sock, &s_body[have], declared - have, 0);
        if (received <= 0) {
            return false;
        }
        have += (size_t)received;
    }

    s_body[declared] = '\0';
    return true;
}

/** @brief Read one request, hand it to the handler, and send the answer. */
static void serve_connection(int sock)
{
    size_t head_length = 0;
    size_t body_in_hand = 0;

    switch (receive_head(sock, &head_length, &body_in_hand)) {
    case HEAD_OK:
        break;
    case HEAD_TOO_LARGE:
        respond(sock, 431, NULL, "", 0);
        return;
    case HEAD_CLOSED:
        /* No reply. There is nothing to reply to, and a client that vanished is not
         * owed an error page. */
        return;
    }

    if (!parse_request_line()) {
        respond(sock, 400, NULL, "", 0);
        return;
    }

    if (declares_transfer_encoding()) {
        /* Before Content-Length is read at all: a message carrying both is precisely
         * the one where believing the wrong header smuggles a second request. */
        respond(sock, 501, NULL, "", 0);
        return;
    }

    size_t declared = 0;
    switch (content_length(&declared)) {
    case LENGTH_ABSENT:
        declared = 0;
        break;
    case LENGTH_PRESENT:
        if (declared > HTTP_REQUEST_BODY_MAX) {
            respond(sock, 413, NULL, "", 0);
            return;
        }
        break;
    case LENGTH_INVALID:
        respond(sock, 400, NULL, "", 0);
        return;
    }

    s_body[0] = '\0';
    if (declared > 0 && !receive_body(sock, head_length, body_in_hand, declared)) {
        /* The body was cut short. Answering would mean answering a request that was
         * never fully made. */
        return;
    }

    /* receive_head() only returns HEAD_OK once "\r\n\r\n" has been found in s_head, so
     * the first "\r\n" — the end of the request line — is guaranteed to exist too. */
    const char *header_start = strstr(s_head, "\r\n");
    header_start = header_start != NULL ? header_start + 2 : s_head;

    const http_request_t request = {
        .method = s_method,
        .target = s_target,
        .headers = header_start,
        .body = s_body,
        .body_length = declared,
    };

    http_response_t response = {
        .status = 0,
        .content_type = NULL,
        .body = s_response,
        .body_capacity = sizeof(s_response),
        .body_length = 0,
    };

    s_handler(&request, &response);

    if (response.status == 0) {
        ESP_LOGE(TAG, "%s %s: handler set no status", s_method, s_target);
        respond(sock, 500, NULL, "", 0);
        return;
    }

    if (response.body_length > response.body_capacity) {
        /* Past this point memory beyond the buffer has already been written, so the
         * response cannot be trusted and neither can much else. Refusing to send it and
         * saying so is the only useful thing left. */
        ESP_LOGE(TAG, "%s %s: handler wrote %u bytes into %u", s_method, s_target,
                 (unsigned)response.body_length, (unsigned)response.body_capacity);
        respond(sock, 500, NULL, "", 0);
        return;
    }

    ESP_LOGI(TAG, "%s %s -> %d", s_method, s_target, response.status);
    respond(sock, response.status, response.content_type, response.body,
            response.body_length);
}

/** @brief Apply the timeouts a connection is served under. */
static void configure_connection(int sock)
{
    const struct timeval receive_timeout = {
        .tv_sec = RECEIVE_TIMEOUT_MS / 1000,
        .tv_usec = (RECEIVE_TIMEOUT_MS % 1000) * 1000,
    };
    const struct timeval send_timeout = {
        .tv_sec = SEND_TIMEOUT_MS / 1000,
        .tv_usec = (SEND_TIMEOUT_MS % 1000) * 1000,
    };

    (void)setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout,
                     sizeof(receive_timeout));
    (void)setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));
}

/** @brief Create, bind and listen. Returns the socket, or -1. */
static int open_listener(void)
{
    const int listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener < 0) {
        ESP_LOGE(TAG, "socket: errno %d", errno);
        return -1;
    }

    /* So that rebuilding the listener does not fail while the old port is still in
     * TIME_WAIT — which is exactly the situation this recovers from. */
    const int reuse = 1;
    (void)setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    const struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(s_port),
    };

    if (bind(listener, (const struct sockaddr *)&address, sizeof(address)) != 0) {
        ESP_LOGE(TAG, "bind: errno %d", errno);
        close(listener);
        return -1;
    }

    if (listen(listener, LISTEN_BACKLOG) != 0) {
        ESP_LOGE(TAG, "listen: errno %d", errno);
        close(listener);
        return -1;
    }

    return listener;
}

static void server_task(void *arg)
{
    (void)arg;

    /*
     * The outer loop exists because the interface underneath this socket does not last
     * forever. Switching out of setup mode tears down the access point, and every socket
     * bound to it goes with it — accept() then fails and keeps failing. Rebuilding the
     * listener is the correct response to that, and it is not an edge case here: it is
     * what happens every time the device is provisioned.
     */
    for (;;) {
        const int listener = open_listener();
        if (listener < 0) {
            vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
            continue;
        }

        ESP_LOGI(TAG, "listening on port %u", (unsigned)s_port);

        for (;;) {
            struct sockaddr_in peer;
            socklen_t peer_length = sizeof(peer);

            const int sock = accept(listener, (struct sockaddr *)&peer, &peer_length);
            if (sock < 0) {
                ESP_LOGW(TAG, "accept: errno %d, rebuilding listener", errno);
                break;
            }

            configure_connection(sock);
            serve_connection(sock);

            /* Shut down before closing, so the client sees an orderly end of stream
             * rather than a reset. A browser shown a reset reports a failed page even
             * when every byte of it arrived. */
            (void)shutdown(sock, SHUT_RDWR);
            (void)close(sock);
        }

        close(listener);
        vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
    }
}

esp_err_t http_server_start(uint16_t port, http_handler_t handler)
{
    if (handler == NULL || port == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_handler != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    s_port = port;
    s_handler = handler;

    if (xTaskCreate(server_task, "http", TASK_STACK, NULL, TASK_PRIORITY, NULL) != pdPASS) {
        s_handler = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
