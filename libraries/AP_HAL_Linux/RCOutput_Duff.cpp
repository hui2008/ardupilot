#include "RCOutput_Duff.h"

#include <AP_HAL/AP_HAL.h>
#include <AP_Math/AP_Math.h>

#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <stdint.h>

extern const AP_HAL::HAL& hal;

using namespace Linux;

#define PCA9685_RA_MODE1           0x00
#define PCA9685_RA_MODE2           0x01
#define PCA9685_RA_LED0_ON_L       0x06
#define PCA9685_RA_ALL_LED_ON_L    0xFA
#define PCA9685_RA_ALL_LED_ON_H    0xFB
#define PCA9685_RA_ALL_LED_OFF_L   0xFC
#define PCA9685_RA_ALL_LED_OFF_H   0xFD
#define PCA9685_RA_PRE_SCALE       0xFE

#define PCA9685_MODE1_RESTART_BIT  (1 << 7)
#define PCA9685_MODE1_AI_BIT       (1 << 5)
#define PCA9685_MODE1_SLEEP_BIT    (1 << 4)
#define PCA9685_MODE2_OUTDRV_BIT   (1 << 2)
#define PCA9685_ALL_LED_OFF_H_SHUT (1 << 4)

#define PCA9685_LED_ON_H_ALWAYS_ON_BIT   (1 << 4)
#define PCA9685_LED_OFF_H_ALWAYS_OFF_BIT (1 << 4)

static constexpr float PCA9685_INTERNAL_CLOCK = 1.04f * 25000000.0f;
static constexpr uint16_t PCA9685_FULL_ON = 4096;

static void duff_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fflush(stderr);
}

RCOutput_Duff::~RCOutput_Duff()
{
    shutdown_outputs();
    delete _dev;
}

void RCOutput_Duff::init()
{
    if (_dev == nullptr) {
        _dev = hal.i2c_mgr->get_device_ptr(PCA9685_BUS, PCA9685_ADDRESS);
    }

    if (_dev == nullptr) {
        duff_log("RCOutput_Duff: failed to open PCA9685 bus=%u addr=0x%02x\n",
                 unsigned(PCA9685_BUS), unsigned(PCA9685_ADDRESS));
        return;
    }

    _dev->set_retries(2);

    if (!_dev->get_semaphore()->take(10)) {
        duff_log("RCOutput_Duff: failed to lock PCA9685\n");
        return;
    }

    const bool ok =
        write_register(PCA9685_RA_ALL_LED_OFF_H, PCA9685_ALL_LED_OFF_H_SHUT) &&
        write_register(PCA9685_RA_MODE1, PCA9685_MODE1_SLEEP_BIT) &&
        write_register(PCA9685_RA_ALL_LED_ON_L, 0) &&
        write_register(PCA9685_RA_ALL_LED_ON_H, 0) &&
        write_register(PCA9685_RA_ALL_LED_OFF_L, 0) &&
        write_register(PCA9685_RA_ALL_LED_OFF_H, 0);

    _dev->get_semaphore()->give();

    if (!ok) {
        duff_log("RCOutput_Duff: PCA9685 init failed\n");
        return;
    }

    set_freq(0, DEFAULT_FREQ_HZ);

    if (_dev->get_semaphore()->take(10)) {
        write_register(PCA9685_RA_MODE2, PCA9685_MODE2_OUTDRV_BIT);
        _dev->get_semaphore()->give();
    }

    stage_stop(_left);
    stage_stop(_right);
    if (!flush_channel_range(0, PCA9685_USED_CHANNEL_COUNT - 1)) {
        duff_log("RCOutput_Duff: failed to stop PCA9685 outputs during init\n");
        return;
    }
    _safety_on = false;
    report_motor(_left, "init");
    report_motor(_right, "init");

    duff_log("RCOutput_Duff: PCA9685 L298N skid output ready on bus=%u addr=0x%02x\n",
             unsigned(PCA9685_BUS), unsigned(PCA9685_ADDRESS));
}

