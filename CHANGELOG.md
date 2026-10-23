# Changelog

All notable changes to this fork. The base is
[`makstech/esphome-BusT4`](https://github.com/makstech/esphome-BusT4) v1.2.2;
see [Releases](https://github.com/makstech/esphome-BusT4/releases) for the upstream history.

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
