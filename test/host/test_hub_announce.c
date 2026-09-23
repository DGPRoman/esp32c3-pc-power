/**
 * @file test_hub_announce.c
 * @brief What this device sends the hub, and when it decides to send it again.
 *
 * Linked rather than included: everything under test is public. The suite is split
 * the way the header is — what a value has to look like before it goes on the wire,
 * and what the retry schedule does with the answers that come back.
 *
 * The schedule is the half that cannot be observed on a bench. Its interesting cases
 * are a hub that is down for an hour, a hub that says no, and a clock that wraps
 * after seven weeks, none of which anyone is going to sit and wait for.
 */

#include "check.h"

#include "hub_announce.h"

#include <stdlib.h>
#include <string.h>

void test_hub_announce(void);

/** @brief Settings that pass, for a test that wants to vary one field at a time. */
static hub_settings_t good_settings(void)
{
    hub_settings_t settings;
    memset(&settings, 0, sizeof(settings));
    snprintf(settings.origin, sizeof(settings.origin), "http://10.0.0.2:5000");
    snprintf(settings.device_id, sizeof(settings.device_id), "workshop-pc");
    snprintf(settings.key, sizeof(settings.key), "0123456789abcdef0123456789abcdef");
    return settings;
}

/** @brief A key of exactly @p length characters, into @p out. */
static void key_of(char *out, size_t size, size_t length)
{
    CHECK(length < size);
    for (size_t at = 0; at < length; at++) {
        out[at] = 'k';
    }
    out[length] = '\0';
}

static void test_an_origin_this_device_will_post_to(void)
{
    CHECK(hub_origin_valid("http://10.0.0.2"));
    CHECK(hub_origin_valid("http://10.0.0.2:5000"));
    CHECK(hub_origin_valid("http://192.168.1.1:80"));
    CHECK(hub_origin_valid("http://255.255.255.255:65535"));

    /* No TLS on this device and nothing to check a certificate against, so accepting
     * this would be promising something it cannot do. */
    CHECK(!hub_origin_valid("https://10.0.0.2"));

    /* A name needs DNS working before this device can say where it is. */
    CHECK(!hub_origin_valid("http://hub.local"));
    CHECK(!hub_origin_valid("http://localhost"));

    /* The path is this device's to build. A stored one is a way to aim its key. */
    CHECK(!hub_origin_valid("http://10.0.0.2/v1"));
    CHECK(!hub_origin_valid("http://10.0.0.2/"));
    CHECK(!hub_origin_valid("http://10.0.0.2?x=1"));
    CHECK(!hub_origin_valid("http://10.0.0.2#x"));
    CHECK(!hub_origin_valid("http://user:pass@10.0.0.2"));

    CHECK(!hub_origin_valid(""));
    CHECK(!hub_origin_valid(NULL));
    CHECK(!hub_origin_valid("10.0.0.2"));
    CHECK(!hub_origin_valid("http://"));
    CHECK(!hub_origin_valid("http://10.0.0"));
    CHECK(!hub_origin_valid("http://10.0.0.2.3"));
    CHECK(!hub_origin_valid("http://10.0.0.256"));
    CHECK(!hub_origin_valid("http://10.0.0.2:"));
    CHECK(!hub_origin_valid("http://10.0.0.2:0"));
    CHECK(!hub_origin_valid("http://10.0.0.2:65536"));
    CHECK(!hub_origin_valid("http://10.0.0.2:http"));

    /* "010" is ten here and eight to anything reading it as octal. An address that
     * means two things is not one to build a request from. */
    CHECK(!hub_origin_valid("http://10.0.0.010"));
    CHECK(!hub_origin_valid("http://010.0.0.2"));
    CHECK(!hub_origin_valid("http://10.0.0.2:0080"));

    /* Longer than the field that stores it. */
    char oversized[HUB_ORIGIN_MAX + 8u];
    memset(oversized, 'x', sizeof(oversized) - 1u);
    oversized[sizeof(oversized) - 1u] = '\0';
    memcpy(oversized, "http://", 7u);
    CHECK(!hub_origin_valid(oversized));
}

