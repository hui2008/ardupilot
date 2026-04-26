#include "RCOutput_Duff.h"

#include <AP_HAL/AP_HAL.h>
#include <AP_Math/AP_Math.h>

#include <unistd.h>

extern const AP_HAL::HAL& hal;

using namespace Linux;

void RCOutput_Duff::init()
{
    hal.gpio->pinMode(PWM_GPIO, HAL_GPIO_OUTPUT);
    hal.gpio->write(PWM_GPIO, 0);

    if (!_thread_started) {
        const int ret = pthread_create(&_thread, nullptr, thread_trampoline, this);
        if (ret == 0) {
            _thread_started = true;
        } else {
            hal.console->printf("RCOutput_Duff: pthread_create failed: %d\n", ret);
        }
    }
}

void RCOutput_Duff::set_freq(uint32_t chmask, uint16_t freq_hz)
{
    if ((chmask & PWM_CHAN_MASK) == 0) {
        return;
    }

    _freq_hz.store(constrain_uint16(freq_hz, 1, 400));
}

void RCOutput_Duff::enable_ch(uint8_t ch)
{
    if (ch == PWM_CHAN) {
        _enabled.store(true);
    }
}

void RCOutput_Duff::disable_ch(uint8_t ch)
{
    if (ch == PWM_CHAN) {
        _enabled.store(false);
        hal.gpio->write(PWM_GPIO, 0);
    }
}

void RCOutput_Duff::write(uint8_t ch, uint16_t period_us)
{
    if (ch != PWM_CHAN) {
        return;
    }

    _pwm_us.store(constrain_uint16(period_us, MIN_PWM_US, MAX_PWM_US));
}

uint16_t RCOutput_Duff::read(uint8_t ch)
{
    if (ch == PWM_CHAN) {
        return _pwm_us.load();
    }
    return 0;
}

void *RCOutput_Duff::thread_trampoline(void *arg)
{
    static_cast<RCOutput_Duff *>(arg)->pwm_loop();
    return nullptr;
}

void RCOutput_Duff::pwm_loop()
{
    while (true) {
        if (!_enabled.load()) {
            hal.gpio->write(PWM_GPIO, 0);
            usleep(20000);
            continue;
        }

        const uint16_t freq_hz = _freq_hz.load();
        const uint32_t frame_us = 1000000UL / freq_hz;
        const uint32_t high_us = MIN(uint32_t(_pwm_us.load()), frame_us);
        const uint32_t low_us = frame_us - high_us;

        hal.gpio->write(PWM_GPIO, 1);
        usleep(high_us);

        hal.gpio->write(PWM_GPIO, 0);
        usleep(low_us);
    }
}