void RCOutput_Duff::set_freq(uint32_t chmask, uint16_t freq_hz)
{
    (void)chmask;

    if (_dev == nullptr) {
        return;
    }

    freq_hz = constrain_uint16(freq_hz, 50, 1600);
    const uint8_t prescale = ceilf(PCA9685_INTERNAL_CLOCK / (4096.0f * freq_hz)) - 1;

    if (!_dev->get_semaphore()->take(10)) {
        return;
    }

    const bool ok =
        write_register(PCA9685_RA_ALL_LED_OFF_H, PCA9685_ALL_LED_OFF_H_SHUT) &&
        write_register(PCA9685_RA_MODE1, PCA9685_MODE1_SLEEP_BIT) &&
        write_register(PCA9685_RA_PRE_SCALE, prescale) &&
        write_register(PCA9685_RA_MODE1, PCA9685_MODE1_RESTART_BIT | PCA9685_MODE1_AI_BIT) &&
        write_register(PCA9685_RA_ALL_LED_OFF_H, 0);

    _dev->get_semaphore()->give();

    if (ok) {
        _freq_hz = PCA9685_INTERNAL_CLOCK / (4096.0f * (prescale + 1));
        duff_log("RCOutput_Duff: freq requested=%u actual=%u prescale=%u\n",
                 unsigned(freq_hz), unsigned(_freq_hz), unsigned(prescale));
    } else {
        duff_log("RCOutput_Duff: failed to set freq=%u\n", unsigned(freq_hz));
    }
}

uint16_t RCOutput_Duff::get_freq(uint8_t ch)
{
    (void)ch;
    return _freq_hz;
}

void RCOutput_Duff::enable_ch(uint8_t ch)
{
    Motor *motor = find_motor(ch);
    if (motor == nullptr) {
        return;
    }

    motor->enabled = true;
    duff_log("RCOutput_Duff: enable %s ch=%u\n",
             motor->name, unsigned(ch));
    if (!_corked) {
        set_motor(*motor, motor->pwm_us);
    }
}

void RCOutput_Duff::disable_ch(uint8_t ch)
{
    Motor *motor = find_motor(ch);
    if (motor == nullptr) {
        return;
    }

    motor->enabled = false;
    duff_log("RCOutput_Duff: disable %s ch=%u\n",
             motor->name, unsigned(ch));
    stop_motor(*motor);
}

bool RCOutput_Duff::force_safety_on()
{
    _safety_on = true;
    stage_stop(_left);
    stage_stop(_right);
    const bool ok = flush_channel_range(0, PCA9685_USED_CHANNEL_COUNT - 1);
    duff_log("RCOutput_Duff: safety on %s\n", ok ? "ok" : "failed");
    if (ok) {
        report_motor(_left, "safety");
        report_motor(_right, "safety");
    }
    return ok;
}

void RCOutput_Duff::force_safety_off()
{
    _safety_on = false;
    bool any_motor = false;
    if (_left.enabled) {
        stage_motor(_left, _left.pwm_us);
        any_motor = true;
    }
    if (_right.enabled) {
        stage_motor(_right, _right.pwm_us);
        any_motor = true;
    }
    if (any_motor) {
        const bool ok = flush_channel_range(0, PCA9685_USED_CHANNEL_COUNT - 1);
        duff_log("RCOutput_Duff: safety off %s\n", ok ? "ok" : "failed");
        if (ok) {
            if (_left.enabled) {
                report_motor(_left, "safety");
            }
            if (_right.enabled) {
                report_motor(_right, "safety");
            }
        }
    } else {
        duff_log("RCOutput_Duff: safety off ok, no enabled motors\n");
    }
}

void RCOutput_Duff::write(uint8_t ch, uint16_t period_us)
{
    Motor *motor = find_motor(ch);
    if (motor == nullptr) {
        return;
    }

    if (period_us != 0) {
        period_us = constrain_uint16(period_us, MIN_PWM_US, MAX_PWM_US);
    }
    motor->pwm_us = period_us;
    if (_corked) {
        _pending = true;
        return;
    }
    if (!set_motor(*motor, period_us)) {
        _pending = true;
    }
}

uint16_t RCOutput_Duff::read(uint8_t ch)
{
    const Motor *motor = find_motor(ch);
    return motor != nullptr ? motor->pwm_us : DEFAULT_PWM_US;
}

void RCOutput_Duff::read(uint16_t *period_us, uint8_t len)
{
    if (period_us == nullptr) {
        return;
    }
    for (uint8_t i = 0; i < len; i++) {
        period_us[i] = read(i);
    }
}

