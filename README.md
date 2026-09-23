# esp32c3-pc-power

[![CI](https://github.com/DGPRoman/esp32c3-pc-power/actions/workflows/ci.yml/badge.svg)](https://github.com/DGPRoman/esp32c3-pc-power/actions/workflows/ci.yml)

Remote power control for a desktop PC, on an ESP32-C3.

The device sits inside the case and drives the motherboard's front-panel power
header through an optocoupler — the same signal path as the case button — while
sensing the power LED header to know whether the machine is actually on. That
makes a rarely-used desktop something you can leave switched off and bring up on
demand, from a phone, without a second machine having to stay awake to do it.

> **Status: the power path is written and has not met a motherboard yet.** The
> firmware drives the button line, samples the LED, and derives on, off,
> turning_on and turning_off from the two; `/v1/power` reports what the LED says
> rather than what was last asked for. What has not happened is bring-up: the two
> GPIO numbers in `main/board.h` are chosen rather than measured, and nothing here
> has been confirmed against a front-panel header. The paragraph above describes
> what the device is for; the checklist at the end is the accurate version.

Commands arrive over HTTP. The intended caller is
[`pihome-hub`](https://github.com/DGPRoman/pihome-hub), a self-hosted FastAPI control
plane, and the firmware here is the device half of that system — though the hub does
not call it yet, and nothing here depends on it beyond the HTTP contract. Anything on
the LAN holding the shared secret can drive it in the meantime.

The contract is written down in [`docs/http-api.md`](docs/http-api.md) — every route,
field and status code, and the rule for what may change under `/v1` without breaking
a caller.

## Why not Wake-on-LAN

WoL solves a third of the problem. It can turn a machine on, but it cannot turn
one off, it reports nothing back about whether it worked, and it depends on the
NIC holding standby power plus firmware and driver settings that differ between
boards. It also has no answer for a machine that has already hung.

Driving the front-panel header covers all of it: a short pulse is a power-on when
the machine is off and an ACPI shutdown request when it is on, a long hold is the
hardware cut-off of last resort, and the power LED gives a ground truth for state
that no network probe can. WoL is still worth having as a redundant path and may
be added alongside.

## How it works

```
   phone / browser
         │  HTTPS
         ▼
   pihome-hub (FastAPI)
         │  HTTP + shared-secret auth, on the LAN
         │  ▲
         │  ╎  announcement (device → hub), then polling (hub → device)
         ▼  ╎
   ESP32-C3 ─── optocoupler ──▶ PWR_BTN header   (pulse: on / shutdown)
         │
         ├───── divider ───────  PWR_LED header   (sense: is it running?)
         │
         └───── I²C ──────────▶  72×40 OLED       (state, IP, last command)
```

The device runs its own HTTP server rather than polling the hub, so a button press
takes one request and no waiting. The cost of that direction is that the hub has to
know where to send it, and DHCP means the answer changes.

**The device tells it.** On boot, and again whenever its address changes, it posts
its address and its own API key to `POST /v1/devices/{id}/announcements` on the hub.
The hub then polls `GET /v1/power` on a timer, presenting the key it was given. The
[contract is the hub's](https://github.com/DGPRoman/pihome-hub/blob/main/docs/devices.md);
this device is declared there under an id, which is what stops an announcement
introducing an address nobody chose.

Three things follow from how the hub records an announcement, and they are why the
schedule is what it is:

- **An announcement clears what the poller had recorded** — the last reading, the
  last error, and how long this device had been unreachable. So nothing here
  announces on a timer. Doing that would reset the hub's record of a fault every
  time it fired, which is the record somebody diagnosing the fault would be reading.
- **A hub that cannot be reached is retried**, starting at two seconds and backing
  off to five minutes. It ends by itself; this device just has to still be trying.
- **A hub that answers "no" waits five minutes from the first refusal.** A `401` or
  a `404` is somebody's configuration rather than the weather, and the hub counts
  failed authentication attempts per address — so a device retrying a rejected key
  every two seconds would lock itself out of the route it needs the moment somebody
  fixed the key.

None of it is on the path that switches the PC. A hub that is down, misconfigured or
absent leaves this device answering its own API exactly as it did before there was a
hub, which is what `curl` and the panel have always talked to.

## Hardware

- **ESP32-C3 SuperMini** variant with an on-board 0.42″ 72×40 SSD1306 OLED (I²C)
  and a single-colour LED on GPIO8. 4 MB flash, native USB Serial/JTAG — no
  USB-to-UART bridge, so the chip's USB peripheral is the console.
- **Optocoupler** (PC817 or an opto-isolated solid-state relay) across the two
  `PWR_BTN` pins of the front-panel header. Preferred over a mechanical relay:
  galvanic isolation between the ESP32 and the motherboard, no contact wear, and
  no coil transient near a running board.
- **Power-LED sense** on `PWR_LED+`, level-shifted into a GPIO.
- **Standby power** for the ESP32 — either its own USB supply or the
  motherboard's `5VSB` rail. Never the switched 5 V: a controller that loses power
  with the PC cannot turn the PC back on.

The GPIO assignments are in [`main/board.h`](main/board.h), which is the only file
that knows any of them. The two on the front-panel header are marked provisional:
they are chosen to be clear of the strapping pins, the display's I²C, the native
USB pair and the module's SPI flash, and bring-up is what turns that into a fact.

The button line is active high so that the safe level is the one a reset produces
— a pad leaves reset as a high-impedance input, which with the optocoupler's LED
pulled down is the button not pressed. Active low would make every reset a
keypress on the machine inside the case.

> **Working inside a PC:** unplug the power cord and drain the PSU before touching
> the front-panel header. The header itself is low-voltage logic, but the supply a
> few centimetres away is not.

## Firmware layout

| Path | Contents |
| --- | --- |
| `main/` | Entry point, application wiring, and the board's pin map |
| `components/` | Self-contained drivers and services, one per directory |
| `docs/` | [The HTTP contract](docs/http-api.md) |
| `test/host/` | Host tests for everything that does not need the chip |
| `test/check-contract.sh` | Fails if a route is served without being documented, or the reverse |
| `sdkconfig.defaults` | Tracked build configuration |

## Implementation notes

Peripheral access is written directly against the ESP32-C3 Technical Reference
Manual — the GPIO, I²C and timer drivers, the SSD1306 display driver and the HTTP
server are all implemented in this repository rather than pulled in. ESP-IDF is
used for the parts where that is not a real option: the Wi-Fi MAC and PHY are a
closed binary blob with no alternative, and FreeRTOS, lwIP and NVS come with it.

One thing is adopted rather than written: the 5×7 glyph table in
`components/font5x7` is the classic public-domain GLCD font, and the file says
so. The text renderer around it — the indexing, the column layout and the 72×40
geometry — is written here. Drawing ninety-five glyphs by hand would have
demonstrated patience rather than anything about the hardware.

## Building

Requires [ESP-IDF v5.5.5 or newer](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32c3/get-started/)
with the `esp32c3` target installed. The patch version is a minimum, not a
suggestion, and `main/idf_component.yml` enforces it: an older framework fails
the build instead of quietly producing different firmware. The same file caps
the range below 6.0, which is a statement about what has been compiled rather
than a prediction about what breaks.

```sh
. "$IDF_PATH/export.sh"      # or: . ~/esp/esp-idf/export.sh
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

The board appears as `/dev/ttyACM0` over native USB, with no driver needed. Exit
the monitor with `Ctrl+]`.

## Testing

The components that are pure logic — the request parser, the body readers, the
font table, the framebuffer — are tested on a workstation, with no toolchain and
no board:

```sh
make -C test/host
```

That compiles them for the host against thin stand-ins for the four ESP-IDF
headers they include, with warnings as errors, and runs the suite under
AddressSanitizer and UndefinedBehaviorSanitizer. The parser tests feed it
truncated, oversized and duplicated input on purpose, so an overread the
assertions happen not to notice is still caught.

The server itself runs here too, over a `socketpair`: lwIP's socket API is the
BSD one, so `serve_connection` is the real function reading a real socket. That
matters more than it sounds. Testing a parser in isolation shows what it decides,
never whether the server asks it — removing the `Transfer-Encoding` refusal left
every parser test passing.

Everything that touches a peripheral is not covered here and is not pretended to
be: those need the chip.

## Configuration

Wi-Fi credentials, the shared secret the hub authenticates with, and where the hub
is are all stored in NVS on the device and provisioned at runtime. Nothing secret is
compiled into the firmware or checked into this repository, which is also what makes
the same build artefact flashable to more than one unit.

**Pointing it at a hub.** `GET /hub` on the device — linked from the setup page, and
reachable afterwards with the device's API key — takes three things:

| Field | What it is |
| --- | --- |
| `origin` | `http://<address>` with an optional port. An address, never a name: resolving one would need DNS working before this device could say where it is. `https` is refused, because this device terminates no TLS and would be promising a guarantee it cannot keep. |
| `device_id` | The id this device is declared under in the hub's device file. An id the hub does not know is a `404`, which shows on this page. |
| `key` | The hub's **device key** — `PIHOME_DEVICE_API_KEY`. Not this device's own key, and not the hub's relay key. |

The page shows whether a key is stored, never the key. Announcing starts within a
second of saving, and how it went shows on that same page and on the setup page.

## Status

Early development. [`/v1` is frozen](docs/http-api.md#compatibility) as of the
commit that wrote it down: fields are added and never removed, enumerations grow,
and a breaking change would be a `/v2` served alongside it. The provisioning
pages are outside that promise and may change in any release.

- [x] Project scaffolding, build, flash, console
- [x] Register-level GPIO — status LED
- [x] Register-level I²C master, bus scan
- [x] SSD1306 driver — framebuffer, 72×40 geometry
- [x] Text rendering over an adopted 5×7 glyph table
- [x] Setup-mode access point, credentials in NVS
- [x] HTTP/1.1 server on BSD sockets
- [x] Provisioning portal, joining a network
- [x] Authenticated command endpoints
- [x] Power pulse output, LED sense, power-state machine — written and host-tested;
      the pin numbers are provisional until bring-up
- [x] Hub-side integration — this device announces its address and its key, and the
      hub polls `GET /v1/power` with it. Both halves exist; the schedule is
      host-tested, the wire format is checked against a running hub.

## License

MIT — see [LICENSE](LICENSE).
