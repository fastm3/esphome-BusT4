#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <functional>
#include <vector>
#include "esphome/components/uart/uart.h"
#include "t4_packet.h"

#include <driver/uart.h>
#include "esphome/components/uart/uart_component_esp_idf.h"

namespace esphome::bus_t4 {

// Forward declaration
class BusT4Device;

// A queued transmission, already framed. Packets and raw sends share one path so that
// nothing writes to the UART outside the bus task.
struct T4Frame {
  uint8_t size = 0;
  bool raw = false;
  uint8_t data[sizeof(T4Packet::data) + 3] = {};  // SYNC + size + payload + size
};

class BusT4Component final : public Component, public uart::UARTDevice {
 public:
  BusT4Component() = default;

  void setup() override;
  void loop() override;
  void dump_config() override;

  bool read(T4Packet *packet, TickType_t xTicksToWait) {
    if (rxQueue_ == nullptr)
      return false;
    return xQueueReceive(rxQueue_, packet, xTicksToWait);
  }

  bool write(T4Packet *packet, TickType_t xTicksToWait);

  // Queue already-framed bytes verbatim (for debugging/testing)
  void write_raw(const uint8_t *data, size_t len);

  void set_address(const uint16_t address) {
    address_.address = static_cast<uint8_t>(address >> 8);
    address_.endpoint = static_cast<uint8_t>(address & 0xFF);
  }

  T4Source get_address() const { return address_; }

  // Register a device to receive packet callbacks
  void register_device(BusT4Device *device) { devices_.push_back(device); }

  // Bus link health. A growing `rx_discarded` is the signature of a frame that lost
  // synchronisation, which shut the transmit gate until it was recovered; a growing
  // `tx_dropped` means frames were lost because the transmit queue was full. Both used to
  // be silent, and both look like "Home Assistant answers, the gate ignores commands".
  uint32_t get_rx_packets() const { return rx_packets_; }
  uint32_t get_rx_discarded() const { return rx_discarded_; }
  uint32_t get_tx_frames() const { return tx_frames_; }
  uint32_t get_tx_dropped() const { return tx_dropped_; }
  void count_tx_frame() { tx_frames_++; }
  void count_tx_dropped() { tx_dropped_++; }

 private:
  // Receive and transmit share one task so a send can never land mid-frame
  void busTask();
  static void busTaskThunk(void *self) { static_cast<BusT4Component *>(self)->busTask(); }

  bool queue_frame(const T4Frame &frame, TickType_t xTicksToWait);

  // Send a BusT4 break signal (~1ms low pulse) before each packet.
  // Temporarily lowers UART baud rate to produce the correct break duration.
  void send_break();

  T4Source address_;

  TaskHandle_t busTask_ = nullptr;

  QueueHandle_t rxQueue_ = nullptr;
  QueueHandle_t txQueue_ = nullptr;

  std::vector<BusT4Device *> devices_;

  // Cached UART port for direct baud rate register writes during break signal.
  uart_port_t uart_num_ = UART_NUM_MAX;

  // Updated by the bus task, read from the main loop. Plain 32-bit stores are atomic on the
  // ESP32, and a value that is one poll interval stale is harmless for a counter.
  uint32_t rx_packets_ = 0;
  uint32_t rx_discarded_ = 0;
  uint32_t tx_frames_ = 0;
  uint32_t tx_dropped_ = 0;
};

} // namespace esphome::bus_t4
