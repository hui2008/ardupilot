# Duffy Board Notes

Duffy is a Linux rover board profile for a Raspberry Pi Zero 2 W driving a
two-motor skid-steer rover through a PCA9685 PWM controller and an L298N dual
H-bridge. The board uses `RCOutput_Duffy` for motor output and ArduPilot's
serial RC protocol path for receiver input.

Bring-up order:

1. Verify PCA9685/L298N motor wiring.
2. Verify which Linux UART backs `/dev/serial0`.
3. Remove Linux serial console ownership from the receiver UART.
4. Confirm Linux can see I.Bus bytes from the receiver.
5. Start Rover with `SERIAL0` on TCP and `SERIAL1` on `/dev/serial0`.

## Hardware

`RCOutput_Duffy` opens PCA9685 I2C bus `1`, address `0x40`. The PCA9685 output
frequency defaults to `1000 Hz`.

The L298N wiring expected by the driver is:

| Motor | PCA9685 channel | L298N input | Driver field |
|-------|-----------------|-------------|--------------|
| Left | `5` | `ENA` | enable |
| Left | `4` | `IN1` | direction A |
| Left | `3` | `IN2` | direction B |
| Right | `0` | `ENB` | enable |
| Right | `2` | `IN3` | direction A |
| Right | `1` | `IN4` | direction B |

For I.Bus RC input, connect the receiver signal output to the Raspberry Pi UART
RX pin used by `SERIAL1`, and share ground between the receiver and Pi. I.Bus
uses normal `115200` baud serial byte input and does not require signal
inversion on a normal UART RX pin.

## Runtime Serial Layout

The intended serial role split is:

| ArduPilot port | Duffy use |
|----------------|-----------|
| `SERIAL0` | TCP connection to the ground control station |
| `SERIAL1` | RC input receiver UART |

Do not confuse Linux device names with ArduPilot serial port names. Linux
`/dev/serial0` is an operating-system device path. ArduPilot `SERIAL0` and
`SERIAL1` are autopilot serial port instances. The Duffy run command maps Linux
`/dev/serial0` to ArduPilot `SERIAL1`.

### UART Device Identity

Check which real UART backs `/dev/serial0`:

```sh
ls -l /dev/serial0
readlink -f /dev/serial0
```

On the observed Duffy Raspberry Pi Zero 2 W setup:

```text
/dev/serial0 -> ttyS0
```

This means the GPIO header serial pins are currently backed by `/dev/ttyS0`,
the mini-UART.

### Pi Zero 2 W UART Selection

The Raspberry Pi Zero 2 W has onboard Bluetooth. The Bluetooth chip may use the
PL011 UART internally. In that layout, the GPIO header serial pins are backed
by the mini-UART (`ttyS0`) instead of the PL011 UART (`ttyAMA0`).

Always enable the Raspberry Pi UART in the boot configuration:

```text
enable_uart=1
```

On newer Raspberry Pi OS images this is normally in
`/boot/firmware/config.txt`; on older images it may be in `/boot/config.txt`.
Reboot after changing it.

To put the more robust PL011 UART on the GPIO header serial pins, either
disable Bluetooth:

```text
enable_uart=1
dtoverlay=disable-bt
```

or keep Bluetooth enabled and move Bluetooth to the mini-UART:

```text
enable_uart=1
dtoverlay=miniuart-bt
```

If Bluetooth is disabled, also stop the Bluetooth UART service:

```sh
sudo systemctl disable --now hciuart.service
```

After reboot, check the serial alias again. When GPIO serial is backed by
PL011, the expected result is:

```text
/dev/serial0 -> ttyAMA0
```

Using the mini-UART can still work for I.Bus at `115200` baud. Using PL011 is
preferred when Bluetooth is not needed because its clocking is more robust.

### Linux Console Ownership

