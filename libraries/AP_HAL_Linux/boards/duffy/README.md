# Duffy Board Notes

Duffy is a Linux rover board profile for a Raspberry Pi class host driving a
two-motor skid-steer rover through a PCA9685 PWM controller and an L298N dual
H-bridge. The board uses `RCOutput_Duffy` for motor output and ArduPilot's
serial RC protocol path for receiver input.

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

For I.Bus RC input, connect the receiver signal to the UART RX used by
`SERIAL1`. I.Bus uses normal `115200` baud serial byte input and does not
require signal inversion on a normal UART RX pin. Make sure the selected UART
is enabled in Linux and is not being used as the Linux console.

## Runtime Serial Layout

On the observed Duffy Raspberry Pi setup, `/dev/serial0` points to the UART
mapped to the GPIO header serial pins.

Do not confuse Linux device names with ArduPilot serial port names. Linux
`/dev/serial0` is an operating-system device path. ArduPilot `SERIAL0` and
`SERIAL1` are autopilot serial port instances. The Duffy run command below maps
Linux `/dev/serial0` to ArduPilot `SERIAL1`.

The intended serial role split is:

| ArduPilot port | Duffy use |
|----------------|-----------|
| `SERIAL0` | TCP connection to the ground control station |
| `SERIAL1` | RC input receiver UART |

### Linux UART Preparation

Before starting Rover, make sure Linux exposes the receiver UART and that no
console service is using it.

Check which real UART backs `/dev/serial0`:

```sh
ls -l /dev/serial0
readlink -f /dev/serial0
```

Enable the Raspberry Pi UART in the boot configuration:

```text
enable_uart=1
```

On newer Raspberry Pi OS images this is normally in
`/boot/firmware/config.txt`; on older images it may be in `/boot/config.txt`.
Reboot after changing it.

Check the kernel command line:

```sh
cat /proc/cmdline
```

If the receiver UART appears as a console, remove that console assignment from
the boot command line. Common examples are:

```text
console=serial0,115200
console=ttyAMA0,115200
console=ttyS0,115200
```

On newer Raspberry Pi OS images the boot command line is normally
`/boot/firmware/cmdline.txt`; on older images it may be `/boot/cmdline.txt`.
Keep `cmdline.txt` as a single line.

Disable any serial getty service for the receiver UART:

```sh
sudo systemctl disable --now serial-getty@serial0.service
sudo systemctl disable --now serial-getty@ttyAMA0.service
sudo systemctl disable --now serial-getty@ttyS0.service
```

It is fine if one of those services does not exist. The important condition is
that the UART used for RC input is not owned by a login console.

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
barometer, or a real IMU:

```text
FRAME_CLASS 1
AHRS_EKF_TYPE 10
COMPASS_ENABLE 0
GPS_TYPE 0
ARMING_CHECK 0
FS_THR_ENABLE 0
```

The RC input defaults are:

```text
SERIAL1_PROTOCOL 23
RC_PROTOCOLS 1
MODE_CH 5
MODE1 0
MODE6 4
```

`SERIAL1_PROTOCOL=23` configures `SERIAL1` as RC input. `RC_PROTOCOLS=1` leaves
all RC protocols enabled for auto-detection, including I.Bus. To restrict
detection to I.Bus only after bring-up, use:

```text
RC_PROTOCOLS 4
```

`RC_PROTOCOLS` is a bitmask. Bit `2` is I.Bus, so I.Bus-only is `1 << 2`,
which is `4`.

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
- `RCInput_RCProtocol::_timer_tick()` returns early when there is no direct file
  descriptor and `AP::RC().has_uart()` is false.
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
