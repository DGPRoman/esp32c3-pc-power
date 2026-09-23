/**
 * @file hub_announce.h
 * @brief When to tell the hub where this device is, and what to send it.
 *
 * The whole of the decision-making, with no network in it: it is handed the address
 * this device currently has and the current time, and it answers whether an
 * announcement is owed. Opening a socket is ::hub_link's job.
 *
 * Split the same way ::power_machine is, and for the same reason. A retry schedule
 * is the part worth testing and the part a host can run; the thing it drives needs a
 * hub on the other end of a real network to exercise at all, which in practice means
 * it would be exercised once.
 *
 * ### The contract this is written against
 *
 * The hub's, not this device's — `docs/devices.md` in pihome-hub. Three parts of it
 * shape everything below.
 *
 * **Which devices exist is declared on the hub.** This device announces only *where*
 * it is and *what key to ask it with*. It cannot introduce itself to a hub that has
 * not been told it exists, and is answered `404` if it tries. So a rejection is
 * somebody's configuration, not weather, and is not worth retrying at the same rate.
 *
 * **An announcement clears everything the hub's poller had recorded** — the last
 * reading, the last error, and how long this device has been unreachable. That is
 * the right thing for a boot or a move, both of which make the old record a
 * statement about a situation that no longer holds. It is also why nothing here
 * announces on a timer: a periodic re-announcement would reset the hub's record of a
 * fault every time it fired, and that record is exactly what somebody diagnosing the
 * fault would be reading. An announcement is an event, and this sends one when the
 * event happens.
 *
 * **Re-announcing an unchanged address is harmless.** Which is what makes "the hub
 * has this address already" a safe thing to be wrong about in the cautious direction
 * after a reboot: this device cannot know what the hub still remembers, so it starts
 * from the assumption that the hub knows nothing.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Longest `http://<ipv4>[:port]` this accepts, in characters.
 *
 * `http://255.255.255.255:65535` is 28. The room above that is for nothing in
 * particular — it is a stored string on a device with flash to spare, and a limit
 * with no slack is one that has to be revised the first time it is slightly wrong.
 */
#define HUB_ORIGIN_MAX 47u

/** @brief Longest device id the hub accepts. Matches its own `id` field. */
#define HUB_DEVICE_ID_MAX 64u

/**
 * @brief Shortest and longest hub key accepted.
 *
 * The floor is the hub's own: it refuses to start with an API key under 32
 * characters, so a shorter one here could only ever be a typo. The ceiling is this
 * device's, and is well past any key the hub generates.
 */
#define HUB_KEY_MIN 32u
#define HUB_KEY_MAX 128u

/** @brief Longest `http://<ipv4>` this device announces itself as. */
#define HUB_ADDRESS_MAX 22u

/** @brief What came of one attempt to announce. */
typedef enum {
    /** The hub recorded it: `204`. */
    HUB_RESULT_ACCEPTED = 0,
    /**
     * @brief The hub answered, and the answer was no.
     *
     * `401` (wrong key), `404` (this device is not declared), `422` (it will not
     * accept the address or the key) and `429` (too many failed attempts already).
     * None of these is transient, and one of them is made worse by hurrying — see
     * ::HUB_ANNOUNCE_REFUSED_MS.
     */
    HUB_RESULT_REFUSED,
    /**
     * @brief Nothing usable came back.
     *
     * No route, a refused connection, a timeout, or a `5xx`. The hub is rebooting,
     * or is not up yet, or the network is not there — all of which end by
     * themselves, and all of which are worth trying again shortly.
     */
    HUB_RESULT_UNREACHABLE,
} hub_result_t;

/** @brief How long to wait after the first failure to reach the hub. */
#define HUB_ANNOUNCE_FIRST_RETRY_MS 2000u

/** @brief The longest the backoff grows to: five minutes. */
#define HUB_ANNOUNCE_MAX_RETRY_MS 300000u

/**
 * @brief How long to wait after the hub refuses, which is the maximum at once.
 *
 * No backing off into it gradually, because retrying a refusal quickly is not merely
 * useless — it is harmful twice over. The hub counts failed authentication attempts
 * per source address and answers `429` once there have been too many, so a device
 * hammering a rejected key locks itself out of the very route it needs the moment
 * somebody fixes the key. And a `404` means nobody has declared this device yet,
 * which is a person at a keyboard, not something that resolves in two seconds.
 */
#define HUB_ANNOUNCE_REFUSED_MS HUB_ANNOUNCE_MAX_RETRY_MS

/** @brief Where the hub is, who this device is to it, and the key it admits. */
typedef struct {
    /** @brief `http://<ipv4>[:port]`, or empty when the owner has set none. */
    char origin[HUB_ORIGIN_MAX + 1u];
    /** @brief The id this device is declared under on the hub. */
    char device_id[HUB_DEVICE_ID_MAX + 1u];
    /** @brief The hub's device key, sent in `X-API-Key`. Not this device's own key. */
    char key[HUB_KEY_MAX + 1u];
} hub_settings_t;

/**
 * @brief Whether @p origin is one this device will send an announcement to.
 *
 * `http://<ipv4>` with an optional port, and nothing else. Narrow on purpose:
 *
 * - **`http` only.** This device terminates no TLS and would have nothing to check a
 *   certificate against, so accepting `https` would promise a guarantee it cannot
 *   keep. The hub refuses `https` from devices for the same reason.
 * - **An address, never a name.** Resolving one needs DNS to be working before this
 *   device can say where it is, which adds a way for announcing to fail that has
 *   nothing to do with the hub. The hub refuses names in the other direction on the
 *   same grounds.
 * - **Origin only.** No path, query, fragment or `user:password@`. The path is this
 *   device's to build, and a stored one would be a way to aim its key elsewhere.
 */