The receiver UART must not be used as a Linux serial console. A serial console
means the kernel and login service treat the UART as a terminal for boot logs
and login text, while Duffy needs the same UART for binary I.Bus receiver
frames.

Check the kernel command line:

```sh
cat /proc/cmdline
```

Observed Duffy Pi Zero 2 W output:

```text
coherent_pool=1M
8250.nr_uarts=1
snd_bcm2835.enable_headphones=0
cgroup_disable=memory
snd_bcm2835.enable_hdmi=1
snd_bcm2835.enable_hdmi=0
smsc95xx.macaddr=B8:27:EB:01:41:F2
vc_mem.mem_base=0x1ec00000
vc_mem.mem_size=0x20000000
console=ttyS0,115200
console=tty1
root=PARTUUID=efeabd0c-02
rootfstype=ext4
fsck.repair=yes
rootwait
ds=nocloud;i=rpi-imager-1780302854544
cfg80211.ieee80211_regdom=AU
```

`/proc/cmdline` is one physical line. It is wrapped above only to make each
boot argument easier to inspect. For UART ownership, the important entry is:

```text
console=ttyS0,115200
```

That tells Linux to use `/dev/ttyS0` as a serial console at `115200` baud. On
the observed Duffy setup `/dev/serial0 -> ttyS0`, so this is the same UART that
Rover would use for I.Bus RC input. Remove `console=ttyS0,115200` from
`cmdline.txt`.

The local display console can remain:

```text
console=tty1
```

`tty1` is the first Linux virtual console on the local monitor and keyboard
path. It is not a UART and does not conflict with the receiver serial port.

The other fields are unrelated to RC input. They configure boot memory,
audio/display options, root filesystem selection, first-boot cloud-init data,
and WiFi regulatory region.

Other serial console entries to avoid on the receiver UART are:

```text
console=serial0,115200
console=ttyAMA0,115200
```

On newer Raspberry Pi OS images the boot command line is normally
`/boot/firmware/cmdline.txt`; on older images it may be `/boot/cmdline.txt`.
Keep `cmdline.txt` as a single line. Reboot after changing it.

Disable any serial getty service for the receiver UART:

```sh
sudo systemctl disable --now serial-getty@serial0.service
sudo systemctl disable --now serial-getty@ttyAMA0.service
sudo systemctl disable --now serial-getty@ttyS0.service
```

It is fine if one of those services does not exist. The important condition is
that the UART used for RC input is not owned by a login console.

### Final UART Checks

After reboot, verify that the serial console was removed:

```sh
cat /proc/cmdline
```

There should be no `console=ttyS0,115200`, `console=serial0,115200`, or
`console=ttyAMA0,115200` entry for the receiver UART.

Verify that nothing has the UART open before starting Rover:

```sh
sudo lsof /dev/serial0
sudo lsof "$(readlink -f /dev/serial0)"
```

No output from `lsof` means no process currently has the device open.

### Rover Start Command

Run the vehicle with `SERIAL0` assigned to the TCP endpoint used by the ground
control station, and keep the receiver UART assigned to `SERIAL1`.

Example Rover start command:

```sh
./build/duffy/bin/ardurover \
    --serial0 'tcp:*:5760:wait' \
    --serial1 /dev/serial0
```

In this command, ArduPilot `SERIAL0` is a TCP server listening on port `5760`
for the ground control station, while ArduPilot `SERIAL1` reads receiver bytes
from the Linux device `/dev/serial0`. The quotes around `tcp:*:5760:wait` avoid
shell expansion of `*`.

Use a different Linux device path in `--serial1` if the receiver UART is not
`/dev/serial0` on the target.

If these command-line options are omitted, Linux HAL defaults are not the Duffy
runtime layout:

- `SERIAL0` falls back to the process console device, not a TCP port.
- `SERIAL1` has no Linux device path, so `SERIAL1_PROTOCOL=23` has no receiver
  UART to read from.

