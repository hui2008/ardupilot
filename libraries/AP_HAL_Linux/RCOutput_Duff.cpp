#include "RCOutput_Duff.h"

#include <unistd.h>

extern const AP_HAL::HAL& hal;

using namespace Linux;

RCOutput_Duff::RCOutput_Duff() :
    _pwm_us(1500),
    _freq_hz(50),
    _enabled(false),
    _running(false),
    _thread_started(false)
{
}

void RCOutput_Duff::init()
{
    hal.console->printf("RCOutput_Duff: SERVO1 -> BCM GPIO%u\n", GPIO_SERVO1);

    hal.gpio->pinMode(GPIO_SERVO1, HAL_GPIO_OUTPUT);
    hal.gpio->write(GPIO_SERVO1, 0);

    _running.store(true);

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
    if ((chmask & (1U << CH_SERVO1)) == 0) {
        return;
    }

    if (freq_hz < 1) {
        freq_hz = 1;
    } else if (freq_hz > 400) {
        freq_hz = 400;
    }

    _freq_hz.store(freq_hz);
}

void RCOutput_Duff::enable_ch(uint8_t ch)
{
    if (ch == CH_SERVO1) {
        _enabled.store(true);
    }
}

void RCOutput_Duff::disable_ch(uint8_t ch)
{
    if (ch == CH_SERVO1) {
        _enabled.store(false);
        hal.gpio->write(GPIO_SERVO1, 0);
    }
}

void RCOutput_Duff::write(uint8_t ch, uint16_t period_us)
{
    if (ch != CH_SERVO1) {
        return;
    }

    if (period_us < 1000) {
        period_us = 1000;
    } else if (period_us > 2000) {
        period_us = 2000;
    }

    _pwm_us.store(period_us);
}

uint16_t RCOutput_Duff::read(uint8_t ch)
{
    if (ch == CH_SERVO1) {
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
    while (_running.load()) {
        if (!_enabled.load()) {
            hal.gpio->write(GPIO_SERVO1, 0);
            usleep(20000);
            continue;
        }

        const uint16_t freq_hz = _freq_hz.load();
        const uint32_t frame_us = 1000000UL / freq_hz;
        uint32_t high_us = _pwm_us.load();

        if (high_us > frame_us) {
            high_us = frame_us;
        }

        const uint32_t low_us = frame_us - high_us;

        hal.gpio->write(GPIO_SERVO1, 1);
        usleep(high_us);

        hal.gpio->write(GPIO_SERVO1, 0);
        usleep(low_us);
    }

    hal.gpio->write(GPIO_SERVO1, 0);
}