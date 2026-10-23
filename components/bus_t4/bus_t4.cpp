#include "bus_t4.h"

#include <algorithm>

namespace esphome::bus_t4 {

void BusT4Device::send_cmd(T4Command cmd) {
  send_cmd(cmd, OVIEW);
}

void BusT4Device::send_cmd(T4Command cmd, T4Device device) {
  // DEP packet structure: [device] [command] [cmd_value] [offset]
  uint8_t message[4] = { device, RUN, cmd, 0x64 };
  T4Packet packet(target_address_, parent_->get_address(), DEP, message, sizeof(message));
  write(&packet, 0);
}

void BusT4Device::send_info_request(T4Target target, T4InfoCommand command) {
  // DMP GET packet structure: [target] [command] [request_type] [offset] [length]
  uint8_t message[5] = { target, command, REQ_GET, 0x00, 0x00 };
  T4Packet packet(target_address_, parent_->get_address(), DMP, message, sizeof(message));
  write(&packet, 0);
}

void BusT4Device::send_info_request(T4Target target, T4InfoCommand command, const uint8_t *args,
                                    size_t args_size) {
  // DMP GET packet structure: [target] [command] [request_type] [offset] [length] [args...]
  // "length" carries the number of argument bytes that follow.
  if (args == nullptr || args_size == 0 || args_size > 16) {
    ESP_LOGW("bus_t4", "send_info_request: invalid args_size (%u)", (unsigned) args_size);
    return;
  }

  uint8_t message[5 + 16];
  message[0] = target;
  message[1] = command;
  message[2] = REQ_GET;
  message[3] = 0x00;
  message[4] = static_cast<uint8_t>(args_size);
  std::copy(args, args + args_size, message + 5);

  T4Packet packet(target_address_, parent_->get_address(), DMP, message, static_cast<uint8_t>(5 + args_size));
  write(&packet, 0);
}

void BusT4Device::send_config_set(uint8_t param, uint8_t value) {
  // DMP SET packet structure: [target] [param] [request_type] [offset] [length] [value]
  uint8_t message[6] = { FOR_CU, param, REQ_SET, 0x00, 0x01, value };
  T4Packet packet(target_address_, parent_->get_address(), DMP, message, sizeof(message));
  write(&packet, 0);
}

void BusT4Device::send_config_write(uint8_t param, const uint8_t *bytes, size_t length, uint8_t offset) {
  // DMP SET packet structure: [target] [param] [request_type] [offset] [length] [bytes...]
  // length is the number of payload bytes written, starting at offset.
  if (bytes == nullptr || length == 0 || length > 16) {
    ESP_LOGW("bus_t4", "send_config_write: invalid length (%u)", (unsigned) length);
    return;
  }

  uint8_t message[5 + 16];
  message[0] = FOR_CU;
  message[1] = param;
  message[2] = REQ_SET;
  message[3] = offset;
  message[4] = static_cast<uint8_t>(length);
  std::copy(bytes, bytes + length, message + 5);

  T4Packet packet(target_address_, parent_->get_address(), DMP, message, static_cast<uint8_t>(5 + length));
  write(&packet, 0);
}

} // namespace esphome::bus_t4