The parameter `SERIAL1_PROTOCOL=23` tells ArduPilot what `SERIAL1` is used for.
The command-line option `--serial1 /dev/serial0` tells the Linux HAL which OS
device backs that ArduPilot serial port.

## Board Defaults

Linux board-specific default parameters are stored in:

```text
libraries/AP_HAL_Linux/boards/duffy/defaults.parm
```

The Duffy defaults configure a rover bring-up profile without GPS, compass,
or compass. The board is expected to use the real MPU6500 IMU configured in
`libraries/AP_HAL_Linux/hwdef/duffy/hwdef.dat`; accelerometer and gyro
calibration should be saved into the runtime storage file after the first
real-IMU boot:

```text
FRAME_CLASS 1
AHRS_EKF_TYPE 3
COMPASS_ENABLE 0
GPS_TYPE 0
ARMING_SKIPCHK -1
FS_THR_ENABLE 0
```

Do not seed `INS_GYR_ID`, `INS_ACC_ID`, `INS_ACCOFFS_*`, or `INS_ACCSCAL_*`
from defaults for the real IMU setup. Those values must come from ArduPilot's
detected sensor IDs and normal sensor calibration flow.

The RC input defaults are:

```text
SERIAL1_PROTOCOL 23
SERIAL1_BAUD 115
RC_PROTOCOLS 4
RC_OPTIONS 34
RCMAP_ROLL 1
RCMAP_PITCH 2
RCMAP_THROTTLE 3
RCMAP_YAW 4
MODE_CH 5
MODE1 0
MODE6 4
```

`SERIAL1_PROTOCOL=23` configures `SERIAL1` as RC input, and
`SERIAL1_BAUD=115` selects `115200` baud for I.Bus. `RC_PROTOCOLS=4` restricts
RC protocol detection to I.Bus. `RC_OPTIONS=34` keeps the default neutral
throttle arming check bit and adds the "ignore MAVLink overrides" bit. This
prevents a ground station virtual joystick from masking the physical receiver
during bench bring-up.

The `RCMAP_*` values match a Mode 2 FlySky IA6B-style channel order:

| Function | RC input channel |
|----------|------------------|
| Roll / steering | `1` |
| Pitch | `2` |
| Throttle | `3` |
| Yaw | `4` |

For Rover, the important inputs are usually steering on channel `1`, throttle
on channel `3`, and mode selection on channel `5`.

To temporarily allow all receiver protocol decoders during bring-up, use:

```text
RC_PROTOCOLS 1
```

`RC_PROTOCOLS` is a bitmask. Bit `2` is I.Bus, so I.Bus-only is `1 << 2`,
which is `4`. Value `1` enables all protocols for auto-detection.

### Physical RC vs GCS Joystick

For a vehicle with a real RC receiver, common practice is to keep the physical
transmitter as the primary manual control and takeover path. The ground station
is normally used for telemetry, parameter changes, mode changes, missions, and
monitoring, not for silently replacing stick inputs.

Duffy defaults to `RC_OPTIONS=34`, which keeps the default throttle arming check
and ignores MAVLink RC overrides. This prevents a ground station virtual
joystick from pinning or replacing physical receiver channels. MAVLink mission,
arming, mode-change, and guided commands are separate from RC overrides and are
not disabled by this setting.

Only enable MAVLink RC overrides or a ground station joystick intentionally, for
example on a bench rover with no physical receiver or when testing companion
computer control. In that case, configure the control-link loss behavior and
failsafes for the MAVLink control source.

At compile time, Duffy is included in the Linux `RCInput_RCProtocol` path with
guards such as:

```cpp
CONFIG_HAL_BOARD_SUBTYPE == HAL_BOARD_SUBTYPE_LINUX_DUFFY
```

That makes the Duffy board build use the serial RC protocol input driver. The
actual receiver port and protocol selection are still controlled at runtime by
parameters such as `SERIAL1_PROTOCOL` and `RC_PROTOCOLS`.

