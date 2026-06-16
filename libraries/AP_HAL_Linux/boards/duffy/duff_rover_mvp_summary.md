# Duff Rover MVP Summary

## Goal

Bring up a minimal ArduPilot Rover target for the Duff Linux board using:

- Raspberry Pi style Linux HAL board subtype `duff`
- PCA9685 PWM driver on I2C bus 1, address `0x40`
- L298N dual H-bridge motor driver
- Skid-steering rover output
- No physical IMU for the first MVP

The wiring is treated as correct and remains based on `my/py/pca9685_l298n_wiring.md`.

## RC Output Implementation

Implemented `AP_HAL_Linux/RCOutput_Duff` as a real `AP_HAL::RCOutput` backend instead of inheriting from `Empty::RCOutput`.

The Duff backend drives the PCA9685 directly over I2C and translates Rover servo outputs into L298N direction and enable signals.

Motor mapping:

| Rover output | Motor | PCA9685 channels | L298N pins |
| --- | --- | --- | --- |
| `SERVO1` / channel 0 | Left | CH0, CH1, CH2 | ENA, IN1, IN2 |
| `SERVO3` / channel 2 | Right | CH5, CH3, CH4 | ENB, IN3, IN4 |

The right motor mapping intentionally follows the wiring document:

- CH3 = IN3
- CH4 = IN4
- CH5 = ENB

This was not changed to match the PDF's conceptual order because the real wiring document is the source of truth.

## Motor Behavior

The backend interprets standard Rover PWM values:

- `1500 us` = stop
- Above deadzone = forward
- Below deadzone = reverse
- `0 us` = hard stop / disabled output state

Important fix: `write(ch, 0)` is preserved as zero and is not clamped to `1000 us`. This matters because ArduPilot can call zero-output paths during safety or shutdown. Clamping zero to `1000 us` would command full reverse instead of stopping.

The PCA9685 is configured at `1000 Hz` by default. Enable channels use duty cycle proportional to throttle demand, while direction channels are driven fully on/off.

## Four-Motor Rover Support

ArduPilot Rover supports four physical motors when the vehicle is still controlled as a skid-steer rover.

The usual control model is still left side and right side:

```text
Left front + left rear   = ThrottleLeft
Right front + right rear = ThrottleRight
```

That means the same ArduPilot motor function can drive both motors on one side. This can be done in hardware by wiring paired motors to one suitable motor driver channel, or in software/hardware by duplicating the left and right commands to multiple driver channels.

Duff's current PCA9685 + L298N backend implements two logical motors:

```text
SERVO1_FUNCTION = 73  # ThrottleLeft
SERVO3_FUNCTION = 74  # ThrottleRight
```

So it already matches the standard skid-steer control model. No code change is needed if the two left motors are electrically paired and the two right motors are electrically paired.

A code change is needed only if Duff should drive four independent H-bridge channels from the PCA9685. In that case the backend would duplicate the left command to left-front/left-rear channels and the right command to right-front/right-rear channels, or implement a more advanced per-wheel control model.

The L298N is current-limited and inefficient, so it may not be a good choice for four motors unless the motors are very small. For a four-motor rover, a better hardware direction is two stronger dual H-bridge drivers or four separate motor driver channels.

## PCA9685 Robustness

The Duff backend now:

- Initializes the PCA9685 into a stopped state before enabling output
- Uses auto-increment register writes for contiguous channel updates
- Tracks staged channel ticks before flushing to hardware
- Preserves pending output writes across cork/push cycles until a flush succeeds
- Handles safety on/off by explicitly staging and flushing both motors stopped or restored
- Uses the same PCA9685 prescale style as `RCOutput_PCA9685.cpp`, including the 25 MHz clock correction factor

## Console Status Messages

Console logging was added so hardware bring-up can be observed without a debugger.

Messages include:

- PCA9685 open/init failures
- PCA9685 frequency configuration
- Backend ready status
- Channel enable/disable
- Safety on/off
- Motor state transitions and coarse PWM changes

Motor write messages are rate-limited by state change or significant PWM delta to avoid flooding the console.

## ArduPilot Logging Practice

Normal ArduPilot logging depends on who needs to read the message:

- Operator/runtime status should usually use `GCS_SEND_TEXT(...)` or `gcs().send_text(...)`. These produce MAVLink `STATUSTEXT` messages that show up in Mission Planner, QGroundControl, MAVProxy, and similar GCS tools.
- Persistent post-run records should use `AP_Logger`, such as `Write_Message(...)` or a structured log message, so the event is saved in the dataflash log.
- Low-level HAL bring-up messages often use `hal.console->printf(...)`, but on Linux this can share the same stream as `SERIAL0` and can mix human-readable text with binary MAVLink bytes.
- Developer-only diagnostics can use `DEV_PRINTF(...)`, which is compiled out unless developer/debug support enables it.

For the Duff MVP, `RCOutput_Duff` writes its `RCOutput_Duff: ...` messages to `stderr` through a local `duff_log(...)` helper. This is a practical Raspberry Pi console bring-up choice because it keeps Duff status separate from binary stdout/`SERIAL0` output.

Longer term, important Duff runtime status should move to `GCS_SEND_TEXT(...)`, with persistent events optionally mirrored into `AP_Logger`. The `stderr` helper should remain only for very early hardware bring-up or failures that happen before normal GCS status text is usable.

## Default Duff Parameters

`libraries/AP_HAL_Linux/boards/duff/defaults.parm` configures the Rover frame and motor outputs:

```text
FRAME_CLASS 1

SERVO1_FUNCTION 73
SERVO1_MIN 1000
SERVO1_TRIM 1500
SERVO1_MAX 2000

SERVO3_FUNCTION 74
SERVO3_MIN 1000
SERVO3_TRIM 1500
SERVO3_MAX 2000
```

This maps Rover's throttle-left and throttle-right functions onto the two motor outputs used by `RCOutput_Duff`.

## No-IMU MVP Support

For the MVP there is no physical IMU.

Duff now explicitly selects no physical INS in `libraries/AP_HAL_Linux/hwdef/duff/hwdef.dat`:

```text
define HAL_INS_DEFAULT HAL_INS_NONE
```

However, `HAL_INS_NONE` alone is not enough on Linux Rover. The current ArduPilot code path still needs a usable inertial backend during startup. Without one, Duff can fail with an INS initialization/config error.

To solve this, the existing `AP_InertialSensor_NONE` mock backend was generalized behind a compile-time gate:

```text
AP_INERTIALSENSOR_NONE_BACKEND_ENABLED
```

The default behavior remains unchanged for other boards. ESP32 still enables the mock backend by default, and Duff opts in explicitly from its hwdef:

```text
define AP_INERTIALSENSOR_NONE_BACKEND_ENABLED 1
```

Duff also adds MVP parameters to avoid sensor-related startup blockers:

```text
AHRS_EKF_TYPE 10
INS_GYR_CAL 0
INS_GYR_ID 2752772
INS_ACC_ID 2753028
INS_ACCOFFS_X 0.001
INS_ACCOFFS_Y 0.001
INS_ACCOFFS_Z 0.001
INS_ACCSCAL_X 1.001
INS_ACCSCAL_Y 1.001
INS_ACCSCAL_Z 1.001
COMPASS_ENABLE 0
GPS_TYPE 0
ARMING_SKIPCHK -1
FS_THR_ENABLE 0
```

The `INS_GYR_ID` and `INS_ACC_ID` values match the synthetic `AP_InertialSensor_NONE` device IDs used by this MVP. The nonzero accel offsets and scale values prevent the mock accel from failing the normal `3D Accel calibration needed` prearm check on a fresh storage directory.

Current Rover firmware has replaced the old `ARMING_CHECK` parameter with
`ARMING_SKIPCHK`. For this bench-only no-sensor MVP, `ARMING_SKIPCHK -1` is the
closest equivalent to the old `ARMING_CHECK 0` behavior because ArduPilot's
4.7 parameter conversion maps old `ARMING_CHECK=0` to `ARMING_SKIPCHK=-1`.
Leaving `ARMING_CHECK` in `defaults.parm` causes QGroundControl to report:

```text
Parameters are missing from firmware ... Missing params: 1:ARMING_CHECK
```

Runtime note: if the GCS reports the following after boot, the direct fix is to
run accelerometer calibration:

```text
PreArm: 3D Accel calibration needed
```

In QGroundControl, use **Vehicle Setup** -> **Sensors** -> **Accelerometer**,
complete the calibration flow, then reboot the vehicle. This saves the accel ID,
offsets, and scale values into the `.stg` parameter storage.

After calibration, verify these parameters are populated:

```text
INS_ACC_ID
INS_ACCOFFS_X/Y/Z
INS_ACCSCAL_X/Y/Z
```

If the pre-arm message returns after a successful calibration and reboot, then
debug parameter storage persistence. In that case, investigate the Duff storage
path, confirm the same `.stg` file is reused between boots, and check that the
file timestamp/content changes when calibration parameters are saved.

### Default Parameters vs Persisted Storage

Duff's `defaults.parm` is embedded into the Rover binary as ROMFS data and is
loaded as a default-override file:

```text
Embedding file defaults.parm:libraries/AP_HAL_Linux/boards/duff/defaults.parm
```

These values are defaults, not forced EEPROM writes. ArduPilot applies them to
RAM when a parameter is not already configured in storage, but it does not
automatically save them to the `.stg` file. This protects user-tuned parameters
from being overwritten on every boot.

For the accel ID check, this distinction matters. Accelerometer calibration is
the normal way to create saved calibration state. `AP_InertialSensor` calls
`load()` on the accel ID parameter and only treats the ID as calibration state if
that value was actually loaded from saved storage. A value supplied only by
`defaults.parm` can appear in the parameter list but still fail the saved-ID
check.

On Linux, persistent parameter storage is a binary `.stg` file named after the
vehicle target. Depending on how the binary is launched and named, Duff Rover
bring-up may use a file such as:

```text
ardurover.stg
Rover.stg
```

If `ardurover` is launched with `--storage-directory /some/path`, the storage
file is in that directory. Otherwise it is placed under the board's default
Linux storage directory or the working/state directory selected by the HAL.

To locate the file on the target:

```sh
find . /tmp /var /home -name ardurover.stg -o -name Rover.stg 2>/dev/null
```

To check the file directly:

```sh
ls -l /path/to/Rover.stg
xxd -g1 -l 128 /path/to/Rover.stg
```

A valid AP_Param storage file starts with:

```text
50 41 06 00
```

That is the `PA` magic plus storage revision `6`. In 16-bit `hexdump` output it
can appear as:

```text
0000000 4150 0006 ...
```

The dump observed during Duff bring-up had this valid header, then only a small
number of records followed by mostly zero-filled storage. That is consistent
with a valid but mostly empty parameter store, where many values are still only
coming from `defaults.parm`.

ArduPilot includes a low-level dump helper:

```sh
gcc -o /tmp/eedump_apparam libraries/AP_Param/tools/eedump_apparam.c
/tmp/eedump_apparam /path/to/Rover.stg
```

Expected output begins with:

```text
Header OK
```

The helper prints raw AP_Param keys, group elements, types, and values. It does
not resolve names such as `INS_ACC_ID`, but it is useful for confirming that the
storage file is valid and contains saved records.

QGroundControl can view and edit the parameters exposed over MAVLink, but it
cannot directly inspect the `.stg` file. In QGC, use **Vehicle Setup**,
**Parameters**, then search for:

```text
INS_ACC_ID
INS_ACCOFFS
INS_ACCSCAL
```

Seeing `INS_ACC_ID=2753028` in QGC does not prove the value came from saved
storage, because it may be supplied by embedded defaults. The practical test is:

1. Run QGC accelerometer calibration.
2. Reboot without changing the storage directory.
3. Confirm the pre-arm message is gone.
4. Confirm the `.stg` timestamp/content changed.

`FS_THR_ENABLE 0` disables Rover's throttle/radio failsafe for this bench-only no-RC bring-up. Without it, the GCS can report:

```text
PreArm: Radio failsafe on
```

These defaults are intended for bring-up and motor testing only. They should be revisited when real sensors or real RC input are added.

## No-Barometer MVP Support

For the MVP there is also no physical barometer.

Duff allows startup without a barometer in `libraries/AP_HAL_Linux/hwdef/duff/hwdef.dat`:

```text
define HAL_BARO_ALLOW_INIT_NO_BARO 1
```

Without this, startup can fail with:

```text
Config Error: Baro: unable to initialise driver
Config Error: fix problem then reboot
```

This does not add a fake barometer. It only tells ArduPilot that no barometer is acceptable for this Duff MVP. This should be revisited when real sensors are added.

## Compass For Rover

A compass is not always required for Rover.

