# Xiaomi keyboard nanoapp AIDL service

`vendor.lineage.keyboard-service.xiaomi` is a clean implementation of Xiaomi's
stable `vendor.xiaomi.hardware.keyboardnanoapp_aidl` V1 transport. It registers
the exact framework-facing instance:

```
vendor.xiaomi.hardware.keyboardnanoapp_aidl.IKeyboardNanoapp_aidl/default
```

The two service calls and the two one-way callback calls intentionally match
the stock ABI. This implementation does not implement, trigger, schedule or
package Xiaomi authentication or firmware-upgrade flows. The byte transport
remains ABI-compatible, but those server-dependent policy state machines are
deliberately outside this service.

## Transport and connection policy

- Binder commands use `AA <length> <payload>` framing. The payload starts with
  `32 00` and is zero-padded to the 66-byte transfer required by
  `/dev/nanodev0`.
- Driver replies are returned to the client as `AA <length> <payload>`.
- The service actively requests pogo state every two seconds. A keyboard is
  connected only when the A2 response has no over-current indication and
  `(KEY_S & 0x63) == 0x23`.
- The service monitors the standard `SW_LID` and `SW_TABLET_MODE` switches from
  `gpio-keys`; the driver's local Hall query is retained as a fallback. Linux
  input devices are exposed only while the pogo check is valid and both Hall
  switches are open. On each gate transition, command `0x21` disables/enables
  touch reporting as well.
- Status or Hall data older than six seconds fails closed. Service/device
  errors and service restarts force `_inputenable` back to `0`.
- Command `0x28` is an inbound keyboard sleep notification (`length == 1`,
  value `0` means asleep). While asleep, active protocol polling is paused and
  the stale pogo result is discarded; after wake, the input devices and
  touchpad are enabled again only after a fresh pogo response.
- Display state command `0x25` remains owned by the Nanosic driver's panel
  notifier. The HAL exposes it for diagnostics but does not send a duplicate
  screen policy command.

The matching Nanosic driver must provide
`/sys/class/nanodev/nanodev0/_inputenable`. It defaults to disabled and
dynamically registers only Keyboard (`15d9:a3`), Consumer (`15d9:a4`) and
Touchpad (`15d9:a1`) together with the legacy synthetic mouse (`15d9:a2`).

## Queries and sysfs

The controller periodically requests pogo/Hall state, keyboard GSensor samples,
and keyboard, touchpad and MCU versions. It requests the keyboard identity once
per connection using the stock all-zero local-address fallback and parses the
returned Bluetooth address. The existing driver continues to publish its cached
status and versions through `_version176x` and `_version803x`; `_versionSDK` and
the other Nanosic debug/control nodes remain driver-owned.

`vendor.lineage.keyboard-client` can issue explicit `status`, `version`,
`hall`, `identity`, `gsensor`, `touchpad`, `backlight`, `power`, `caps`, `mute`,
or arbitrary `raw` commands. Typed arguments reject trailing parameters and
out-of-range values; backlight follows the stock `0..100` range. The confirmed
common feature opcodes are `0x21` (touchpad), `0x23` (backlight) and `0x25`
(screen off/on).

Backlight and indicator controls accept either the matching effect report or
the stock generic `f0` acknowledgement (`d5` is the original command and `d7`
is the status). That acknowledgement has a special layout, so it is checked by
its fixed vendor header and fields rather than incorrectly treating `d5` as a
payload length.

Caps and mute indicators use either opcode `0x2e` (legacy) or `0x26` (XM2022).
Their values are not interchangeable: legacy Caps uses `fd/fc`, XM2022 Caps
uses `01/00`, and mute uses `f7/f3`. Therefore the client requires an explicit
`legacy` or `xm2022` style and the service never guesses from an incomplete
model/version heuristic. The yudi-specific XM2022 exception is intentionally
not applied to liuqin.

The `0x64` GSensor report is decoded as signed 12-bit `(x, -y, -z)` in SI units
and is still forwarded unchanged to the AIDL callback. Calculating the usable
keyboard angle also requires the tablet's Android `SensorManager` sample and
belongs in framework policy, so the vendor service does not fabricate an angle.

NFC OneHop needs framework-generated payload and Bluetooth identity ownership;
BLE attach notification needs the corresponding system Bluetooth integration.
Both therefore remain transparent protocol traffic rather than incomplete HAL
policy. Stock liuqin does not advertise the `0x37/0xa0` peripheral-vibrator
feature (it is enabled only for other models), so no unsafe vibrator shortcut is
exposed. Authentication and firmware upgrade are intentionally unsupported as
described above.