The rover output defaults are:

```text
SERVO1_FUNCTION 73
SERVO1_MIN 1000
SERVO1_TRIM 1500
SERVO1_MAX 2000

SERVO3_FUNCTION 74
SERVO3_MIN 1000
SERVO3_TRIM 1500
SERVO3_MAX 2000
```

Function `73` is `ThrottleLeft` and function `74` is `ThrottleRight`.
`SERVO1_FUNCTION` maps to zero-based HAL output channel `0`, and
`SERVO3_FUNCTION` maps to zero-based HAL output channel `2`.

## Output Path

`RCOutput_Duffy` receives final zero-based HAL output channels from
`SRV_Channel::output_ch()`.

The high-level output path is:

1. Rover motor code writes `ThrottleLeft` or `ThrottleRight`.
2. `SRV_Channels` maps the function to a physical output such as `SERVO1`.
3. `SRV_Channel::output_ch()` calls `hal.rcout->write(ch_num, output_pwm)`.
4. `RCOutput_Duffy::write()` receives the zero-based channel and PWM value.

The driver handles only these two output channels:

| Function | Parameter | HAL channel | Motor |
|----------|-----------|-------------|-------|
| `ThrottleLeft` | `SERVO1_FUNCTION` | `0` | left |
| `ThrottleRight` | `SERVO3_FUNCTION` | `2` | right |

PWM values are centered on `1500 us`:

| PWM value | Motor state |
|-----------|-------------|
| near `1500 us` | stop |
| above `1500 us` | forward |
| below `1500 us` | reverse |

`RCOutput_Duffy::write()` constrains non-zero output to `1000..2000 us`. The
distance from `1500 us` determines the PCA9685 enable-channel duty cycle.

## RC Input Testing

Start with protocol auto-detection while bringing up receiver wiring:

```text
SERIAL1_PROTOCOL 23
RC_PROTOCOLS 1
```

After I.Bus input is proven, narrow protocol detection:

```text
RC_PROTOCOLS 4
```

With Rover running, watch receiver input in a ground station:

- `RC_CHANNELS` or `RCIN` messages should update when sticks move.
- `RCIN.C1` through `RCIN.C4` should be near `1000..2000 us`.
- `RCIN.C5` should select mode because Duffy defaults set `MODE_CH 5`.
- No input for longer than `RC_FS_TIMEOUT` should be treated as RC loss.
- If the direct I.Bus decoder shows moving channels but `RC_CHANNELS` has some
  channels stuck near `1500`, check for ground station virtual joystick or
  `RC_CHANNELS_OVERRIDE` messages. Duffy defaults set `RC_OPTIONS=34` to ignore
  MAVLink RC overrides during physical receiver testing.

For direct UART debugging on the Raspberry Pi, stop Rover first and check that
Linux is receiving bytes before debugging ArduPilot. Run this direct
`stty`/`cat` check only when ArduPilot is not using the receiver UART. Once
Rover is running with `--serial1 /dev/serial0`, ArduPilot owns that device.

```sh
stty -F /dev/serial0 115200 cs8 -parenb -cstopb -ixon -ixoff -crtscts raw
timeout 2 cat /dev/serial0 | od -An -tx1
```

The `stty` command configures `/dev/serial0` for raw I.Bus serial framing:

| Option | Meaning |
|--------|---------|
| `-F /dev/serial0` | Apply settings to `/dev/serial0`, not the current terminal. |
| `115200` | Set baud rate to `115200`, the I.Bus baud rate accepted by `AP_RCProtocol_IBUS`. |
| `cs8` | Use 8 data bits. |
| `-parenb` | Disable parity. |
| `-cstopb` | Use 1 stop bit. |
| `-ixon` | Disable outgoing XON/XOFF software flow control. |
| `-ixoff` | Disable incoming XON/XOFF software flow control. |
| `-crtscts` | Disable RTS/CTS hardware flow control. |
| `raw` | Disable terminal line processing so bytes pass through directly. |

