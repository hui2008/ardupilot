#pragma once

#include <AP_HAL_Empty/AP_HAL_Empty.h>
#include <AP_HAL_Empty/AP_HAL_Empty_Private.h>

#include <atomic>
#include <pthread.h>

namespace Linux {

class RCOutput_Duff : public Empty::RCOutput {
public:
    void init() override;
    void set_freq(uint32_t chmask, uint16_t freq_hz) override;
    void enable_ch(uint8_t ch) override;
    void disable_ch(uint8_t ch) override;
    void write(uint8_t ch, uint16_t period_us) override;
    uint16_t read(uint8_t ch) override;

private:
    static void *thread_trampoline(void *arg);
    void pwm_loop();

    static constexpr uint8_t PWM_CHAN = 0;
    static constexpr uint8_t PWM_GPIO = 18; // BCM GPIO18, physical pin 12
    static constexpr uint32_t PWM_CHAN_MASK = 1U << PWM_CHAN;
    static constexpr uint16_t MIN_PWM_US = 1000;
    static constexpr uint16_t MAX_PWM_US = 2000;
    static constexpr uint16_t DEFAULT_PWM_US = 1500;
    static constexpr uint16_t DEFAULT_FREQ_HZ = 50;

    std::atomic<uint16_t> _pwm_us { DEFAULT_PWM_US };
    std::atomic<uint16_t> _freq_hz { DEFAULT_FREQ_HZ };
    std::atomic<bool> _enabled { false };

    pthread_t _thread {};
    bool _thread_started {};
};

}