static void test_an_id_the_hub_would_recognise(void)
{
    CHECK(hub_device_id_valid("workshop-pc"));
    CHECK(hub_device_id_valid("a"));
    CHECK(hub_device_id_valid("pc1"));
    CHECK(hub_device_id_valid("a1-b2-c3"));

    CHECK(!hub_device_id_valid(""));
    CHECK(!hub_device_id_valid(NULL));
    CHECK(!hub_device_id_valid("-pc"));
    CHECK(!hub_device_id_valid("pc-"));
    CHECK(!hub_device_id_valid("a--b"));
    CHECK(!hub_device_id_valid("Workshop"));
    CHECK(!hub_device_id_valid("work_shop"));
    CHECK(!hub_device_id_valid("work shop"));
    CHECK(!hub_device_id_valid("work/shop"));
    CHECK(!hub_device_id_valid("work.shop"));

    /* The hub's own ceiling, so one character over is a 422 rather than a 404. */
    char longest[HUB_DEVICE_ID_MAX + 2u];
    memset(longest, 'a', HUB_DEVICE_ID_MAX);
    longest[HUB_DEVICE_ID_MAX] = '\0';
    CHECK(hub_device_id_valid(longest));

    longest[HUB_DEVICE_ID_MAX] = 'a';
    longest[HUB_DEVICE_ID_MAX + 1u] = '\0';
    CHECK(!hub_device_id_valid(longest));
}

static void test_a_key_that_can_go_in_a_header(void)
{
    char key[HUB_KEY_MAX + 8u];

    key_of(key, sizeof(key), HUB_KEY_MIN);
    CHECK(hub_key_valid(key));

    key_of(key, sizeof(key), HUB_KEY_MAX);
    CHECK(hub_key_valid(key));

    key_of(key, sizeof(key), HUB_KEY_MIN - 1u);
    CHECK(!hub_key_valid(key));

    key_of(key, sizeof(key), HUB_KEY_MAX + 1u);
    CHECK(!hub_key_valid(key));

    CHECK(!hub_key_valid(""));
    CHECK(!hub_key_valid(NULL));

    /* The reason this checks characters at all. A CR or an LF in a header value is
     * not a strange character: it ends the header and starts another one. */
    key_of(key, sizeof(key), HUB_KEY_MIN);
    key[4] = '\r';
    CHECK(!hub_key_valid(key));

    key_of(key, sizeof(key), HUB_KEY_MIN);
    key[4] = '\n';
    CHECK(!hub_key_valid(key));

    key_of(key, sizeof(key), HUB_KEY_MIN);
    key[4] = ' ';
    CHECK(!hub_key_valid(key));

    key_of(key, sizeof(key), HUB_KEY_MIN);
    key[4] = (char)0x7F;
    CHECK(!hub_key_valid(key));
}

static void test_settings_are_complete_only_when_every_part_is(void)
{
    hub_settings_t settings = good_settings();
    CHECK(hub_settings_complete(&settings));
    CHECK(!hub_settings_complete(NULL));

    settings = good_settings();
    settings.origin[0] = '\0';
    CHECK(!hub_settings_complete(&settings));

    settings = good_settings();
    settings.device_id[0] = '\0';
    CHECK(!hub_settings_complete(&settings));

    settings = good_settings();
    settings.key[0] = '\0';
    CHECK(!hub_settings_complete(&settings));
}

