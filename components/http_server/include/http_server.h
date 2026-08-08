/**
 * @file http_server.h
 * @brief A small HTTP/1.1 server, written directly on BSD sockets.
 *
 * Scoped to what this device serves: a provisioning page while it is in setup mode,
 * and authenticated commands from the hub once it is on a network. That is a handful
 * of requests over the device's lifetime, which is why this handles one connection at
 * a time and closes it afterwards rather than keeping any alive.
 *
 * The narrow scope is the point. A general-purpose server has to survive arbitrary
 * clients; this one has to survive a phone browser and a Python client, and the code
 * that would handle everything else is code that could go wrong instead.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Largest request head — request line and headers — that will be accepted.
 *
 * A browser's GET runs to roughly 600 bytes of headers, so this is generous for what
 * it has to serve while still being a hard ceiling. A request whose head does not fit
 * is answered and dropped, never accumulated: the buffer is a fixed cost, not
 * something a client gets to grow.
 */
#define HTTP_REQUEST_HEAD_MAX 2048u

/** @brief Largest request body accepted. A provisioning form is a few hundred bytes. */
#define HTTP_REQUEST_BODY_MAX 512u

/**
 * @brief Capacity of the buffer a handler writes its response into.
 *
 * Sized for the provisioning page with a full list of nearby networks rendered into
 * it, the largest thing this device serves — comfortably past that rather than exactly
 * at it, because the margin is cheap and a handler that comes up five bytes short only
 * finds out from a 500 on real hardware.
 */
#define HTTP_RESPONSE_BODY_MAX 8192u

/** @brief The parts of a request a handler is given. */
typedef struct {
    /** Request method, uppercase as it arrived: "GET", "POST". */
    const char *method;
    /** Request target, with any query string still attached. */
    const char *target;
    /**
     * Raw header block: everything between the request line and the blank line that
     * ends it, CRLF-delimited exactly as it arrived. Not meant to be scanned by hand —
     * see ::http_request_header.
     */
    const char *headers;
    /** Request body, NUL-terminated, or an empty string when there was none. */
    const char *body;
    /** Body length in bytes, excluding the terminator. */
    size_t body_length;
} http_request_t;

/**
 * @brief Find header @p name among @p headers.
 *
 * A lookup rather than a parsed table: this server has at most a couple of headers any
 * handler will ever ask for, and a name/value table for that is more code than the
 * handful of call sites it would serve.
 *
 * @param headers      A request's ::http_request_t::headers.
 * @param name         Header name, matched case-insensitively, as HTTP requires.
 * @param value_length Receives the value's length. The value is not NUL-terminated at
 *                      that point — whatever follows in the request comes right after
 *                      it, usually '\r' — so a caller comparing it must use the length
 *                      rather than strlen().
 * @return Pointer to the value's first non-whitespace byte, or NULL if @p name is not
 *         present.
 */
const char *http_request_header(const char *headers, const char *name, size_t *value_length);

/**
 * @brief What a handler fills in.
 *
 * The body buffer belongs to the server and lives as long as the connection, so a
 * handler can generate a page into it without allocating. Leaving @ref status at zero
 * means the handler declined to answer, and the server sends 500 — silence is a bug,
 * not an empty response.
 */
typedef struct {
    /** HTTP status code to send. */
    int status;
    /** Value for the Content-Type header. Defaults to text/plain when left NULL. */
    const char *content_type;
    /** Buffer to write the response body into. */
    char *body;
    /** Bytes available in @ref body. */
    size_t body_capacity;
    /** Bytes the handler wrote. */
    size_t body_length;
} http_response_t;

/** @brief Answers one request. Runs on the server's task, so it must not block long. */
typedef void (*http_handler_t)(const http_request_t *request, http_response_t *response);

/**
 * @brief Start listening on @p port and dispatch requests to @p handler.
 *
 * Creates one task, which owns the listening socket for the life of the process. There
 * is no stop function because nothing needs one: the server is wanted for as long as
 * the device is running, and an unused shutdown path is a path that has never been
 * tested.
 */
esp_err_t http_server_start(uint16_t port, http_handler_t handler);

#ifdef __cplusplus
}
#endif
