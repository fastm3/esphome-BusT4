#pragma once

#include "esphome/components/sensor/sensor.h"
#include "esphome/core/component.h"

namespace esphome::bus_t4 {

class BusT4Cover;

// Exposes one control-unit parameter (a raw DMP register) as a sensor.
//
// The parameter is polled: every update_interval the cover sends an info
// request for `register_`, optionally carrying the motor index as an argument,
// and the reply payload is published here. Registers whose layout is not
// established (e.g. the MC824H forces 0x4A/0x4B) can publish a single byte of
// the payload instead of the whole big-endian value.
class BusT4RegisterSensor : public sensor::Sensor, public PollingComponent {
 public:
  void setup() override;
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_parent(BusT4Cover *parent) { this->parent_ = parent; }
  void set_register(uint8_t reg) { this->register_ = reg; }
  void set_motor(uint8_t motor) {
    this->motor_ = motor;
    this->has_motor_ = true;
  }
  void set_byte(uint8_t byte) {
    this->byte_ = byte;
    this->has_byte_ = true;
  }

  uint8_t get_register() const { return this->register_; }

  // Called by the cover when the reply for get_register() arrives
  void publish_reply(const uint8_t *payload, uint8_t length);

 protected:
  BusT4Cover *parent_{nullptr};
  uint8_t register_{0x00};
  uint8_t motor_{0};
  bool has_motor_{false};
  uint8_t byte_{0};
  bool has_byte_{false};
};

}  // namespace esphome::bus_t4