GPS gives position, but GPS heading/course is only reliable once the rover is moving. When the rover is stopped, GPS cannot tell which way the vehicle is facing. A compass can help provide heading before movement, during slow driving, and while turning.

For ground vehicles, a compass can also be a liability. Motors, battery wiring, steel chassis parts, and motor drivers such as the L298N can create magnetic interference. A bad compass can make heading estimation worse than running without one.

For the Duff MVP, compass is disabled:

```text
COMPASS_ENABLE 0
```

This is reasonable for bench testing, manual driving, and early GPS/telemetry bring-up. Add compass later only if low-speed or stopped heading is needed and the module can be mounted away from magnetic interference.

For higher quality rover navigation, GPS-for-yaw with dual GNSS antennas/modules or wheel encoders can be better than relying on a cheap magnetometer.

## Duff Rover Build Verification

The Duff board defaults file is embedded by waf during the Linux board build.
Duff now defaults to a Raspberry Pi compatible 32-bit ARMHF Linux toolchain:

```text
env TOOLCHAIN arm-linux-gnueabihf
```

The dev container installs the ARMHF cross compiler and the normal
`pkg-config`/`pkgconf` tooling:

```text
g++-arm-linux-gnueabihf
pkg-config
```

Waf looks for a toolchain-prefixed pkg-config executable named
`arm-linux-gnueabihf-pkg-config` when `TOOLCHAIN=arm-linux-gnueabihf`.
Older Ubuntu releases packaged this as `pkg-config-arm-linux-gnueabihf`, but
the Debian Bookworm/Trixie devcontainer base does not provide that package
name. The devcontainer Dockerfile therefore creates
`/usr/local/bin/arm-linux-gnueabihf-pkg-config` as a small wrapper around the
normal `pkg-config`. This is intentional and is the portable fix for the Duff
cross-build container.

It also enables Debian multi-arch support with `dpkg --add-architecture armhf`,
which lets apt install ARMHF packages when cross-build dependencies need target
headers or libraries.

Default Duff build:

```sh
./waf configure --board duff
./waf rover --board duff
```

Override to a native 64-bit build from the CLI:

```sh
./waf configure --board duff --toolchain native
./waf rover --board duff
```

The build output included:

```text
Embedding file defaults.parm:libraries/AP_HAL_Linux/boards/duff/defaults.parm
```

The Rover build completed successfully and produced:

```text
build/duff/bin/ardurover
```

The build summary from this run was:

```text
Target         Text (B)  Data (B)  BSS (B)  Total Flash Used (B)
bin/ardurover   2664840    133484    71560               2798324
```

## GPS, IMU, Compass, And Barometer Hardware

For Duff Rover, these sensors serve different jobs:

```text
GPS        = outdoor position, ground speed, course while moving
IMU        = accelerometer + gyro for attitude and motion sensing
Compass    = magnetic heading, useful when stopped or moving slowly
Barometer  = air-pressure altitude, usually optional for Rover
```

An M10 GPS fits well with an MPU6500 IMU. The M10 provides position, while the MPU6500 provides accelerometer and gyro data. The MPU6500 does not include a compass.

Compass can be standalone or integrated into another module:

```text
GPS + compass module:
  M10Q-5883, M10-5883, M8N-5883, M9N-5883, Here3/Here4, many Holybro GPS modules

Standalone compass:
  HMC5883L, QMC5883L, IST8310, RM3100

IMU with compass:
  MPU9250, ICM20948

IMU without compass:
  MPU6500
```

In ArduPilot-style vehicles, the compass is often integrated into the GPS module because the GPS is normally mounted high and away from motors, ESCs, batteries, and high-current wiring. That is also a better location for a magnetometer.

For Duff, a practical sensor progression is:

```text
1. M10 GPS over UART
2. MPU6500 IMU over SPI
3. Keep compass disabled initially
4. If heading while stopped/slow is needed, use GPS with compass or standalone compass
5. If magnetic interference is a problem, prefer GPS-for-yaw or wheel encoders
```

Barometer is usually not needed for a ground rover. It can help with altitude logging or terrain/grade estimation, but it is not required for normal outdoor waypoint driving. Duff currently allows startup without a barometer:

```text
define HAL_BARO_ALLOW_INIT_NO_BARO 1
```

Standalone barometers such as BMP280, MS5611, or DPS310 can be added later over I2C/SPI, but they are not a priority for this rover MVP.

## Raspberry Pi GPIO

Duff does not enable `HAL_LINUX_GPIO_RPI_ENABLED` for this MVP.

That GPIO driver opens `/dev/mem` during startup on Raspberry Pi boards. Running `ardurover` without the required privileges can fail with:

```text
RPI 4
Can't open /dev/mem
```

The PCA9685 + L298N output path only needs I2C, not direct Raspberry Pi GPIO. Leaving RPI GPIO disabled lets the HAL use the empty GPIO backend and avoids the `/dev/mem` requirement for this MVP.

## Raspberry Pi I2C Bring-Up

Duff's PCA9685 output board is expected on Raspberry Pi I2C bus 1 at address
`0x40`. Before testing `RCOutput_Duff`, confirm Linux exposes the I2C device and
the PCA9685 answers on the bus.

Check for I2C device nodes:

```sh
ls -l /dev/i2c-*
```

List the I2C adapters Linux actually exposes:

```sh
i2cdetect -l
```

For Duff's normal PCA9685 wiring, use the Raspberry Pi 40-pin header I2C bus:

```text
GPIO2 = SDA1
GPIO3 = SCL1
/dev/i2c-1
```

A normal listing includes:

```text
i2c-1  i2c  bcm2835 (i2c@7e804000)  I2C adapter
```

Some Raspberry Pi kernels also expose extra I2C adapters, for example:

```text
i2c-20  i2c  fef04500.i2c  I2C adapter
i2c-21  i2c  fef09500.i2c  I2C adapter
```

Do not assume those extra adapters are connected to the normal GPIO header.
`i2c-20` and `i2c-21` are not the standard exposed PCA9685 pins in the Duff
wiring. They are useful only if a device-tree overlay intentionally maps that
controller to physical pins and the hardware is wired there.

Normally, do not disable `i2c-20` or `i2c-21` just because they appear in
`i2cdetect -l`. Extra adapters do not interfere with Duff as long as
`RCOutput_Duff` uses the correct header bus, `/dev/i2c-1`. Also do not run:

```sh
sudo raspi-config nonint do_i2c 1
```

