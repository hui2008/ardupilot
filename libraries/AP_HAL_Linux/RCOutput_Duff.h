#pragma once

#include <AP_HAL_Empty/AP_HAL_Empty.h>
#include <AP_HAL_Empty/AP_HAL_Empty_Private.h>

#include <atomic>
#include <pthread.h>

namespace Linux {

/*
  Duff has one Linux-controlled PWM output implemented directly with GPIO.

  This class is deliberately small: higher layers decide which vehicle function
  is assigned to a physical SERVO output using SERVOx_FUNCTION parameters.
  RCOutput_Duff only receives the final zero-based HAL channel number and PWM
  pulse width from SRV_Channel::output_ch().

  For this experiment the backend intentionally does not filter by channel.
  Whatever channel the high-level caller passes is logged, and the latest PWM
  value is sent to the single physical output on Raspberry Pi BCM GPIO18. This
  makes it easy to see which SERVOx mapping reached the HAL without needing the
  driver to know that mapping in advance.
 */
class RCOutput_Duff : public Empty::RCOutput {
public:
    void init() override;
    void set_freq(uint32_t chmask, uint16_t freq_hz) override;
    void enable_ch(uint8_t ch) override;
    void disable_ch(uint8_t ch) override;
    void write(uint8_t ch, uint16_t period_us) override;
    uint16_t read(uint8_t ch) override;

private:
    // pthread entry points are plain C function pointers, so this static
    // trampoline converts the void* argument back to the C++ object before
    // running the member PWM loop.
    static void *thread_trampoline(void *arg);
    void pwm_loop();

    // GPIO18 supports hardware PWM on Raspberry Pi, but this experimental
    // backend drives it as a normal GPIO so the implementation does not depend
    // on Linux PWM sysfs, overlays, or board-specific PWM device paths.
    static constexpr uint8_t PWM_GPIO = 18;

    // RC servo/ESC PWM is conventionally expressed in microseconds. Constraining
    // to 1000..2000us keeps this backend in the normal actuator range even if a
    // caller sends a wider ArduPilot RCOutput value.
    static constexpr uint16_t MIN_PWM_US = 1000;
    static constexpr uint16_t MAX_PWM_US = 2000;
    static constexpr uint16_t DEFAULT_PWM_US = 1500;

    // 50Hz is the traditional servo frame rate. set_freq() can change it, but
    // the default should produce a valid neutral signal before configuration.
    static constexpr uint16_t DEFAULT_FREQ_HZ = 50;

    // These values are written from ArduPilot control code and read from the
    // PWM pthread. Atomics avoid a mutex in the timing loop and ensure the
    // worker always observes complete 16-bit/bool values.
    std::atomic<uint16_t> _pwm_us { DEFAULT_PWM_US };
    std::atomic<uint16_t> _freq_hz { DEFAULT_FREQ_HZ };
    std::atomic<bool> _enabled { false };

    pthread_t _thread {};
    bool _thread_started {};
};

} // namespace Linux
