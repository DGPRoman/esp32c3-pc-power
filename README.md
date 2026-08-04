# esp32c3-pc-power

Remote power control for a desktop PC, on an ESP32-C3.

The device sits inside the case and drives the motherboard's front-panel power
header through an optocoupler — the same signal path as the case button — while
sensing the power LED header to know whether the machine is actually on. That
makes a rarely-used desktop something you can leave switched off and bring up on
demand, from a phone, without a second machine having to stay awake to do it.

Commands arrive over HTTP from [`pihome-hub`](https://github.com/DGPRoman/pihome-hub),
a self-hosted FastAPI control plane; the firmware here is the device half of that
system and has no dependency on it beyond the HTTP contract.

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
         ▼
   ESP32-C3 ─── optocoupler ──▶ PWR_BTN header   (pulse: on / shutdown)
         │
         ├───── divider ───────  PWR_LED header   (sense: is it running?)
         │
         └───── I²C ──────────▶  72×40 OLED       (state, IP, last command)
```

The device runs its own HTTP server rather than polling the hub, so a button press
takes one request and no waiting. Two mechanisms keep that reachable: the device
announces its address to the hub on boot and on every IP change, and the hub polls
`GET /status` on a timer. Between them, either side noticing the other has gone
quiet is a detectable event rather than a silent failure.

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

Exact GPIO assignments and the wiring diagram are recorded in `docs/hardware.md`
once bring-up has confirmed them on the board in hand.

> **Working inside a PC:** unplug the power cord and drain the PSU before touching
> the front-panel header. The header itself is low-voltage logic, but the supply a
> few centimetres away is not.

## Firmware layout

| Path | Contents |
| --- | --- |
| `main/` | Entry point and application wiring |
| `components/` | Self-contained drivers and services, one per directory |
| `docs/` | Wiring, pin assignments, HTTP contract |
| `sdkconfig.defaults` | Tracked build configuration |

## Implementation notes

Peripheral access is written directly against the ESP32-C3 Technical Reference
Manual — the GPIO, I²C and timer drivers, the SSD1306 display driver and the HTTP
server are all implemented in this repository rather than pulled in. ESP-IDF is
used for the parts where that is not a real option: the Wi-Fi MAC and PHY are a
closed binary blob with no alternative, and FreeRTOS, lwIP and NVS come with it.

## Building

Requires [ESP-IDF v5.5](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32c3/get-started/)
with the `esp32c3` target installed.

```sh
. "$IDF_PATH/export.sh"      # or: . ~/esp/esp-idf/export.sh
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

The board appears as `/dev/ttyACM0` over native USB, with no driver needed. Exit
the monitor with `Ctrl+]`.

## Configuration

Wi-Fi credentials and the shared secret the hub authenticates with are stored in
NVS on the device and provisioned at runtime. Nothing secret is compiled into the
firmware or checked into this repository, which is also what makes the same build
artefact flashable to more than one unit.

## Status

Early development. The HTTP contract is not stable yet.

- [x] Project scaffolding, build, flash, console
- [x] Register-level GPIO — status LED
- [x] Register-level I²C master, bus scan
- [x] SSD1306 driver — framebuffer, 72×40 geometry
- [x] 5×7 font and text rendering
- [x] Setup-mode access point, credentials in NVS
- [ ] Provisioning portal, joining a network
- [ ] HTTP server and authenticated command endpoints
- [ ] Power pulse output, LED sense, power-state machine
- [ ] Hub-side integration

## License

MIT — see [LICENSE](LICENSE).