That disables Raspberry Pi I2C globally and can remove the `/dev/i2c-1` bus
needed by the PCA9685.

If an extra bus is causing a real problem, first identify which overlay or
device-tree setting created it:

```sh
grep -nEi 'i2c|dtparam|dtoverlay' /boot/firmware/config.txt /boot/config.txt 2>/dev/null
dtoverlay -l
ls -l /proc/device-tree/aliases | grep i2c
```

Then disable only the specific unneeded overlay and keep the normal header I2C
setting enabled, usually:

```text
dtparam=i2c_arm=on
```

If no `/dev/i2c-*` devices are present, or the expected adapter is missing,
check the Raspberry Pi I2C setting non-interactively:

```sh
sudo raspi-config nonint get_i2c
```

The return value is:

```text
0 = I2C enabled
1 = I2C disabled
```

Enable I2C non-interactively with:

```sh
sudo raspi-config nonint do_i2c 0
```

Then reboot the Raspberry Pi.

Alternatively, enable I2C from the interactive menu:

```sh
sudo raspi-config
```

Use:

```text
Interface Options -> I2C -> Enable
```

Then reboot the Raspberry Pi.

Install the I2C tools if needed:

```sh
sudo apt-get update
sudo apt-get install -y i2c-tools
```

Scan the Raspberry Pi header I2C bus connected to the PCA9685:

```sh
i2cdetect -y 1
```

The PCA9685 should appear at `0x40`, shown as `40` in the scan table. If no
device appears at `0x40` on bus 1, check:

- PCA9685 power and ground
- SDA/SCL wiring to the Raspberry Pi I2C pins
- Whether the PCA9685 address jumpers changed the address
- Whether I2C is enabled after reboot
- Whether another device is holding the bus low

A healthy scan normally shows mostly `--`, with only real responding device
addresses populated. If nearly every address appears, for example `08` through
`4f` and `60` through `77`, do not treat that as many devices being present.
That usually means the I2C bus is electrically bad, commonly because SDA is held
low, SDA/SCL are shorted or swapped, pull-ups are missing, the device is
unpowered, or a device is dragging the bus. Fix wiring/power before trusting the
scan.

If `i2cdetect` works with `sudo` but `ardurover` cannot open the I2C device as a
normal user, check permissions:

```sh
groups
ls -l /dev/i2c-1
```

The runtime user should normally be in the `i2c` group. Add it if needed:

```sh
sudo usermod -aG i2c "$USER"
```

Log out and back in, or reboot, before retesting group permissions.

## RC Input For FS-iA6B

Duff should use serial RC input for the FlySky FS-iA6B receiver.

The preferred receiver output is i-BUS, not individual PWM channels. The
receiver provides pilot input only; ArduPilot remains in the control path and
continues to drive the PCA9685 + L298N skid-steering outputs through
`RCOutput_Duff`.

Conceptually, `RC_Channels` is the input-side counterpart to `SRV_Channels`:

```text
Input side:
receiver bytes / pulses
  -> AP_RCProtocol / HAL RCInput
  -> RC_Channels / RC_Channel
  -> vehicle mode logic

Output side:
vehicle mode / controllers
  -> SRV_Channels / SRV_Channel
  -> HAL RCOutput
  -> servo/motor hardware
```

`RC_Channels` answers what the pilot commanded after input calibration,
channel mapping, deadzones, scaling, overrides, and auxiliary switch handling.
`SRV_Channels` answers what each output pin/function should emit after mode
logic, motor mixing, output limits, output scaling, and servo function mapping.

For Duff throttle:

```text
FS-iA6B i-BUS
  -> AP_RCProtocol_IBUS
  -> RC_Channels
  -> ModeManual / AP_MotorsUGV
  -> SRV_Channels
  -> RCOutput_Duff
  -> PCA9685 + L298N
```

Wiring:

```text
FS-iA6B i-BUS/SERVO signal -> Raspberry Pi UART RX, usually GPIO15 / /dev/serial0
FS-iA6B GND                -> Raspberry Pi / Duff ground
FS-iA6B +5V                -> 5V receiver supply
```

Do not wire FS-iA6B PWM channel outputs directly to the L298N or PCA9685 for
normal Duff Rover control. That would bypass ArduPilot's mode handling,
failsafe handling, logging, and future autonomous control.

The right Linux HAL input backend for Duff is `RCInput_RCProtocol`, using an
ArduPilot serial port configured as RC input:

```text
/dev/serial0
  -> HAL serial port selected by --serial1 /dev/serial0
  -> SERIAL1_PROTOCOL=23
  -> AP_SerialManager calls AP::RC().add_uart()
  -> AP_RCProtocol decodes i-BUS
  -> hal.rcin exposes RC channels to Rover
  -> Rover::read_radio()
  -> SERVO1/SERVO3 skid outputs through RCOutput_Duff
```

Required source changes:

```text
libraries/AP_HAL_Linux/RCInput_RCProtocol.h
libraries/AP_HAL_Linux/RCInput_RCProtocol.cpp
```

Add `HAL_BOARD_SUBTYPE_LINUX_DUFF` to the preprocessor lists that enable
`RCInput_RCProtocol`.

```text
libraries/AP_HAL_Linux/HAL_Linux_Class.cpp
```

Add Duff to the Linux board group that uses:

```cpp
static RCInput_RCProtocol rcinDriver{nullptr, nullptr};
```

The `nullptr, nullptr` form is intentional. It avoids hardcoding a receiver
device path in the HAL board subtype and lets normal ArduPilot serial-port
configuration provide the UART through `SERIALx_PROTOCOL=23`.

Do not use `RCInput_RPI` for Duff. That path samples Raspberry Pi GPIO using
the RPI GPIO/DMA infrastructure and can reintroduce `/dev/mem` privilege
requirements. Duff only needs ordinary UART serial input for i-BUS.

Suggested Duff RC defaults once physical RC is added:

```text
SERIAL1_PROTOCOL 23
RC_PROTOCOLS 1
MODE_CH 5
MODE1 0
MODE6 4
```

`SERIAL1_PROTOCOL 23` makes `SERIAL1` an RC input port. `RC_PROTOCOLS 1` keeps
ArduPilot's normal RC protocol auto-detection enabled. The FS-iA6B i-BUS stream
should be detected as `IBUS` at 115200 8N1. If protocol auto-detection causes a
problem during bring-up, use IBUS-only:

```text
RC_PROTOCOLS 4
```

