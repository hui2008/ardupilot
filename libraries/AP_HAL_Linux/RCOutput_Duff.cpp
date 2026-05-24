#include "RCOutput_Duff.h"

#include <AP_HAL/AP_HAL.h>
#include <AP_Math/AP_Math.h>

#include <cmath>
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

RCOutput_Duff::~RCOutput_Duff()
{
    delete _dev;
}

void RCOutput_Duff::init()
{
    if (_dev == nullptr) {
        _dev = hal.i2c_mgr->get_device_ptr(PCA9685_BUS, PCA9685_ADDRESS);
    }

    if (_dev == nullptr) {
        hal.console->printf("RCOutput_Duff: failed to open PCA9685 bus=%u addr=0x%02x\n",
                            unsigned(PCA9685_BUS), unsigned(PCA9685_ADDRESS));
        return;
    }

    _dev->set_retries(2);

    if (!_dev->get_semaphore()->take(10)) {
        hal.console->printf("RCOutput_Duff: failed to lock PCA9685\n");
        return;
    }

    const bool ok =
        write_register(PCA9685_RA_ALL_LED_OFF_H, PCA9685_ALL_LED_OFF_H_SHUT) &&
        write_register(PCA9685_RA_MODE1, PCA9685_MODE1_SLEEP_BIT);

    _dev->get_semaphore()->give();

    if (!ok) {
        hal.console->printf("RCOutput_Duff: PCA9685 init failed\n");
        return;
    }

    set_freq(0, DEFAULT_FREQ_HZ);

    if (_dev->get_semaphore()->take(10)) {
        write_register(PCA9685_RA_MODE2, PCA9685_MODE2_OUTDRV_BIT);
        write_register(PCA9685_RA_ALL_LED_ON_L, 0);
        write_register(PCA9685_RA_ALL_LED_ON_H, 0);
        write_register(PCA9685_RA_ALL_LED_OFF_L, 0);
        write_register(PCA9685_RA_ALL_LED_OFF_H, 0);
        _dev->get_semaphore()->give();
    }

    stop_motor(_left);
    stop_motor(_right);
    _safety_on = false;

    hal.console->printf("RCOutput_Duff: PCA9685 L298N skid output ready on bus=%u addr=0x%02x\n",
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

    write_register(PCA9685_RA_ALL_LED_OFF_H, PCA9685_ALL_LED_OFF_H_SHUT);
    write_register(PCA9685_RA_MODE1, PCA9685_MODE1_SLEEP_BIT);
    write_register(PCA9685_RA_PRE_SCALE, prescale);
    write_register(PCA9685_RA_MODE1, PCA9685_MODE1_RESTART_BIT | PCA9685_MODE1_AI_BIT);
    write_register(PCA9685_RA_ALL_LED_OFF_H, 0);

    _dev->get_semaphore()->give();

    _freq_hz = PCA9685_INTERNAL_CLOCK / (4096.0f * (prescale + 1));
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
    stop_motor(*motor);
}

bool RCOutput_Duff::force_safety_on()
{
    _safety_on = true;
    stop_motor(_left);
    stop_motor(_right);
    return true;
}

void RCOutput_Duff::force_safety_off()
{
    _safety_on = false;
    if (_left.enabled) {
        set_motor(_left, _left.pwm_us);
    }
    if (_right.enabled) {
        set_motor(_right, _right.pwm_us);
    }
}

void RCOutput_Duff::write(uint8_t ch, uint16_t period_us)
{
    Motor *motor = find_motor(ch);
    if (motor == nullptr) {
        return;
    }

    motor->pwm_us = period_us;
    if (_corked) {
        _pending = true;
        return;
    }
    set_motor(*motor, period_us);
}

uint16_t RCOutput_Duff::read(uint8_t ch)
{
    const Motor *motor = find_motor(ch);
    return motor != nullptr ? motor->pwm_us : 0;
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
    _pending = false;
}

void RCOutput_Duff::push()
{
    _corked = false;
    if (!_pending) {
        return;
    }
    _pending = false;

    if (_left.enabled) {
        set_motor(_left, _left.pwm_us);
    }
    if (_right.enabled) {
        set_motor(_right, _right.pwm_us);
    }
}

bool RCOutput_Duff::write_register(uint8_t reg, uint8_t reg_value)
{
    if (_dev == nullptr) {
        return false;
    }
    return _dev->write_register(reg, reg_value);
}

bool RCOutput_Duff::write_channel(uint8_t pca_ch, uint16_t ticks)
{
    if (_dev == nullptr || pca_ch >= PCA9685_CHANNEL_COUNT) {
        return false;
    }

    ticks = MIN(ticks, PCA9685_FULL_ON);

    uint8_t data[] {
        uint8_t(PCA9685_RA_LED0_ON_L + 4U * pca_ch),
        0,
        ticks == PCA9685_FULL_ON ? uint8_t(PCA9685_LED_ON_H_ALWAYS_ON_BIT) : uint8_t(0),
        uint8_t(ticks & 0xFF),
        ticks == PCA9685_FULL_ON ? uint8_t(0) :
            (ticks == 0 ? uint8_t(PCA9685_LED_OFF_H_ALWAYS_OFF_BIT) : uint8_t(ticks >> 8)),
    };

    return _dev->transfer(data, sizeof(data), nullptr, 0);
}

bool RCOutput_Duff::write_gpio_channel(uint8_t pca_ch, bool active)
{
    return write_channel(pca_ch, active ? PCA9685_FULL_ON : 0);
}

bool RCOutput_Duff::set_motor(Motor &motor, uint16_t pwm_us)
{
    if (_dev == nullptr || _safety_on || !motor.enabled || pwm_us == 0) {
        stop_motor(motor);
        return false;
    }

    pwm_us = constrain_uint16(pwm_us, MIN_PWM_US, MAX_PWM_US);
    return apply_motor(motor, pwm_us);
}

bool RCOutput_Duff::apply_motor(Motor &motor, uint16_t pwm_us)
{
    const int16_t centered = int16_t(pwm_us) - int16_t(DEFAULT_PWM_US);

    if (abs(centered) <= DEADZONE_US) {
        stop_motor(motor);
        return true;
    }

    const bool forward = centered > 0;
    const uint16_t magnitude_us = constrain_uint16(abs(centered), 0, DEFAULT_PWM_US - MIN_PWM_US);
    const uint16_t duty_ticks = (uint32_t(magnitude_us) * (PCA9685_FULL_ON - 1)) /
                                (DEFAULT_PWM_US - MIN_PWM_US);

    if (!_dev->get_semaphore()->take(10)) {
        return false;
    }

    const bool ok =
        write_channel(motor.enable_ch, 0) &&
        write_gpio_channel(motor.in_a_ch, forward) &&
        write_gpio_channel(motor.in_b_ch, !forward) &&
        write_channel(motor.enable_ch, duty_ticks);

    _dev->get_semaphore()->give();

    return ok;
}

void RCOutput_Duff::stop_motor(Motor &motor)
{
    if (_dev == nullptr) {
        return;
    }

    if (!_dev->get_semaphore()->take(10)) {
        return;
    }

    write_channel(motor.enable_ch, 0);
    write_gpio_channel(motor.in_a_ch, false);
    write_gpio_channel(motor.in_b_ch, false);

    _dev->get_semaphore()->give();
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