void RCOutput_Duff::cork()
{
    _corked = true;
}

void RCOutput_Duff::push()
{
    _corked = false;
    if (!_pending) {
        return;
    }

    bool any_motor = false;
    if (_left.enabled) {
        stage_motor(_left, _left.pwm_us);
        any_motor = true;
    }
    if (_right.enabled) {
        stage_motor(_right, _right.pwm_us);
        any_motor = true;
    }

    if (!any_motor) {
        _pending = false;
        return;
    }

    if (flush_channel_range(0, PCA9685_USED_CHANNEL_COUNT - 1)) {
        _pending = false;
        if (_left.enabled) {
            report_motor(_left, "push");
        }
        if (_right.enabled) {
            report_motor(_right, "push");
        }
    }
}

bool RCOutput_Duff::write_register(uint8_t reg, uint8_t reg_value)
{
    if (_dev == nullptr) {
        return false;
    }
    return _dev->write_register(reg, reg_value);
}

bool RCOutput_Duff::set_motor(Motor &motor, uint16_t pwm_us)
{
    if (_dev == nullptr) {
        return false;
    }
    if (_safety_on || !motor.enabled || pwm_us == 0) {
        stage_stop(motor);
        return flush_motor(motor);
    }

    pwm_us = constrain_uint16(pwm_us, MIN_PWM_US, MAX_PWM_US);
    stage_motor(motor, pwm_us);
    const bool ok = flush_motor(motor);
    if (ok) {
        report_motor(motor, "write");
    }
    return ok;
}

void RCOutput_Duff::stage_motor(Motor &motor, uint16_t pwm_us)
{
    if (pwm_us == 0) {
        stage_stop(motor);
        return;
    }

    pwm_us = constrain_uint16(pwm_us, MIN_PWM_US, MAX_PWM_US);
    const int16_t centered = int16_t(pwm_us) - int16_t(DEFAULT_PWM_US);

    if (abs(centered) <= DEADZONE_US) {
        stage_stop(motor);
        return;
    }

    const bool forward = centered > 0;
    const uint16_t magnitude_us = constrain_uint16(abs(centered), 0, DEFAULT_PWM_US - MIN_PWM_US);
    const uint16_t duty_ticks = (uint32_t(magnitude_us) * (PCA9685_FULL_ON - 1)) /
                                (DEFAULT_PWM_US - MIN_PWM_US);

    _pca_ticks[motor.enable_ch] = duty_ticks;
    _pca_ticks[motor.in_a_ch] = forward ? PCA9685_FULL_ON : 0;
    _pca_ticks[motor.in_b_ch] = forward ? 0 : PCA9685_FULL_ON;
}

void RCOutput_Duff::stage_stop(Motor &motor)
{
    _pca_ticks[motor.enable_ch] = 0;
    _pca_ticks[motor.in_a_ch] = 0;
    _pca_ticks[motor.in_b_ch] = 0;
}

bool RCOutput_Duff::flush_motor(const Motor &motor)
{
    const uint8_t first_ch = MIN(motor.enable_ch, MIN(motor.in_a_ch, motor.in_b_ch));
    const uint8_t last_ch = MAX(motor.enable_ch, MAX(motor.in_a_ch, motor.in_b_ch));
    return flush_channel_range(first_ch, last_ch);
}

bool RCOutput_Duff::flush_channel_range(uint8_t first_ch, uint8_t last_ch)
{
    if (_dev == nullptr ||
        first_ch >= PCA9685_USED_CHANNEL_COUNT ||
        last_ch >= PCA9685_USED_CHANNEL_COUNT ||
        first_ch > last_ch) {
        return false;
    }

    struct PACKED pca_values {
        uint8_t reg;
        uint8_t data[PCA9685_USED_CHANNEL_COUNT * 4];
    } pca_values {};

    pca_values.reg = PCA9685_RA_LED0_ON_L + 4U * first_ch;
    for (uint8_t ch = first_ch; ch <= last_ch; ch++) {
        fill_channel_bytes(_pca_ticks[ch], &pca_values.data[(ch - first_ch) * 4]);
    }

    if (!_dev->get_semaphore()->take(10)) {
        return false;
    }

    const size_t payload_size = 1U + (last_ch - first_ch + 1U) * 4U;
    const bool ok = _dev->transfer(reinterpret_cast<uint8_t *>(&pca_values),
                                   payload_size,
                                   nullptr,
                                   0);
    _dev->get_semaphore()->give();

    return ok;
}

