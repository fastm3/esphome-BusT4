# Changelog

All notable changes to this fork. The base is
[`makstech/esphome-BusT4`](https://github.com/makstech/esphome-BusT4) v1.2.2;
see [Releases](https://github.com/makstech/esphome-BusT4/releases) for the upstream history.

## v1.3.6 — discarded frames really end on silence, and corrected counters

- **`DISCARD` no longer resynchronises on a `0x00`.** v1.3.4 left that state on "the next
  break", but the UART delivers a break as a plain `0x00`, and `0x00` also appears in almost
  every frame (addresses, status bytes). The parser therefore left `DISCARD` at the first
  `0x00` of the rest of the bad frame and restarted the sync search mid-frame - the very case
  the state was meant to prevent. It only recovered on the 50 ms deadline. `DISCARD` now ends
  when the line has been silent for 15 ms (bytes reach the driver in bursts of up to ~9.4 ms,
  so a shorter gap can occur inside a frame), with the 50 ms deadline kept for a bus that never
  goes quiet.
- **`get_tx_frames()` counted every frame twice** - once when queued, once when sent. It now
  counts frames actually transmitted.
- **The counters are `std::atomic`.** The bus task and the main loop update them, and `++` on a
  plain `uint32_t` can lose increments across tasks.

## v1.3.5 — dropped frames at boot, and an encoder that could be missed for good

Both were exposed by the counters added in v1.3.4, on the first boot that had them: the device
reported six lost frames and `Position source: Time-based estimation` while the controller was
answering normally.

- **The boot burst overflowed the transmit queue.** The cover init steps, every register
  sensor's first poll and the restored template numbers all queue within a second, while the bus
  drains one frame per ~200 ms. Every send path called `write(&packet, 0)`, i.e. never waited,
  so past the queue depth the frame was dropped - six of them on the reference unit. The queue
  is now 64 deep and a send waits up to 30 ms for a slot.
- **A missed position reply disabled encoder tracking until the next manoeuvre.** The periodic
  refresh asked for the position only when `has_encoder_` was already true, and the init
  sequence never requested the current position at all. One lost reply at boot therefore left
  the cover on time-based estimation indefinitely: nothing else would ever ask again. The init
  now requests the position, and the periodic refresh no longer gates it on prior detection.
- **Drop counters are split by cause**: `get_tx_dropped()` (queue full) and
  `get_tx_not_ready()` (sent before the bus component's setup created the queues). Every send
  path, discovery and the identity requests included, goes through the same accounting.
- **`Encoder support detected (position N)` is logged** when the encoder is first seen.
  `dump_config` prints the position source at boot, before any reply can have arrived, so that
  line alone could not distinguish an encoder controller from a time-based one.

## v1.3.4 — the parser can no longer deadlock the bus

Reported by another fork on a Robus RBS600, then confirmed here: the transmit path only runs
while the receive parser is in `WAIT_SYNC`. A frame that stopped arriving mid-way left the
parser in `DATA` / `TRAILING_SIZE` for ever, so nothing was polled - and with nothing polled,
the control unit had no reason to talk, so nothing could resynchronise it. The link stayed
dead until the gate was moved by other means (a remote), whose traffic completes the stuck
frame. From Home Assistant it looked like a device that answers but ignores commands.

The evidence on the reference unit is in its history: the cover went from `closed` to `open`
without ever reporting an `opening` transition, across a window of about 10 h 30 in which the
device stayed connected to Home Assistant the whole time (no `unavailable` entry). So the API
and the WiFi were fine; the bus was deaf.

- A frame that has not completed within 50 ms is abandoned. The largest frame is 63 bytes
  (32.8 ms at 19200 8N1) and arrives without gaps, so a 50 ms gap can only mean the frame will
  never complete. The parser returns to `WAIT_SYNC` and the transmit gate reopens, which
  breaks the deadlock without waiting for someone to move the gate.
- A frame that fails the trailing-size, header or payload check now enters a `DISCARD` state
  instead of resynchronising in the middle of the frame still arriving - where a payload
  `0x55` is taken for a sync byte and the byte after it for a length, which is the most likely
  way into the stuck state in the first place. `DISCARD` leaves on the next break, or on the
  same 50 ms deadline, so it recovers even on a bus that never goes quiet. (The "next break"
  exit did not work - a break reads as an ordinary `0x00`; fixed in v1.3.6.)
- Frames that cannot be queued are no longer lost silently. Every send path called
  `write(&packet, 0)` and dropped the result, so a full transmit queue (32 frames) swallowed
  commands with no trace in the log. It is now counted and logged with the target and command.
- New counters for YAML: `get_rx_discarded()`, `get_tx_dropped()`, `get_tx_frames()` and
  `get_rx_packets()`, so a link problem becomes an entity instead of a guess. `example.yaml`
  exposes three of them as diagnostic template sensors.

## v1.3.3 — the previous manoeuvre's acknowledgement is not the end of this one

On an MC824H a command is answered ~50 ms later by a DEP status of `STA_ENDTIME` (`0x06`,
"manoeuvre ended by timeout") that describes the *previous* run; only 3-4 s later does the
controller report `STA_OPENING` / `STA_CLOSING`. The component took the first packet for
the end of the movement, so the cover published IDLE with a clamped position (5 % / 95 %)
and asked for a status confirmation in between - two state changes per order, and the
reason automations carry a `for:` debounce.

- `BusT4Cover` now tracks when a command was sent and whether the movement it asked for
  has been reported as started. `STA_ENDTIME` is ignored until then, with a 5 s guard so a
  manoeuvre that never starts still falls back to the old behaviour. `send_cmd()` is
  overridden on the cover, so the YAML buttons, the lock and `control()` are all covered.
- The cover no longer flickers to IDLE with a stale position right after an order.

## v1.3.2 — per-motor replies no longer reach the wrong sensor

Found on real traffic: the control unit answers a per-motor parameter with the motor index
as its first byte, and `parse_dmp_packet` forwards the reply to *every* sensor attached to
that register. Two sensors on `0x25` (`motor: 0` and `motor: 1`) therefore alternated
between 1100 and 1300, each publishing whichever motor answered last.

- A sensor with `motor` set now ignores a reply whose first byte reports a different
  index, so each sensor keeps its own motor's value.
- `docs/MC824H.md` and `example-mc824h.yaml` correct the force layout: measuring `0x4A`
  and `0x4B` repeatedly shows `[motor index] [force]` on both, the force being the second
  byte (4 on the factory settings), not the first.

## v1.3.1 — decode multi-byte register values behind a prefix

- The `sensor` platform gains `offset` and `length`, so a value that sits behind
  another field can be decoded as a big-endian integer. This is what the MC824H
  deceleration zones need: `0x24`/`0x25` answer `[motor index] [2-byte value]`, which
  `byte` alone could not express (`offset: 1` + `length: 2` gives the 800/1100 pulses
  instead of the raw 3-byte blob).
- `byte: N` remains available and is equivalent to `offset: N` with `length: 1`.
- `example.yaml` now exercises both decoding paths, so CI compiles them.

## v1.3.0 — register sensors

- New `sensor` platform: `platform: bus_t4` publishes a control-unit parameter as a
  Home Assistant entity instead of only logging it. `register` selects the parameter
  address, `motor` adds the motor index as a request argument (required by the MC824H
  for its per-motor values), and `byte` publishes a single byte of the reply for the
  parameters whose layout is not established — the MC824H forces `0x4A`/`0x4B`, for
  example, answer two bytes in an order that is not yet documented.
- `BusT4Cover::request_register(register, motor)` and
  `BusT4Cover::register_register_reader()` expose that plumbing to the sensor platform;
  the cover still owns the bus conversation and forwards each reply to the sensors
  attached to that register.
- `example.yaml` grows two register sensors so CI compiles the new platform.

## v1.2.4 — format-string fix in the OXI remote logging

- `parse_oxi_packet()`: the two log lines that print a 32-bit remote serial
  (`Remote: serial=%08X` and `Button press: remote=%05X`) passed a `uint32_t`
  to a `%X` conversion. On xtensa `uint32_t` is `unsigned long`, so both lines
  emitted a `-Wformat=` warning on every build. The value is now cast to
  `unsigned`, which matches the conversion on every target; the printed value is
  unchanged.
- No behaviour change: the MC824H work from v1.2.3 is untouched.

## v1.2.3 — Nice MC824H / MC824HR support

Fork of upstream **v1.2.2**. The patch is purely additive: no upstream line is
removed except the four no-argument requests that the MC824H branch replaces with
their per-motor equivalents.

### Added

- `BusT4Device::send_info_request(target, command, args, args_size)`: DMP GET
  carrying argument bytes (`[target] [command] [REQ_GET] [offset] [length] [args…]`).
  The MC824H needs the **motor index** for its position and per-motor parameters;
  without it `INF_CUR_POS` answers a constant value and the cover never moves.
- `BusT4Device::send_config_write(param, bytes, length, offset = 0x00)`: DMP SET
  with an arbitrary payload length, required for parameters wider than one byte
  (`send_config_set()` can only express one). Used for the deceleration zones
  `0x24`/`0x25`, whose value is `[motor index] [2-byte big-endian pulses]`.
- `INF_MIN_CLS` (`0x13`) in `t4_packet.h`: the MC824H does not implement
  `INF_POS_MIN`, it reports the fully closed position through this command. The
  reply is decoded by the same branch as `INF_POS_MIN` (`case INF_MIN_CLS:` falls
  through to it), and `read_position_value()` strips the motor-index byte that
  prefixes the 2-byte value.
- Automatic MC824H detection from the `INF_PRD` product name (`is_mc824h_`,
  reported as `Mode: MC824H` in the boot log). Other controllers keep the
  upstream behaviour.
- `docs/MC824H.md`: register map measured on a real MC824H (firmware `CD08`)
  driving a two-leaf swing gate, with the observed behaviour of the deceleration
  zones.
- `README.md`: MC824H section and documentation of `send_config_write()`.

### Changed

- `INF_POS_MAX`, `INF_MIN_CLS`/`INF_POS_MIN`, `INF_MAX_OPN` and `INF_CUR_POS` are
  requested with the motor index as an argument when the control unit is an
  MC824H; the single-argument requests used by every other controller are
  unchanged.

### Notes

- Multi-byte writes carry no range check: the control unit may clamp or reject an
  out-of-range value, so read every write back with a matching
  `send_info_request()`.
- The `CFG_*` names in `t4_packet.h` still disagree with two independent reference
  implementations (upstream issue #34); the MC824H work addresses parameters by
  raw address only.
- Credit: the motor-argument idea was first published in
  [`yawor/esphome-BusT4`](https://github.com/yawor/esphome-BusT4) branch `mc824h`
  (commit `f2a5078`). This fork applies the same idea to the v1.2.x code base and
  adds `send_config_write()`.
