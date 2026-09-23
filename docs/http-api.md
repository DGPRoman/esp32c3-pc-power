# HTTP contract

What this device serves, and what a client may rely on. Everything here is
implemented in [`main/main.c`](../main/main.c) and
[`components/http_server/`](../components/http_server/); this file is the
statement of intent that outlives either.

There are two surfaces and they carry different promises.

**`/v1/…` is the contract.** It is what a hub is written against, and the
compatibility rule at the end of this file applies to all of it.

**`/` and `/network` are the provisioning surface.** A person with a phone
joining the device to a network for the first time, and nothing else. They are
HTML, they are not versioned, and they may change shape in any release. Nothing
should be automated against them.

## Transport

HTTP/1.1 on port 80, over plain TCP. No TLS: a device inside a PC case has no
way to obtain a certificate anyone would check, and terminating TLS somewhere it
can be done properly — a reverse proxy, a VPN — is a better answer than a
self-signed one here. The key travels in a header, so the network this is on has
to be one you trust with it.

One request per connection. The response carries `Connection: close` and the
socket is closed after it; there is no keep-alive and no pipelining.

Requests must use CRLF line endings, as HTTP requires. A request whose lines end
in a bare LF is never recognised as complete: it is not answered at all, and the
connection closes when the three-second receive timeout expires. This is exactly
the request that comes of typing one into `netcat` by hand.

The request line is read as one line and nothing beyond it. The method is upper-case
letters — this device answers `GET` and `POST`, and a method spelled any other way is
`400` rather than `405`. The target is printable characters other than space, which is
what a target already is once it has been percent-encoded. Anything else in either is
`400`, because both are written to the device's console log and a log line should be
one line.

`401` carries a JSON body. Every other error status carries none — `Content-Length:
0` and a `Content-Type` of `text/plain; charset=utf-8`, which describes the nothing
that follows it. The status is the whole of the answer.

Limits, all enforced before a handler sees anything:

| | Limit | Exceeded |
| --- | --- | --- |
| Request head | 2048 bytes | `431` |
| Request body | 1024 bytes | `413` |
| Response body | 10240 bytes | `500` |

`Transfer-Encoding` is answered `501` rather than interpreted. Chunked framing is
not implemented, and a message carrying both it and `Content-Length` is the one
where believing the wrong header smuggles a second request past the first.

## Authentication

Every `/v1/…` request carries the device's API key:

```
X-API-Key: <key>
```

The key is generated on the device at first boot, lives in NVS, and is never
compiled in. It is shown once, on the page returned after the device joins a
network — that page is the only place it can be read without a reflash.

A request without a valid key is answered `401` with

```json
{"detail":"Invalid or missing API key"}
```

A missing key and a wrong key are indistinguishable in the response, which is
deliberate: telling the two apart tells whoever is guessing which half of the
problem they have solved.

The provisioning routes are exempt from this check **only while the device has
not joined a network**. Once it has, they require the key like everything else —
so recovering the key without a reflash stays possible, and learning it does not.

## `GET /v1/power`

What the machine is doing. Read-only.

```json
{
  "state": "on",
  "pending": "none",
  "observed_at_ms": 412934,
  "uptime_ms": 498210
}
```

| Field | Type | Meaning |
| --- | --- | --- |
| `state` | string | See below. Derived from the power LED, not from what was last asked for. |
| `pending` | string | `none`, `press` or `hold` — the request currently being served. |
| `observed_at_ms` | integer | Device uptime, in milliseconds, when `state` was entered. |
| `uptime_ms` | integer | Device uptime now. |

`state` is one of:

| Value | Meaning |
| --- | --- |
| `unknown` | The sense line has not settled yet. True for the first fraction of a second after boot, and for as long as the line will not hold a level. |
| `off` | The power LED is dark. |
| `on` | The power LED is lit. |
| `turning_on` | A request was taken up while off, and the LED has not come on yet. |
| `turning_off` | A request was taken up while on, and the LED has not gone out yet. |

The state is read from the LED rather than remembered, so somebody pressing the
case button by hand moves it here too.

**The two clocks are both this device's own.** Subtract to get the age of the
reading; neither side has to agree about what the time is. A restart shows up as
a `uptime_ms` that went backwards, which is the signal that anything a client
remembered about this device is stale.

`turning_off` can last a minute. A press while the machine is on is a request to
the operating system, which may take its time or put a dialog about unsaved work
on a screen nobody is looking at. When the wait runs out with the LED still lit,
the state returns to `on` — the request was made and it did not take.

