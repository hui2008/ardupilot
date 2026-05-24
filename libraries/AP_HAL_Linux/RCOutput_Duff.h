#pragma once

#include <AP_HAL/AP_HAL.h>
#include <AP_HAL/I2CDevice.h>

namespace Linux {

/*
  Duff drives a two-motor skid-steer rover through a PCA9685 connected to an
  L298N dual H-bridge.

  Rover should map:
    SERVO1_FUNCTION = 73  (ThrottleLeft)
    SERVO3_FUNCTION = 74  (ThrottleRight)

  RCOutput_Duff receives the final zero-based HAL channel number and PWM pulse
  width from SRV_Channel::output_ch(). Channel 0 controls the left motor and
  channel 2 controls the right motor.
 */
class RCOutput_Duff : public AP_HAL::RCOutput {
public:
    ~RCOutput_Duff();

    void init() override;
    void set_freq(uint32_t chmask, uint16_t freq_hz) override;
    uint16_t get_freq(uint8_t ch) override;
    void enable_ch(uint8_t ch) override;
    void disable_ch(uint8_t ch) override;
    bool force_safety_on() override;
    void force_safety_off() override;
    void write(uint8_t ch, uint16_t period_us) override;
    uint16_t read(uint8_t ch) override;
    void read(uint16_t *period_us, uint8_t len) override;
    void cork() override;
    void push() override;

private:
    struct Motor {
        uint8_t output_ch;
        uint8_t enable_ch;
        uint8_t in_a_ch;
        uint8_t in_b_ch;
        uint16_t pwm_us;
        bool enabled;
    };

    static constexpr uint8_t PCA9685_BUS = 1;
    static constexpr uint8_t PCA9685_ADDRESS = 0x40;
    static constexpr uint8_t PCA9685_CHANNEL_COUNT = 16;
    static constexpr uint8_t PCA9685_USED_CHANNEL_COUNT = 6;

    static constexpr uint16_t MIN_PWM_US = 1000;
    static constexpr uint16_t MAX_PWM_US = 2000;
    static constexpr uint16_t DEFAULT_PWM_US = 1500;
    static constexpr uint16_t DEADZONE_US = 25;
    static constexpr uint16_t DEFAULT_FREQ_HZ = 1000;

    bool write_register(uint8_t reg, uint8_t reg_value);
    bool set_motor(Motor &motor, uint16_t pwm_us);
    void stage_motor(Motor &motor, uint16_t pwm_us);
    void stage_stop(Motor &motor);
    bool flush_motor(const Motor &motor);
    bool flush_channel_range(uint8_t first_ch, uint8_t last_ch);
    static void fill_channel_bytes(uint16_t ticks, uint8_t *data);
    void stop_motor(Motor &motor);
    Motor *find_motor(uint8_t output_ch);

    AP_HAL::I2CDevice *_dev = nullptr;
    uint16_t _freq_hz = DEFAULT_FREQ_HZ;
    bool _safety_on = true;
    bool _corked = false;
    bool _pending = false;
    uint16_t _pca_ticks[PCA9685_USED_CHANNEL_COUNT] {};

    // my/py/pca9685_l298n_wiring.md:
    // Motor A: PCA9685 0=ENA, 1=IN1, 2=IN2.
    // Motor B: PCA9685 5=ENB, 3=IN3, 4=IN4.
    Motor _left { 0, 0, 1, 2, DEFAULT_PWM_US, false };
    Motor _right { 2, 5, 3, 4, DEFAULT_PWM_US, false };
};

} // namespace Linux