static void test_the_address_this_device_announces(void)
{
    char address[HUB_ADDRESS_MAX + 1u];

    CHECK(hub_announce_address(address, sizeof(address), "10.0.0.5") > 0);
    CHECK_EQ_STR(address, "http://10.0.0.5");

    /* The longest one there is still fits the field it is stored in. */
    CHECK(hub_announce_address(address, sizeof(address), "255.255.255.255") > 0);
    CHECK_EQ_STR(address, "http://255.255.255.255");

    CHECK_EQ(hub_announce_address(address, sizeof(address), ""), -1);
    CHECK_EQ_STR(address, "");
    CHECK_EQ(hub_announce_address(address, sizeof(address), NULL), -1);
    CHECK_EQ(hub_announce_address(address, sizeof(address), "10.0.0"), -1);
    CHECK_EQ(hub_announce_address(address, sizeof(address), "10.0.0.256"), -1);
    CHECK_EQ(hub_announce_address(address, sizeof(address), "not-an-address"), -1);

    /* A buffer one short of the answer leaves nothing behind, rather than a prefix
     * that looks like an address. */
    char cramped[8];
    CHECK_EQ(hub_announce_address(cramped, sizeof(cramped), "10.0.0.5"), -1);
    CHECK_EQ_STR(cramped, "");
}

static void test_the_url_the_announcement_goes_to(void)
{
    char url[128];
    hub_settings_t settings = good_settings();

    CHECK(hub_announce_url(url, sizeof(url), &settings) > 0);
    CHECK_EQ_STR(url, "http://10.0.0.2:5000/v1/devices/workshop-pc/announcements");

    settings.device_id[0] = '\0';
    CHECK_EQ(hub_announce_url(url, sizeof(url), &settings), -1);
    CHECK_EQ_STR(url, "");

    CHECK_EQ(hub_announce_url(url, sizeof(url), NULL), -1);

    settings = good_settings();
    char cramped[16];
    CHECK_EQ(hub_announce_url(cramped, sizeof(cramped), &settings), -1);
    CHECK_EQ_STR(cramped, "");
}

static void test_the_body_the_hub_is_sent(void)
{
    char body[256];
    const char *const key = "0123456789abcdef0123456789abcdef";

    CHECK(hub_announce_body(body, sizeof(body), "http://10.0.0.5", key, "0.3.0") > 0);
    CHECK_EQ_STR(body, "{\"address\":\"http://10.0.0.5\",\"api_key\":\""
                       "0123456789abcdef0123456789abcdef\",\"firmware\":\"0.3.0\"}");

    /* Optional to the hub, so absent rather than empty when there is nothing to say. */
    CHECK(hub_announce_body(body, sizeof(body), "http://10.0.0.5", key, NULL) > 0);
    CHECK_EQ_STR(body, "{\"address\":\"http://10.0.0.5\",\"api_key\":\""
                       "0123456789abcdef0123456789abcdef\"}");
    CHECK(hub_announce_body(body, sizeof(body), "http://10.0.0.5", key, "") > 0);
    CHECK(strstr(body, "firmware") == NULL);

    /* A version string that would need escaping is dropped, not escaped and not
     * fatal: the address and the key are what the hub actually needs. */
    CHECK(hub_announce_body(body, sizeof(body), "http://10.0.0.5", key, "0.3\"0") > 0);
    CHECK(strstr(body, "firmware") == NULL);
    CHECK(strstr(body, "10.0.0.5") != NULL);

    CHECK(hub_announce_body(body, sizeof(body), "http://10.0.0.5", key, "0.3\\0") > 0);
    CHECK(strstr(body, "firmware") == NULL);

    CHECK(hub_announce_body(body, sizeof(body), "http://10.0.0.5", key, "0.3\n0") > 0);
    CHECK(strstr(body, "firmware") == NULL);

    /* The two the hub cannot do without are refused rather than dropped. */
    CHECK_EQ(hub_announce_body(body, sizeof(body), "", key, "0.3.0"), -1);
    CHECK_EQ(hub_announce_body(body, sizeof(body), NULL, key, "0.3.0"), -1);
    CHECK_EQ(hub_announce_body(body, sizeof(body), "http://10.0.0.5", "", "0.3.0"), -1);
    CHECK_EQ(hub_announce_body(body, sizeof(body), "http://10.0.0.5", NULL, "0.3.0"), -1);
    CHECK_EQ(hub_announce_body(body, sizeof(body), "http://10.0.0.5", "a\"b", "0.3.0"), -1);

    char cramped[16];
    CHECK_EQ(hub_announce_body(cramped, sizeof(cramped), "http://10.0.0.5", key, NULL), -1);
    CHECK_EQ_STR(cramped, "");
}