`MODE_CH 5` assumes the FlySky transmitter's mode switch is on channel 5. Use
the actual channel observed in the GCS radio calibration page if different. A
minimal safe first setup is Manual on the low switch position and Hold on the
high switch position:

```text
MODE1 0   # Manual
MODE6 4   # Hold
```

Runtime example with MAVLink on UDP and FS-iA6B i-BUS on the Pi UART:

```sh
./ardurover --serial0 udp:192.168.1.50:14550 --serial1 /dev/serial0
```

### FS-iA6B To Raspberry Pi UART Wiring

Use the FS-iA6B `i-BUS` output, not the six individual PWM channel pins.
Connect only the one serial signal into the Raspberry Pi UART RX pin.

Basic wiring:

```text
FS-iA6B +5V           -> 5V BEC or Pi 5V rail, if the power budget allows it
FS-iA6B GND           -> Raspberry Pi GND
FS-iA6B i-BUS signal  -> Raspberry Pi GPIO15 / RXD0 / physical pin 10
```

The Raspberry Pi GPIO pins are 3.3 V inputs and are not 5 V tolerant. The
FS-iA6B can be powered from 5 V, but the official receiver documentation does
not clearly specify the i-BUS signal voltage. Treat the i-BUS signal as
potentially 5 V unless it has been measured on the actual receiver.

If the i-BUS signal is 5 V, do not connect it directly to GPIO15. Use either a
logic level shifter or a simple two-resistor voltage divider. For one-way
receiver-to-Pi i-BUS, the divider is enough. Prefer `2k/3.3k` because it gives
some margin below the Pi's 3.3 V GPIO limit while still producing a valid UART
logic-high level:

```text
FS-iA6B i-BUS signal
        |
       2k
        |
        +----> Raspberry Pi GPIO15 / RXD0 / physical pin 10
        |
      3.3k
        |
       GND
```

This converts a 5 V signal to about 3.1 V:

```text
5 V * 3.3k / (2k + 3.3k) = 3.11 V
```

Other resistor pairs such as `1k/2k`, `1k/2.2k`, `4.7k/10k`, or `10k/20k` are
also reasonable. The receiver ground and Raspberry Pi ground must be common,
even if the receiver is powered from a separate BEC.

On Raspberry Pi OS, enable the hardware UART and disable the Linux login console
on the UART used for i-BUS. Otherwise the OS and ArduPilot can both try to use
`/dev/serial0`.

Keep `FS_THR_ENABLE 0` during no-RC bench bring-up. After the FS-iA6B is wired,
detected, calibrated, and its failsafe behavior is verified with wheels off the
ground, revisit the throttle/radio failsafe configuration and re-enable it for
real vehicle testing.

### Testing RC Input With The Example Sketch

ArduPilot's example sketches can be built for the Duff board. The RC input
example is:

```text
libraries/AP_HAL/examples/RCInput/RCInput.cpp
```

Build only that example with:

```sh
./waf configure --board duff
./waf --targets examples/RCInput
```

The resulting binary is:

```sh
build/duff/examples/RCInput
```

Run it on the Raspberry Pi with:

```sh
build/duff/examples/RCInput
```

If the RC input backend has active channels, the output looks like:

```text
Starting RCInput test
 1:1500  2:1500  3:1000  4:1500 ...
```

Moving transmitter sticks should change the printed channel values.

Duff caveat: the current Duff Linux HAL uses:

```cpp
static RCInput_RCProtocol rcinDriver{nullptr, nullptr};
```

This is intentional for normal Rover runtime because the receiver UART is
provided by ArduPilot serial-port configuration:

```text
--serial1 /dev/serial0
SERIAL1_PROTOCOL 23
```

The standalone `RCInput` example does not load vehicle parameters such as
`SERIAL1_PROTOCOL=23` in the same way as a full Rover run. Therefore, on Duff it
may print:

```text
SBUS FD -1  115200 FD -1
No channels detected
```

That result means the standalone example did not attach a UART to
`AP_RCProtocol`; it does not by itself prove the receiver or wiring is bad.

For Duff, the more representative RC input test is to run Rover and pass the
receiver UART as `SERIAL1`:

```sh
./waf configure --board duff
./waf rover --board duff
build/duff/bin/ardurover --serial0 udp:192.168.1.50:14550 --serial1 /dev/serial0
```

Then check radio input in QGroundControl, Mission Planner, or MAVProxy while
moving the transmitter sticks. This exercises the intended path:

```text
/dev/serial0
  -> SERIAL1
  -> SERIAL1_PROTOCOL=23
  -> AP::RC().add_uart()
  -> AP_RCProtocol_IBUS
  -> hal.rcin
  -> Rover radio input
```

### RCOutput Example And The RCInput Thread

The standalone `RCOutput` example is useful for testing `RCOutput_Duff`, but it
still runs inside the full Duff Linux HAL. The example source only calls
`hal.rcout`, but the Linux HAL also constructs and starts the normal scheduler
threads, including the RC input thread:

```text
ap-rcin
```

That is why an RC output-only test can still print:

```text
SBUS FD -1  115200 FD -1
```

Those lines come from `RCInput_RCProtocol::init()`, not from `RCOutput_Duff`.

Observed failure before the guard:

```text
Thread "ap-rcin" received signal SIGSEGV
AP_RCProtocol_CRSF::update()
AP_RCProtocol::new_input()
Linux::RCInput_RCProtocol::_timer_tick()
```

The root cause was not PCA9685, I2C, sudo/root, or `RCOutput_Duff`. The crash
was caused by the Duff RC input backend polling `AP::RC().new_input()` even
though the standalone example had no RC input source:

```text
fd_inverted = -1
fd_115200 = -1
AP::RC().has_uart() = false
```

In full Rover runtime this is different. `ardurover` can attach the receiver
UART through normal serial configuration:

```sh
build/duff/bin/ardurover --serial1 /dev/serial0
```

with:

```text
SERIAL1_PROTOCOL 23
```

That path registers a UART with `AP_RCProtocol`, so the RC input backend has a
real input source.

The fix is for `RCInput_RCProtocol::_timer_tick()` to return early when there is
no directly opened RC fd and no `SERIALx_PROTOCOL=23` UART registered:

```cpp
have_input_source |= AP::RC().has_uart();
if (!have_input_source) {
    return;
}
```

This lets `examples/RCOutput` test `RCOutput_Duff` without unrelated RC input
protocol polling, while preserving normal Rover RC input behavior.