void RCOutput_Duff::fill_channel_bytes(uint16_t ticks, uint8_t *data)
{
    ticks = MIN(ticks, PCA9685_FULL_ON);

    data[0] = 0;
    data[1] = ticks == PCA9685_FULL_ON ? PCA9685_LED_ON_H_ALWAYS_ON_BIT : 0;
    data[2] = ticks & 0xFF;
    data[3] = ticks == PCA9685_FULL_ON ? 0 :
        (ticks == 0 ? PCA9685_LED_OFF_H_ALWAYS_OFF_BIT : ticks >> 8);
}

void RCOutput_Duff::report_motor(Motor &motor, const char *reason)
{
    if (motor.last_reported_pwm == motor.pwm_us) {
        return;
    }

    if (motor.last_reported_pwm != 0 && motor.pwm_us != 0) {
        const int16_t last_centered = int16_t(motor.last_reported_pwm) - int16_t(DEFAULT_PWM_US);
        const int16_t current_centered = int16_t(motor.pwm_us) - int16_t(DEFAULT_PWM_US);
        const int8_t last_state = abs(last_centered) <= DEADZONE_US ? 0 : (last_centered > 0 ? 1 : -1);
        const int8_t current_state = abs(current_centered) <= DEADZONE_US ? 0 : (current_centered > 0 ? 1 : -1);

        if (last_state == current_state &&
            abs(int16_t(motor.pwm_us) - int16_t(motor.last_reported_pwm)) < 50) {
            return;
        }
    }

    const int16_t centered = int16_t(motor.pwm_us) - int16_t(DEFAULT_PWM_US);
    const char *state = "stop";
    uint8_t duty_pct = 0;

    if (motor.pwm_us == 0) {
        state = "zero";
    } else if (abs(centered) > DEADZONE_US) {
        state = centered > 0 ? "forward" : "reverse";
        const uint16_t magnitude_us = constrain_uint16(abs(centered), 0, DEFAULT_PWM_US - MIN_PWM_US);
        duty_pct = (uint32_t(magnitude_us) * 100U) / (DEFAULT_PWM_US - MIN_PWM_US);
    }

    duff_log("RCOutput_Duff: %s %s ch=%u pwm=%u state=%s duty=%u%% en=%u inA=%u inB=%u\n",
             reason,
             motor.name,
             unsigned(motor.output_ch),
             unsigned(motor.pwm_us),
             state,
             unsigned(duty_pct),
             unsigned(motor.enable_ch),
             unsigned(motor.in_a_ch),
             unsigned(motor.in_b_ch));

    motor.last_reported_pwm = motor.pwm_us;
}

void RCOutput_Duff::stop_motor(Motor &motor)
{
    if (_dev == nullptr) {
        return;
    }

    stage_stop(motor);
    if (flush_motor(motor)) {
        report_motor(motor, "stop");
    }
}

void RCOutput_Duff::shutdown_outputs()
{
    if (_dev == nullptr) {
        return;
    }

    _safety_on = true;
    _corked = false;
    _pending = false;
    _left.enabled = false;
    _right.enabled = false;
    _left.pwm_us = 0;
    _right.pwm_us = 0;

    stage_stop(_left);
    stage_stop(_right);
    const bool channels_off = flush_channel_range(0, PCA9685_USED_CHANNEL_COUNT - 1);

    bool all_off = false;
    if (_dev->get_semaphore()->take(10)) {
        all_off = write_register(PCA9685_RA_ALL_LED_OFF_H, PCA9685_ALL_LED_OFF_H_SHUT);
        _dev->get_semaphore()->give();
    }

    duff_log("RCOutput_Duff: shutdown outputs %s\n",
             (channels_off && all_off) ? "ok" : "failed");
}

RCOutput_Duff::Motor *RCOutput_Duff::find_motor(uint8_t output_ch)
{
    if (output_ch == _left.output_ch) {
        return &_left;
    }
    if (output_ch == _right.output_ch) {
        return &_right;
    }
    return nullptr;
}