static void test_an_origin_splits_into_somewhere_to_connect(void)
{
    char host[HUB_ORIGIN_MAX + 1u];
    uint16_t port = 0;

    CHECK(hub_origin_split("http://10.0.0.2:5000", host, sizeof(host), &port));
    CHECK_EQ_STR(host, "10.0.0.2");
    CHECK_EQ(port, 5000);

    /* No port is 80 — the scheme's default, and the one the hub treats as the same
     * address as an explicit :80. */
    CHECK(hub_origin_split("http://10.0.0.2", host, sizeof(host), &port));
    CHECK_EQ_STR(host, "10.0.0.2");
    CHECK_EQ(port, 80);

    CHECK(hub_origin_split("http://255.255.255.255:65535", host, sizeof(host), &port));
    CHECK_EQ_STR(host, "255.255.255.255");
    CHECK_EQ(port, 65535);

    /* Anything the validator refuses never reaches a socket. */
    CHECK(!hub_origin_split("https://10.0.0.2", host, sizeof(host), &port));
    CHECK(!hub_origin_split("http://hub.local", host, sizeof(host), &port));
    CHECK(!hub_origin_split("", host, sizeof(host), &port));
    CHECK(!hub_origin_split(NULL, host, sizeof(host), &port));
    CHECK_EQ_STR(host, "");

    CHECK(!hub_origin_split("http://10.0.0.2", NULL, sizeof(host), &port));
    CHECK(!hub_origin_split("http://10.0.0.2", host, sizeof(host), NULL));

    char cramped[4];
    CHECK(!hub_origin_split("http://10.0.0.2:5000", cramped, sizeof(cramped), &port));
    CHECK_EQ_STR(cramped, "");
}

static void test_the_request_that_goes_on_the_wire(void)
{
    char request[HUB_REQUEST_MAX];
    hub_settings_t settings = good_settings();
    const char *const own_key = "abcdefghijklmnopqrstuvwxyz012345";

    const int written = hub_announce_request(request, sizeof(request), &settings,
                                             "http://10.0.0.5", own_key, "0.3.0");
    CHECK(written > 0);
    CHECK_EQ((size_t)written, strlen(request));

    /* The head, exactly. The request target is built from the id and never taken from
     * anywhere else, and Host carries the port the origin named. */
    CHECK(strncmp(request,
                  "POST /v1/devices/workshop-pc/announcements HTTP/1.1\r\n"
                  "Host: 10.0.0.2:5000\r\n"
                  "X-API-Key: 0123456789abcdef0123456789abcdef\r\n"
                  "Content-Type: application/json\r\n",
                  118u) == 0);

    /* One request per connection, minutes apart, and nothing reads the reply body. */
    CHECK(strstr(request, "\r\nConnection: close\r\n") != NULL);

    /* The hub's key is in the header and this device's own key is in the body. Sending
     * the wrong one of the two would authenticate fine and hand the hub a credential
     * that does not open this device. */
    CHECK(strstr(request, "X-API-Key: 0123456789abcdef0123456789abcdef\r\n") != NULL);
    CHECK(strstr(request, "\"api_key\":\"abcdefghijklmnopqrstuvwxyz012345\"") != NULL);

    /* Content-Length against the body that is actually there, rather than against a
     * number written into this test. A body and a length that disagree is a request
     * that hangs or is truncated, and neither says so. */
    const char *const body = strstr(request, "\r\n\r\n");
    CHECK(body != NULL);
    const char *const declared = strstr(request, "Content-Length: ");
    CHECK(declared != NULL);
    if (body != NULL && declared != NULL) {
        CHECK_EQ(atoi(declared + strlen("Content-Length: ")), (int)strlen(body + 4));
    }

    /* Settings that would not be announced to are not built into a request either. */
    settings.key[0] = '\0';
    CHECK_EQ(hub_announce_request(request, sizeof(request), &settings, "http://10.0.0.5", own_key,
                                  "0.3.0"),
             -1);
    CHECK_EQ_STR(request, "");

    settings = good_settings();
    CHECK_EQ(hub_announce_request(request, sizeof(request), &settings, "", own_key, "0.3.0"), -1);
    CHECK_EQ(hub_announce_request(request, sizeof(request), &settings, "http://10.0.0.5", "",
                                  "0.3.0"),
             -1);
    CHECK_EQ(hub_announce_request(request, sizeof(request), NULL, "http://10.0.0.5", own_key,
                                  "0.3.0"),
             -1);

    /* A buffer too small leaves nothing, rather than half a request with a key in it. */
    char cramped[40];
    CHECK_EQ(hub_announce_request(cramped, sizeof(cramped), &settings, "http://10.0.0.5", own_key,
                                  "0.3.0"),
             -1);
    CHECK_EQ_STR(cramped, "");
}