### Checking `/dev/serial0` On Raspberry Pi

Before relying on FS-iA6B i-BUS input, verify the Raspberry Pi UART in layers:
device mapping, OS console ownership, loopback, then receiver bytes.

Check that the UART device exists:

```sh
ls -l /dev/ttyAMA0 /dev/serial0
readlink -f /dev/serial0
```

If `/dev/serial0` points to `/dev/ttyAMA0` or `/dev/ttyS0`, either the symlink
or the underlying device name can be used. Using
`/dev/serial0` can be more portable across Raspberry Pi models and OS UART
mapping changes.

On the observed Duff Raspberry Pi setup:

```text
/dev/serial0 -> ttyS0
```

So use either `/dev/serial0` or `/dev/ttyS0` for ArduPilot. Prefer
`/dev/serial0` unless there is a specific reason to pin the exact UART device.

Raspberry Pi UART naming:

```text
/dev/ttyAMA0 = AMBA/PL011 UART
/dev/ttyS0   = mini-UART / 8250-style serial port
/dev/serial0 = Raspberry Pi OS symlink to the UART currently mapped to GPIO serial pins
```

`AMA` in `ttyAMA0` comes from ARM AMBA, meaning Advanced Microcontroller Bus
Architecture. PL011 is ARM's PrimeCell UART peripheral. In practical terms,
`ttyAMA0` usually refers to the more capable PL011 UART, while `ttyS0` often
refers to the simpler Raspberry Pi mini-UART.

The UART is not part of the ARM CPU core itself. It is a peripheral block in the
Raspberry Pi SoC. The GPIO pin mux connects that SoC UART peripheral to physical
header pins, typically GPIO14/GPIO15 for TX/RX:

```text
Raspberry Pi SoC
  -> ARM CPU cores
  -> UART peripheral
  -> GPIO pin mux
  -> physical header pins GPIO14/GPIO15
```

For Duff, this means the receiver connects to the GPIO header pins, but
ArduPilot reads the data through the Linux serial device, normally
`/dev/serial0`.

Make sure Linux is not using the same UART as a login console:

```sh
cat /boot/firmware/cmdline.txt
```

Look for entries such as:

```text
console=serial0,115200
console=ttyAMA0,115200
```

If a serial console is enabled on the receiver UART, disable it with:

```sh
sudo raspi-config
```

Use:

```text
Interface Options -> Serial Port
Login shell over serial? No
Enable serial hardware? Yes
```

Reboot after changing serial settings.

For a basic UART loopback test, disconnect the receiver and temporarily connect:

```text
GPIO14 TXD -> GPIO15 RXD
```

Then run in one SSH session:

```sh
stty -F /dev/serial0 115200 raw -echo
cat /dev/serial0
```

In another SSH session:

```sh
echo test > /dev/serial0
```

If the first session prints `test`, the UART works. Remove the TX/RX jumper
after the test.

To check FS-iA6B i-BUS bytes, wire the receiver:

```text
FS-iA6B i-BUS signal -> Pi RX GPIO15
FS-iA6B GND          -> Pi GND
FS-iA6B +5V          -> 5V receiver supply
```

Then inspect the serial stream:

```sh
stty -F /dev/serial0 115200 raw -echo
timeout 3 cat /dev/serial0 | hexdump -C
```

A working i-BUS stream should show repeating binary data. i-BUS frames commonly
start with:

```text
20 40
```

The data will not be readable text. It should appear as changing hex bytes.

Important: Raspberry Pi UART RX is 3.3 V only. Confirm the FS-iA6B i-BUS signal
level is safe for the Pi before connecting it directly.

## Linux Storage Directory

Duff keeps ArduPilot's default Linux storage directory:

```text
/var/lib/ardupilot
```

This is where the Linux HAL stores persistent parameter data, such as the `ardurover.stg` storage file. If the directory does not exist on the Raspberry Pi, startup can fail with:

```text
Failed to open storage directory: /var/lib/ardupilot (No such file or directory)
Cannot create storage /var/lib/ardupilot (No such file or directory)
```

Create the directory once on the vehicle and give the runtime user write access:

```sh
sudo mkdir -p /var/lib/ardupilot
sudo chown "$USER":"$USER" /var/lib/ardupilot
```

Alternatively, for a one-off test without changing system folders, run `ardurover` with `--storage-directory` or `-s` and point it at an existing writable directory.

## Runtime CLI Arguments

Linux `ardurover` supports command-line arguments. Use:

```sh
./ardurover --help
```

The standalone `examples/RCOutput` binary uses the same Linux HAL argument
parser, so its `./RCOutput --help` output is also useful when bringing up Duff.
These arguments tell the Linux HAL what device or network endpoint to use for
each ArduPilot serial port before the vehicle or example starts.

Relevant source files:

- `ardupilot/libraries/AP_HAL_Linux/HAL_Linux_Class.cpp`
- `ardupilot/libraries/AP_HAL_Linux/UARTDriver.cpp`

### Serial Port Selection

`--serialN` maps directly to ArduPilot `SERIALN`, where `N` is `0` through `9`.
The path can be a Linux serial device or a network endpoint:

```text
/dev/ttyXXX
tcp:IP:PORT[:wait]
udp:IP:PORT[:bcast]
udpin:IP:PORT
```

Examples:

```sh
./ardurover --serial0 /dev/ttyAMA0
./ardurover --serial1 /dev/serial0
./ardurover --serial3 /dev/ttyS1

./RCOutput --serial0 /dev/ttyAMA0
./RCOutput --serial1 /dev/ttyUSB0
./RCOutput --serial3 /dev/ttyS1
```

Legacy UART aliases still work, but are deprecated:

```text
-A / --uartA -> SERIAL0
-C / --uartC -> SERIAL1
-D / --uartD -> SERIAL2
-B / --uartB -> SERIAL3
-E / --uartE -> SERIAL4
-F / --uartF -> SERIAL5
-G / --uartG -> SERIAL6
-H / --uartH -> SERIAL7
-I / --uartI -> SERIAL8
-J / --uartJ -> SERIAL9
```

For example:

```sh
./RCOutput -A /dev/ttyAMA0
```

is equivalent to:

```sh
./RCOutput --serial0 /dev/ttyAMA0
```

### TCP And UDP Serial Endpoints

A TCP endpoint creates a TCP server bound to the given address and port:

```sh
./ardurover --serial0 tcp:0.0.0.0:5760:wait
./RCOutput --serial1 tcp:192.168.2.15:1243:wait
```

The optional `:wait` flag means the program waits until a TCP client connects.

A UDP output endpoint sends packets to a remote address:

```sh
./ardurover --serial0 udp:192.168.1.50:14550
./RCOutput --serial0 udp:11.0.0.2:14550
```

Broadcast UDP adds `:bcast`:

```sh
./RCOutput --serial0 udp:11.0.0.255:14550:bcast
```

A UDP input endpoint listens for incoming UDP packets on the selected port:

```sh
./ardurover --serial0 udpin:0.0.0.0:14550
./RCOutput --serial0 udpin:0.0.0.0:14550
```

`udpin` cannot be combined with `:bcast`.

### Runtime Paths

Set the parameter/storage directory:

```sh
./ardurover --storage-directory /var/lib/ardupilot
./ardurover -s /var/lib/ardupilot

./RCOutput --storage-directory /var/APM/storage
./RCOutput -s /var/APM/storage
```

Set the log directory:

```sh
./ardurover --log-directory /var/log/ardupilot
./ardurover -l /var/log/ardupilot

./RCOutput --log-directory /var/APM/logs
./RCOutput -l /var/APM/logs
```

Set the terrain data directory:

```sh
./ardurover --terrain-directory /var/lib/ardupilot/terrain
./ardurover -t /var/lib/ardupilot/terrain

./RCOutput --terrain-directory /var/APM/terrain
./RCOutput -t /var/APM/terrain
```

Set the defaults file:

```sh
./ardurover --defaults /path/to/defaults.parm
```

Set the directory for loadable ArduPilot modules, if module support was built
in:

```sh
./ardurover --module-directory /usr/lib/ardupilot/modules
./ardurover -M /usr/lib/ardupilot/modules

./RCOutput --module-directory /usr/lib/ardupilot/modules
./RCOutput -M /usr/lib/ardupilot/modules
```

### CPU Affinity

`--cpu-affinity` pins the process or threads to specific CPU cores. This can be
useful on Linux boards when more deterministic timing is needed:

```sh
./ardurover --cpu-affinity 1
./ardurover --cpu-affinity 1,3
./ardurover --cpu-affinity 1-3
./ardurover -c 1

./RCOutput --cpu-affinity 1
./RCOutput --cpu-affinity 1,3
./RCOutput --cpu-affinity 1-3
./RCOutput -c 1
```

## Stopping ArduPilot On Linux

When `ardurover` is running in the foreground, `Ctrl+C` sends `SIGINT` and is the normal way to stop the Linux process.

For hardware testing, do not treat `Ctrl+C` as the only motor safety action. The safer bench sequence is:

```text
1. Disarm from the GCS or MAVProxy.
2. Confirm the motors are stopped.
3. Press Ctrl+C to stop the Linux process.
4. Remove motor power before touching the vehicle.
```

Duff now also attempts to stop the PCA9685 outputs when `RCOutput_Duff` is destroyed. The shutdown path:

- Marks safety on
- Disables both Duff motors internally
- Stages left and right motor outputs to zero
- Flushes PCA9685 channels 0 through 5 off
- Sets the PCA9685 `ALL_LED_OFF` shutdown bit as a final all-output-off command

This reduces the risk of the PCA9685 keeping the last commanded motor state after process exit. It is still a software safety layer, not a substitute for disarming and removing motor power during bench work.

## GCS Connection On Linux

In a typical rover/copter setup, the GCS connects to ArduPilot over MAVLink through a UART, USB serial adapter, telemetry radio, or similar serial transport.

On Linux, ArduPilot serial ports are still exposed as `SERIAL0`, `SERIAL1`, and so on, but the backing transport can be a network endpoint instead of a physical UART. The GCS still speaks MAVLink; only the transport changes.

For the Duff Raspberry Pi setup, this means `SERIAL0` can be routed over UDP or TCP and Mission Planner, QGroundControl, or MAVProxy can connect over Wi-Fi/Ethernet.

Examples:

```sh
./ardurover --serial0 udp:192.168.1.50:14550
./ardurover --serial0 udpin:0.0.0.0:14550
./ardurover --serial0 tcp:0.0.0.0:5760:wait
```

This is normally cleaner than leaving MAVLink on the process terminal because it avoids mixing binary MAVLink bytes with human-readable console/debug output.

Common useful arguments:

```sh
./ardurover --serial0 udp:192.168.1.50:14550
./ardurover --serial0 udpin:0.0.0.0:14550
./ardurover --serial0 tcp:0.0.0.0:5760:wait
./ardurover --serial1 /dev/serial0
```

Storage and file path overrides:

```sh
./ardurover --storage-directory /var/lib/ardupilot
./ardurover --log-directory /var/log/ardupilot
./ardurover --terrain-directory /var/lib/ardupilot/terrain
./ardurover --defaults /path/to/defaults.parm
```

Short forms:

```sh
./ardurover -s /var/lib/ardupilot
./ardurover -l /var/log/ardupilot
./ardurover -t /var/lib/ardupilot/terrain
```

The strange non-text characters seen in terminal output are likely binary MAVLink or log bytes sharing the same console stream as human-readable text. Linux `hal.console` uses the default console device, so `hal.console->printf(...)` messages can be mixed with binary `SERIAL0` output when `SERIAL0` is left on the terminal.

Duff RC output status messages now go to `stderr` with a local `duff_log(...)` helper instead of `hal.console->printf(...)`. This keeps messages such as `RCOutput_Duff: ...` separate from the binary stream:

```sh
./ardurover >mavlink.bin 2>duff-console.log
```

To watch the Duff messages live:

```sh
./ardurover 2>&1 >mavlink.bin | sed -n '/RCOutput_Duff:/p'
```

Routing `SERIAL0` to UDP or TCP is still the cleaner normal runtime setup because it keeps telemetry off the terminal entirely:

```sh
./ardurover --serial0 udp:192.168.1.50:14550
```

Then connect Mission Planner, QGroundControl, or MAVProxy to that endpoint.

## Why Common Code Was Touched

The preferred minimal shape would have been changing only:

- `hwdef.dat`
- `defaults.parm`

That is not sufficient with the current common code because the mock inertial backend was hardcoded for ESP32 only.

The common-code change is intentionally small:

- Introduce `AP_INERTIALSENSOR_NONE_BACKEND_ENABLED`
- Keep the existing ESP32 default behavior
- Let Duff opt into the already-existing mock backend from board config