| Status | When |
| --- | --- |
| `200` | Always, when authorised. |
| `401` | No valid key. |
| `405` | Any method other than `GET`. |

## `POST /v1/power/press`

A tap on the front-panel button: a power-on when the machine is off, and a
shutdown request to the running operating system when it is on. No request body.

| Status | Meaning |
| --- | --- |
| `202` | Taken up. **Accepted, not done** — what follows is a pulse and then however long the machine takes. |
| `409` | Refused, with nothing changed. |
| `401` | No valid key. |
| `405` | Any method other than `POST`. |

Both `202` and `409` carry the same body as `GET /v1/power`, so a refused client
learns the current state without asking a second question.

A `409` means one of three things, all of them temporary:

- the button line is still asserted from an earlier request,
- a transition is already running, or
- `state` is `unknown`, so the device does not know what a press would do.

**A retry cannot press twice.** A client that never saw its answer and asks again
is refused for as long as the first request is still being served. This matters
more than it looks: two pulses run together are a longer pulse, and a long enough
pulse is the hardware cut-off.

## `POST /v1/power/hold`

Holds the button past the four seconds a motherboard reads as a cut-off. The
supply is cut with whatever was in flight lost. No request body.

Same statuses and the same body as `press`.

This is **a separate route on purpose**. Cutting the power is a different
decision from asking for a shutdown, and it should not be reachable by getting a
field wrong in a request meant to be a press, nor by a client retrying one.

A hold while the machine is already off is not a cut-off — there is nothing to
cut — and a motherboard reads the pulse as an ordinary press. The state goes to
`turning_on`.

## Provisioning surface

Not versioned, not a contract, and documented here only so that nobody has to
read it out of the source.

`GET /` returns the setup page: an HTML form listing the networks a scan found.
The scan happens while the request is open, so the response takes a second and a
half.

`POST /network` takes that form back as
`application/x-www-form-urlencoded` with `ssid` and an optional `password`, saves
the credentials, and returns a page showing the API key. A name or password too
long to store is `400`. Joining is a separate step from saving, so a `200` here
means the credentials are stored, not that the network was reached.

`GET /hub` returns the page that says where this device announces itself, showing
the stored origin and device id, whether a hub key is stored, and how the last
announcement went. The key itself is never rendered — only whether there is one.

`POST /hub` takes that form back as `application/x-www-form-urlencoded` with
`origin`, `device_id` and `key`, all three required together. Anything this device
would not post to is `400`, without saying which field it was: the three rules are
on the page the form came from, and the values were supplied by whoever is asking.
A `200` means the settings are stored and announcing has started, not that the hub
has answered — `GET /hub` is where that shows up.

Both are reachable after provisioning, behind the API key like everything else, so
moving this device to a different hub does not mean taking its network away first.

## Status codes in one place

| Code | Raised by | Meaning |
| --- | --- | --- |
| `200` | handler | Read succeeded. |
| `202` | handler | A power request was taken up. |
| `400` | server, handler | Unparseable request line, invalid `Content-Length`, truncated body, or a form field that will not fit. |
| `401` | handler | No valid `X-API-Key`. |
| `404` | handler | No such path. |
| `405` | handler | Path exists, method does not. |
| `409` | handler | A power request was refused; nothing changed. |
| `413` | server | Body over 1024 bytes. |
| `431` | server | Head over 2048 bytes. |
| `500` | handler | The response would not fit its buffer, or saving credentials failed. |
| `501` | server | `Transfer-Encoding` present. |

## Compatibility

Inside `/v1`, from the release that adds this file onward:

1. **Fields are added, never removed and never retyped.** A client that reads
   `state` will keep finding a string called `state` there.
2. **Enumerations grow.** New values may appear in `state` and `pending`. A
   client must treat a value it does not recognise as `unknown` rather than as an
   error — that is what the value is for, and it is why it is in the list from
   the start.
3. **A status code does not change meaning** for a given method and path. A code
   may be added for a case that previously had none.
4. **Routes are not repurposed.** A path that answered something is never made to
   answer something else.

A change that cannot be made under those rules is a `/v2`, served alongside `/v1`
for at least one release, and announced in the README before `/v1` is withdrawn.

The provisioning surface is outside all of this, and so is anything a response
says in prose — a log line, an HTML page, or the wording of a `detail` string.