static void test_the_status_line_the_hub_sends_back(void)
{
    const char accepted[] = "HTTP/1.1 204 No Content\r\nDate: x\r\n\r\n";
    CHECK_EQ(hub_announce_status(accepted, sizeof(accepted) - 1u), 204);

    const char refused[] = "HTTP/1.1 401 Unauthorized\r\n";
    CHECK_EQ(hub_announce_status(refused, sizeof(refused) - 1u), 401);

    const char old_version[] = "HTTP/1.0 500 Internal Server Error\r\n";
    CHECK_EQ(hub_announce_status(old_version, sizeof(old_version) - 1u), 500);

    /* A status line with nothing after the code is still a status line. */
    const char bare[] = "HTTP/1.1 204";
    CHECK_EQ(hub_announce_status(bare, sizeof(bare) - 1u), 204);

    const char no_reason[] = "HTTP/1.1 204\r\n";
    CHECK_EQ(hub_announce_status(no_reason, sizeof(no_reason) - 1u), 204);

    /*
     * Something is listening on that address and it is not the hub. Guessing a
     * meaning out of it would be worse than reporting that nothing usable came back:
     * the addresses here are typed by a person, and the one they typed by mistake
     * belongs to something.
     */
    const char not_http[] = "<!doctype html><html>";
    CHECK_EQ(hub_announce_status(not_http, sizeof(not_http) - 1u), -1);

    const char ssh[] = "SSH-2.0-OpenSSH_9.6\r\n";
    CHECK_EQ(hub_announce_status(ssh, sizeof(ssh) - 1u), -1);

    /*
     * The same shape, a different protocol. RTSP answers "RTSP/1.0 200 OK", which
     * satisfies every check here except the one that names the protocol: a digit
     * where the minor version goes, a space, then three digits. A device that read
     * 200 out of that would record an announcement that never happened, and then
     * stop announcing — so the protocol name is load-bearing rather than decorative.
     */
    const char rtsp[] = "RTSP/1.0 200 OK\r\n";
    CHECK_EQ(hub_announce_status(rtsp, sizeof(rtsp) - 1u), -1);

    /* Four digits are not a status code, and taking the first three would turn one
     * into 204. */
    const char too_many_digits[] = "HTTP/1.1 2044\r\n";
    CHECK_EQ(hub_announce_status(too_many_digits, sizeof(too_many_digits) - 1u), -1);

    const char letters[] = "HTTP/1.1 20X\r\n";
    CHECK_EQ(hub_announce_status(letters, sizeof(letters) - 1u), -1);

    const char no_space[] = "HTTP/1.1-204\r\n";
    CHECK_EQ(hub_announce_status(no_space, sizeof(no_space) - 1u), -1);

    /* Shorter than a status line can be. Read past the end and this is where it
     * would show, which is why the suite runs under ASan. */
    const char truncated[] = "HTTP/1.1 20";
    CHECK_EQ(hub_announce_status(truncated, sizeof(truncated) - 1u), -1);
    CHECK_EQ(hub_announce_status("", 0), -1);
    CHECK_EQ(hub_announce_status(NULL, 12u), -1);
}