This avoids copying sensor code into the Duff board and keeps the no-IMU behavior explicit.

## Mock IMU Behavior

Duff's no-IMU MVP uses ArduPilot's `AP_InertialSensor_NONE` backend.

The board config selects no physical INS driver:

```text
define HAL_INS_DEFAULT HAL_INS_NONE
```

Then Duff explicitly enables the fallback mock backend:

```text
define AP_INERTIALSENSOR_NONE_BACKEND_ENABLED 1
```

At startup, `AP_InertialSensor.cpp` sees that no real INS backend was added. Because the mock backend is enabled, it adds:

```cpp
AP_InertialSensor_NONE::detect(*this, INS_NONE_SENSOR_A)
```

`AP_InertialSensor_NONE` then:

- Registers one fake gyro and one fake accel
- Uses SITL-style synthetic device IDs
- Registers a scheduler timer process
- Generates fake accel samples at `1000 Hz`
- Generates fake gyro samples at `1000 Hz`
- Publishes those samples through the normal inertial sensor backend APIs

The fake accel values are essentially tiny constant values:

```cpp
xAccel = 0.01;
yAccel = 0.01;
zAccel = 0.01;
```

The fake gyro values are tiny angular rates with small drift/noise:

```cpp
p = radians(0.01) + gyro_drift();
q = radians(0.01) + gyro_drift();
r = radians(0.01) + gyro_drift();
```

The backend publishes samples with the normal hooks:

```cpp
_notify_new_accel_raw_sample(...)
_notify_new_gyro_raw_sample(...)
```

This is why the GCS can show messages such as:

```text
EKF3 IMU0 initialised
ArduPilot Ready
```

The mock IMU still goes through normal ArduPilot calibration checks, so Duff defaults seed the synthetic gyro/accel IDs and nonzero accel calibration values:

```text
INS_GYR_ID 2752772
INS_ACC_ID 2753028
INS_ACCOFFS_X 0.001
INS_ACCOFFS_Y 0.001
INS_ACCOFFS_Z 0.001
INS_ACCSCAL_X 1.001
INS_ACCSCAL_Y 1.001
INS_ACCSCAL_Z 1.001
```

This mock IMU is only for boot, GCS connection, and bench motor testing. It does not measure gravity correctly, vehicle tilt, vibration, or real motion, so it is not suitable for navigation or closed-loop vehicle behavior.

## Migrating To A Real IMU

When a real IMU is added, remove the mock-IMU path and configure the actual sensor backend.

Current Duff mock setup:

```text
define HAL_INS_DEFAULT HAL_INS_NONE
define AP_INERTIALSENSOR_NONE_BACKEND_ENABLED 1
```

For an MPU6500-style setup, the target direction is:

```text
# remove the mock INS lines above
define HAL_INS_DEFAULT HAL_INS_MPU6500
```

The exact `hwdef.dat` lines depend on the physical wiring:

- SPI bus number
- chip-select GPIO
- SPI mode
- SPI speed
- device name expected by the MPU6500 backend

For Raspberry Pi, prefer MPU6500 over SPI rather than I2C. SPI is better for IMU timing and throughput.

Remove the mock calibration defaults from `libraries/AP_HAL_Linux/boards/duff/defaults.parm`:

```text
INS_GYR_ID 2752772
INS_ACC_ID 2753028
INS_ACCOFFS_X 0.001
INS_ACCOFFS_Y 0.001
INS_ACCOFFS_Z 0.001
INS_ACCSCAL_X 1.001
INS_ACCSCAL_Y 1.001
INS_ACCSCAL_Z 1.001
```

Then revisit MVP-only defaults:

```text
ARMING_SKIPCHK -1
INS_GYR_CAL 0
COMPASS_ENABLE 0
GPS_TYPE 0
FS_THR_ENABLE 0
```

For a real vehicle, normal arming and sensor checks should be re-enabled gradually, starting with:

```text
ARMING_SKIPCHK 0
```

Because Duff keeps the default Linux storage directory, old mock parameters may remain in:

```text
/var/lib/ardupilot/ardurover.stg
```

After switching to a real IMU, either reset parameters from the GCS/MAVProxy or intentionally remove/reset the old storage file before first real-sensor bring-up.

Build after changing `hwdef.dat`:

```sh
./waf configure --board duff
./waf rover --board duff
```

On first boot with the real IMU:

```text
1. Confirm the IMU backend is detected.
2. Calibrate accelerometer.
3. Calibrate gyro.
4. Set board orientation if needed.
5. Check IMU health in GCS.
6. Confirm there is no "INS unable to initialise" or "3D Accel calibration needed" message.
```

## Verification

Configure verification:

```text
./waf configure --board duff
```

This confirms waf can load the Duff board definition and generate the build-time hardware header:

```text
build/duff/hwdef.h
```

Important configure output:

- `Setting board to : duff` means the Duff board profile was selected.
- `Processing ... hwdef/duff/hwdef.dat` means Duff's hardware definition was read.
- `Writing hwdef setup in ... build/duff/hwdef.h` means the hwdef was converted into C/C++ compile-time defines.
- `Using toolchain : arm-linux-gnueabihf` means Duff is building as a 32-bit ARMHF Linux binary.
- `CXX Compiler : arm-linux-gnueabihf-g++ ...` confirms the ARMHF cross compiler is being used.
- `libiio : not found` is not a blocker for this MVP because the PCA9685 output path uses I2C directly.
- `Removing target_list file ... build/duff/target_list` is normal after configure; waf regenerates target metadata on the next build.

To verify the native 64-bit override, configure with:

```text
./waf configure --board duff --toolchain native
```

In that case, `Using toolchain : native` means Duff builds with the host
compiler as a native 64-bit Linux binary on a 64-bit dev container.

The successful configure result was:

```text
'configure' finished successfully
```

Build verification:

```text
./waf rover --board duff
```

Result:

```text
'rover' finished successfully
```

## Current Scope

Done:

- Duff Linux board RC output backend for PCA9685 + L298N skid steering
- Real PCA9685 I2C output path
- Safety and zero-output handling
- Console bring-up messages
- Duff default motor parameters
- No-IMU MVP boot support using the mock inertial backend
- Duff Rover build verification

Not done:

- Real IMU integration
- Closed-loop navigation validation
- Physical motor direction calibration
- Runtime testing on the actual vehicle
- Re-enabling normal arming/sensor checks for production use
