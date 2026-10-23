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

  if (this->offset_ >= length) {
    ESP_LOGW(TAG, "Register 0x%02X: offset %u is past the %u byte(s) of the reply", this->register_,
             this->offset_, length);
    return;
  }

  // Whole payload (or the requested window) as a big-endian integer, capped at 4 bytes
  uint8_t count = this->decoded_length_ == 0 ? static_cast<uint8_t>(length - this->offset_) : this->decoded_length_;
  if (count > 4) {
    ESP_LOGW(TAG, "Register 0x%02X: %u bytes requested, only 4 can be combined", this->register_, count);
    count = 4;
  }
  if (this->offset_ + count > length) {
    ESP_LOGW(TAG, "Register 0x%02X: %u byte(s) requested at offset %u but the reply carries %u byte(s)",
             this->register_, count, this->offset_, length);
    count = static_cast<uint8_t>(length - this->offset_);
  }

  uint32_t value = 0;
  for (uint8_t i = 0; i < count; i++)
    value = (value << 8) | payload[this->offset_ + i];

  ESP_LOGD(TAG, "Register 0x%02X: %u byte(s) at offset %u -> %u", this->register_, count, this->offset_,
           (unsigned) value);
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
  } else if (this->offset_ != 0 || this->decoded_length_ != 0) {
    ESP_LOGCONFIG(TAG, "  Offset: %u, length: %u (0 = to the end of the reply)", this->offset_,
                  this->decoded_length_);
  }
  LOG_UPDATE_INTERVAL(this);
}

}  // namespace esphome::bus_t4