static void test_nothing_is_announced_without_an_address(void)
{
    hub_announce_state_t state;
    hub_announce_reset(&state);

    CHECK(!hub_announce_due(&state, "", 0));
    CHECK(!hub_announce_due(&state, NULL, 0));
    CHECK(!hub_announce_due(NULL, "http://10.0.0.5", 0));
}

static void test_a_boot_announces_and_an_acceptance_settles(void)
{
    hub_announce_state_t state;
    hub_announce_reset(&state);

    /* Nothing is known about what the hub remembers, so it is assumed to remember
     * nothing. Re-announcing an unchanged address is harmless; not announcing after
     * a reboot the hub did not see is a device the hub never polls. */
    CHECK(hub_announce_due(&state, "http://10.0.0.5", 0));

    hub_announce_record(&state, "http://10.0.0.5", HUB_RESULT_ACCEPTED, 0);
    CHECK(!hub_announce_due(&state, "http://10.0.0.5", 0));

    /*
     * And still not, an hour later. This is the test that stands in for the design
     * decision: an announcement clears everything the hub's poller recorded, so a
     * device announcing on a timer would wipe the hub's record of a fault on every
     * tick — including how long it had been unreachable, which is the one field
     * somebody diagnosing it would be reading.
     */
    CHECK(!hub_announce_due(&state, "http://10.0.0.5", 3600u * 1000u));
}

static void test_a_new_address_does_not_wait_out_an_old_backoff(void)
{
    hub_announce_state_t state;
    hub_announce_reset(&state);

    hub_announce_record(&state, "http://10.0.0.5", HUB_RESULT_UNREACHABLE, 0);
    CHECK(!hub_announce_due(&state, "http://10.0.0.5", 1000u));

    /* DHCP moved this device. The wait was earned by an address nobody is asking
     * about any more, and the hub is meanwhile polling somebody else. */
    CHECK(hub_announce_due(&state, "http://10.0.0.9", 1000u));
}

static void test_an_address_that_moves_back_is_announced_again(void)
{
    hub_announce_state_t state;
    hub_announce_reset(&state);

    hub_announce_record(&state, "http://10.0.0.5", HUB_RESULT_ACCEPTED, 0);
    hub_announce_record(&state, "http://10.0.0.9", HUB_RESULT_ACCEPTED, 1000u);
    CHECK(!hub_announce_due(&state, "http://10.0.0.9", 2000u));

    /* Back to the first one. The hub was told about the second, so it has to be
     * told about this, even though this device announced it once before. */
    CHECK(hub_announce_due(&state, "http://10.0.0.5", 2000u));
}

static void test_an_unreachable_hub_is_retried_with_a_growing_wait(void)
{
    hub_announce_state_t state;
    hub_announce_reset(&state);

    const char *const address = "http://10.0.0.5";
    uint32_t now = 0;
    uint32_t expected = HUB_ANNOUNCE_FIRST_RETRY_MS;

    for (unsigned attempt = 0; attempt < 12u; attempt++) {
        hub_announce_record(&state, address, HUB_RESULT_UNREACHABLE, now);
        CHECK_EQ(state.backoff_ms, expected);

        /* One millisecond short of the wait is still waiting; the wait itself is not. */
        CHECK(!hub_announce_due(&state, address, now + expected - 1u));
        CHECK(hub_announce_due(&state, address, now + expected));

        now += expected;
        expected = (expected >= HUB_ANNOUNCE_MAX_RETRY_MS / 2u) ? HUB_ANNOUNCE_MAX_RETRY_MS
                                                                : expected * 2u;
    }

    /* It stops growing rather than running away to hours. */
    CHECK_EQ(state.backoff_ms, HUB_ANNOUNCE_MAX_RETRY_MS);

    /* And an acceptance puts it back, so the next outage starts from seconds. */
    hub_announce_record(&state, address, HUB_RESULT_ACCEPTED, now);
    CHECK_EQ(state.backoff_ms, 0u);
    hub_announce_record(&state, "http://10.0.0.9", HUB_RESULT_UNREACHABLE, now);
    CHECK_EQ(state.backoff_ms, HUB_ANNOUNCE_FIRST_RETRY_MS);
}

