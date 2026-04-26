#pragma once

#include "AP_HAL_Linux.h"
#include <AP_HAL/RCOutput.h>
#include <AP_HAL_Empty/AP_HAL_Empty.h>
#include <AP_HAL_Empty/AP_HAL_Empty_Private.h>

#include <atomic>
#include <pthread.h>

namespace Linux {

class RCOutput_Duff : public Empty::RCOutput {
public:
    RCOutput_Duff();

    void init() override;
    void set_freq(uint32_t chmask, uint16_t freq_hz) override;
    void enable_ch(uint8_t ch) override;
    void disable_ch(uint8_t ch) override;
    void write(uint8_t ch, uint16_t period_us) override;
    uint16_t read(uint8_t ch) override;

private:
    static void *thread_trampoline(void *arg);
    void pwm_loop();

    static constexpr uint8_t CH_SERVO1 = 0;
    static constexpr uint8_t GPIO_SERVO1 = 18; // BCM GPIO18, physical pin 12

    std::atomic<uint16_t> _pwm_us;
    std::atomic<uint16_t> _freq_hz;
    std::atomic<bool> _enabled;
    std::atomic<bool> _running;

    pthread_t _thread {};
    bool _thread_started;
};

}