bool hub_origin_valid(const char *origin);

/** @brief Whether @p id matches the hub's own `^[a-z0-9]+(-[a-z0-9]+)*$`, within 64. */
bool hub_device_id_valid(const char *id);

/**
 * @brief Whether @p key is one this device will put in a header.
 *
 * Length, and then printable ASCII with no space. The character check is not
 * fussiness about key formats: this value is written into an `X-API-Key` header, and
 * a carriage return or newline in it would end that header and start another one of
 * whoever supplied it choosing. A key is rejected here rather than escaped, because
 * there is no such thing as a key that legitimately contains a newline.
 */
bool hub_key_valid(const char *key);

/** @brief Whether every field is present and valid, so announcing can be attempted. */
bool hub_settings_complete(const hub_settings_t *settings);

/**
 * @brief Write `http://<ip>` into @p out, the way this device announces itself.
 *
 * @p ip is checked for the shape of a dotted quad, because this builds the string
 * and a malformed one would be caught by the hub as a `422` five minutes later
 * instead of in a log line now. Whether the address is a *private* one is left to
 * the hub, which has a rule about it: duplicating the rule here would give it two
 * places to be changed and one of them would be missed.
 *
 * @return Characters written, or -1 if @p ip is not a dotted quad or @p out is too
 *         small.
 */
int hub_announce_address(char *out, size_t size, const char *ip);

/**
 * @brief Write the announcement URL for @p settings into @p out.
 *
 * @return Characters written, or -1 if the settings are incomplete or @p out is too
 *         small.
 */
int hub_announce_url(char *out, size_t size, const hub_settings_t *settings);

/**
 * @brief Split a validated @p origin into the host and port to connect to.
 *
 * Only meaningful for an origin ::hub_origin_valid accepts. An absent port is 80,
 * which is the scheme's default and the one the hub's own address normalisation
 * treats as the same address written two ways.
 *
 * @return false if @p origin is not one this device will post to.
 */
bool hub_origin_split(const char *origin, char *host, size_t host_size, uint16_t *port);

/**
 * @brief Write the announcement body into @p out.
 *
 * @p firmware is optional to the hub and treated as optional here: a version string
 * carrying anything that would have to be escaped is dropped rather than allowed to
 * fail the announcement, because the address and the key are what the hub needs and
 * the version is what it records without interpreting.
 *
 * @return Characters written, or -1 if the address or key is unusable, or @p out is
 *         too small.
 */
int hub_announce_body(char *out, size_t size, const char *address, const char *api_key,
                      const char *firmware);

/**
 * @brief Longest request this builds, in bytes.
 *
 * The request line and six headers, the longest of which carries a key, plus a body
 * of an address and a key. Everything in it is bounded by a constant above, so this
 * is the sum of them rather than a guess — and ::hub_announce_request reports a
 * truncation rather than sending a half-written request either way.
 */
#define HUB_REQUEST_MAX 768u

/**
 * @brief Write the whole HTTP request — head and body — into @p out.
 *
 * Built here rather than by an HTTP client library, for the reason ::http_server is
 * written on sockets rather than on one: what a general-purpose client brings with it
 * is TLS, redirects, chunked bodies, proxies and authentication schemes, and on this
 * target it brings them as 145 KB of binary. This device announces over plain HTTP to
 * one origin it has been given, which is one request with a known shape.
 *
 * `Connection: close` because there is exactly one request per connection, minutes or
 * hours apart. Nothing here reads the body of the reply, so the status line is the
 * whole answer and the connection has no second use.
 *
 * @return Characters written, or -1 if anything is unusable or @p out is too small.
 */
int hub_announce_request(char *out, size_t size, const hub_settings_t *settings,
                         const char *address, const char *api_key, const char *firmware);

/**
 * @brief Read the status code out of the first line of @p response.
 *
 * Deliberately strict about the shape. A reply that does not begin with a status line
 * did not come from the hub — something else is listening on that address, which is
 * a thing to report as unreachable rather than to guess a meaning for.
 *
 * @return The code, or -1 if @p response does not begin with a status line.
 */
int hub_announce_status(const char *response, size_t length);

/** @brief What the schedule remembers between attempts. Zeroed is the boot state. */
typedef struct {
    /** @brief The address the hub is believed to have. Empty until one is accepted. */
    char announced[HUB_ADDRESS_MAX + 1u];
    /** @brief The address the last attempt carried, so a change can jump the queue. */
    char attempted[HUB_ADDRESS_MAX + 1u];
    /** @brief When the next attempt may be made, on the same clock as @p now_ms. */
    uint32_t next_attempt_ms;
    /** @brief How long the last wait was, doubling per consecutive failure. */
    uint32_t backoff_ms;
} hub_announce_state_t;

/** @brief Put @p state back to how it boots: nothing announced, nothing owed a wait. */
void hub_announce_reset(hub_announce_state_t *state);

/**
 * @brief Whether an announcement should be sent right now.
 *
 * True when this device has an address, the hub is not believed to have that
 * address, and any wait from a previous failure has run out. An address that differs
 * from the one the last attempt carried does not wait: it is new information, and
 * the backoff was earned by an address nobody is asking about any more.
 */
bool hub_announce_due(const hub_announce_state_t *state, const char *address, uint32_t now_ms);

/** @brief Record what came of an attempt that carried @p address. */
void hub_announce_record(hub_announce_state_t *state, const char *address, hub_result_t result,
                         uint32_t now_ms);

#ifdef __cplusplus
}
#endif