static void test_a_refusal_waits_the_longest_wait_at_once(void)
{
    hub_announce_state_t state;
    hub_announce_reset(&state);

    const char *const address = "http://10.0.0.5";

    /*
     * No gradual backoff into it. The hub counts failed authentication attempts per
     * source address and answers 429 once there have been enough, so a device
     * retrying a rejected key every two seconds locks itself out of the route it
     * needs the moment somebody fixes the key. A 404 is a person who has not
     * declared this device yet, which is not a thing that resolves in seconds either.
     */
    hub_announce_record(&state, address, HUB_RESULT_REFUSED, 0);
    CHECK_EQ(state.backoff_ms, HUB_ANNOUNCE_REFUSED_MS);
    CHECK(!hub_announce_due(&state, address, HUB_ANNOUNCE_REFUSED_MS - 1u));
    CHECK(hub_announce_due(&state, address, HUB_ANNOUNCE_REFUSED_MS));

    /* It does keep trying. The key on the hub is something somebody can go and fix,
     * and a device that gave up would need a reboot to notice they had. */
    hub_announce_record(&state, address, HUB_RESULT_ACCEPTED, HUB_ANNOUNCE_REFUSED_MS);
    CHECK(!hub_announce_due(&state, address, HUB_ANNOUNCE_REFUSED_MS));
}

static void test_the_schedule_survives_the_clock_wrapping(void)
{
    hub_announce_state_t state;
    hub_announce_reset(&state);

    const char *const address = "http://10.0.0.5";

    /*
     * The millisecond clock is 32 bits and wraps after 49.7 days, which is an
     * ordinary uptime for a board that lives inside a closed case. A deadline set
     * just before the wrap lands just after it, and a comparison that did not
     * account for that would either fire every wait at once or stop firing any of
     * them until the next reboot.
     */
    const uint32_t before_wrap = 0xFFFFFFF0u;
    hub_announce_record(&state, address, HUB_RESULT_UNREACHABLE, before_wrap);

    /* The deadline is 2000 ms later, which is 1984 on the other side of zero. */
    CHECK_EQ(state.next_attempt_ms, 1984u);

    CHECK(!hub_announce_due(&state, address, before_wrap));
    CHECK(!hub_announce_due(&state, address, 0xFFFFFFFFu));
    CHECK(!hub_announce_due(&state, address, 1983u));
    CHECK(hub_announce_due(&state, address, 1984u));
    CHECK(hub_announce_due(&state, address, 5000u));
}

void test_hub_announce(void)
{
    check_begin("hub_announce");

    test_an_origin_this_device_will_post_to();
    test_an_id_the_hub_would_recognise();
    test_a_key_that_can_go_in_a_header();
    test_settings_are_complete_only_when_every_part_is();
    test_the_address_this_device_announces();
    test_the_url_the_announcement_goes_to();
    test_the_body_the_hub_is_sent();
    test_an_origin_splits_into_somewhere_to_connect();
    test_the_request_that_goes_on_the_wire();
    test_the_status_line_the_hub_sends_back();
    test_nothing_is_announced_without_an_address();
    test_a_boot_announces_and_an_acceptance_settles();
    test_a_new_address_does_not_wait_out_an_old_backoff();
    test_an_address_that_moves_back_is_announced_again();
    test_an_unreachable_hub_is_retried_with_a_growing_wait();
    test_a_refusal_waits_the_longest_wait_at_once();
    test_the_schedule_survives_the_clock_wrapping();
}
