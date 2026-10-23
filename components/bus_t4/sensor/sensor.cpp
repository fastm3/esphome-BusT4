#include "sensor.h"

#include "../cover/cover.h"
#include "esphome/core/log.h"

namespace esphome::bus_t4 {

static const char *TAG = "bus_t4.sensor";

void BusT4RegisterSensor::setup() {
  if (this->parent_ == nullptr) {
    ESP_LOGE(TAG, "Register 0x%02X: no bus_t4 cover attached", this->register_);
    this->mark_failed();
    return;
  }
  this->parent_->register_register_reader(this);
}

void BusT4RegisterSensor::update() {
  if (this->parent_ == nullptr)
    return;
  // -1 means "no argument": only per-motor parameters carry the motor index
  this->parent_->request_register(this->register_, this->has_motor_ ? static_cast<int8_t>(this->motor_) : -1);
}

void BusT4RegisterSensor::publish_reply(const uint8_t *payload, uint8_t length) {
  if (payload == nullptr || length == 0) {
    ESP_LOGW(TAG, "Register 0x%02X: empty reply", this->register_);
    return;
  }

  if (this->has_byte_) {
    if (this->byte_ >= length) {
      ESP_LOGW(TAG, "Register 0x%02X: byte %u requested but the reply carries %u byte(s)", this->register_,
               this->byte_, length);
      return;
    }
    ESP_LOGD(TAG, "Register 0x%02X: byte %u = %u", this->register_, this->byte_, payload[this->byte_]);
    this->publish_state(static_cast<float>(payload[this->byte_]));
    return;
  }

  // Whole payload as a big-endian integer, capped at 4 bytes
  uint32_t value = 0;
  for (uint8_t i = 0; i < length && i < 4; i++)
    value = (value << 8) | payload[i];

  ESP_LOGD(TAG, "Register 0x%02X: %u byte(s) -> %u", this->register_, length, (unsigned) value);
  this->publish_state(static_cast<float>(value));
}

void BusT4RegisterSensor::dump_config() {
  ESP_LOGCONFIG(TAG, "Bus T4 register sensor:");
  ESP_LOGCONFIG(TAG, "  Register: 0x%02X", this->register_);
  if (this->has_motor_) {
    ESP_LOGCONFIG(TAG, "  Motor index: %u", this->motor_);
  }
  if (this->has_byte_) {
    ESP_LOGCONFIG(TAG, "  Byte: %u", this->byte_);
  }
  LOG_UPDATE_INTERVAL(this);
}

}  // namespace esphome::bus_t4