Together, those settings are `115200` baud, 8 data bits, no parity, 1 stop bit,
raw byte mode, and no flow control.

Moving sticks should produce changing bytes. If there are no bytes, fix wiring,
UART enablement, console use, receiver bind, or receiver power before changing
ArduPilot code.

ArduPilot-side debug points:

- `RCInput_RCProtocol::init()` prints legacy direct-file-descriptor labels
  `SBUS FD` and `115200 FD`; for Duffy these are normally `-1` because the UART
  comes from `SERIAL1_PROTOCOL=23`. The `SBUS FD` label does not mean Duffy is
  using S.Bus.
- `RCInput_RCProtocol::_timer_tick()` reads direct file descriptors when they
  exist. For Duffy, it updates the serial-manager RC UART added by
  `SERIAL1_PROTOCOL=23`.
- `AP::RC().new_input()` becoming true means the protocol decoder accepted a
  receiver frame and updated channel values.

STM32/ChibiOS flight controllers are different from Duffy's Linux setup. On
STM32 boards, the board `hwdef.dat` fixes the physical RC input pin or UART.
RC input is usually either a dedicated `RCIN` receiver pad or a UART whose
`SERIALx_PROTOCOL` parameter is set to `23`. There is no Linux-style
`--serial1 /dev/...` command-line device binding.

## Driver Notes

`RCOutput_Duffy::write()` updates staged PCA9685 channel values and flushes the
corresponding channels over I2C. It does not generate PWM timing in software.
The PCA9685 is the timing engine.

For reference, the normal ArduPilot RCOutput pattern is:

```text
write() = update the desired output value
hardware, firmware, timer, simulator, or external PWM chip = produce the output
```

`SERVOx_FUNCTION` names are runtime parameter names generated by the parameter
system. They are not literal C++ symbols. The stored field is
`SRV_Channel::function`; `SERVO1_FUNCTION` is the user-facing parameter name for
channel 1.

## Build Verification

The Duffy board defaults file is embedded by waf during the Linux board build.
Build the Rover binary and RC output examples with:

```sh
./waf configure --board duffy
./waf rover
./waf --targets examples/RCInputToRCOutput,examples/RCOutput,examples/RCOutput2
```

The build output should include:

```text
Embedding file defaults.parm:libraries/AP_HAL_Linux/boards/duffy/defaults.parm
```

The current Duffy changes were verified with `./waf configure --board duffy`
and the `RCInputToRCOutput`, `RCOutput`, and `RCOutput2` example targets.

## Appendix: Raspberry Pi Serial Console

Raspberry Pi OS can use the GPIO header UART as a Linux serial console. This is
useful for headless debugging before network or HDMI access works.

With a `3.3 V` USB-to-TTL serial adapter, connect:

| USB-to-TTL adapter | Raspberry Pi Zero 2 W |
|--------------------|-----------------------|
| `GND` | `GND` |
| `RX` | `GPIO14 / TXD` |
| `TX` | `GPIO15 / RXD` |

Do not connect a `5 V` adapter signal to the Pi UART pins, and do not use an
RS-232 level serial adapter. The Pi GPIO UART is `3.3 V` TTL.

Open the adapter from another computer at `115200 8N1`. For example:

```sh
screen /dev/ttyUSB0 115200
```

or:

```sh
picocom -b 115200 /dev/ttyUSB0
```

When `cmdline.txt` contains a serial console entry such as:

```text
console=ttyS0,115200
```

the serial terminal can show kernel boot messages. If a serial getty service is
enabled, it can also show a login prompt after boot.

For Duffy runtime, those same GPIO UART pins are used for I.Bus RC input.
Disable the serial console and getty before connecting the receiver and
starting Rover with `--serial1 /dev/serial0`.
