#include "bus_t4_component.h"
#include "bus_t4.h"

#include "esphome/core/log.h"
#include "esphome/core/helpers.h"

#include "t4_packet.h"

#include <esp_rom_sys.h>

namespace esphome::bus_t4 {

static const char *TAG = "bus_t4";

// Bumped when tagging a release; checked against the tag by CI
static const char *VERSION = "1.3.6";

// Break signal baud rate: sending 0x00 at 9200 baud produces a ~1ms low pulse
// on a 19200 baud bus. Reference: pruwait/Nice_BusT4 nice-bust4.h
static constexpr uint32_t T4_BAUD_BREAK = 9200;

// Observed request-to-reply latency on a shared bus reaches ~55ms
static constexpr uint32_t BUS_IDLE_MS = 100;

// How long a frame under assembly may stall before it is abandoned. A complete frame is
// SYNC + length + at most 60 payload bytes + length = 63 bytes sent back to back, i.e.
// 63 x 10 bits at 19200 8N1 = 32.8 ms, and the break in front of it adds ~1 ms. A 50 ms gap
// therefore cannot happen inside a frame that is still coming: it means the frame will never
// complete, and the parser has to resynchronise or the bus deadlocks (see busTask).
static constexpr uint32_t FRAME_TIMEOUT_MS = 50;

// How long the line must stay silent before a discarded frame is considered over. The break
// in front of a frame cannot be recognised in the byte stream: the UART delivers it as a 0x00,
// and 0x00 also appears in almost every frame (addresses, status bytes), so resynchronising on
// a 0x00 restarted the sync search in the middle of the very frame being discarded. Silence is
// the only reliable frame boundary. One byte takes 0.52 ms, but bytes reach the driver in
// bursts - the RX FIFO is handed over at 18 bytes or after a 2-symbol timeout, i.e. up to
// ~9.4 ms - so a gap shorter than that can occur inside a frame. 15 ms is above it and still
// well below FRAME_TIMEOUT_MS.
static constexpr uint32_t DISCARD_SILENCE_MS = 15;

// Bounded so a busy bus cannot starve the transmit path
static constexpr uint8_t RX_DRAIN_LIMIT = 64;

static constexpr TickType_t TX_MIN_INTERVAL = pdMS_TO_TICKS(100);

namespace {

// Parser states. DISCARD swallows the rest of a frame that failed a check instead of
// resynchronising in the middle of it, where a payload 0x55 would be taken for a sync byte
// and the byte after it for a length - and the parser would then wait for bytes that never
// come, with the transmit gate shut behind it. It ends on line silence (DISCARD_SILENCE_MS).
enum RxState : uint8_t { WAIT_SYNC, RX_SIZE, RX_DATA, TRAILING_SIZE, DISCARD };

const char *rx_state_name(RxState state) {
  switch (state) {
    case WAIT_SYNC:
      return "WAIT_SYNC";
    case RX_SIZE:
      return "SIZE";
    case RX_DATA:
      return "DATA";
    case TRAILING_SIZE:
      return "TRAILING_SIZE";
    case DISCARD:
      return "DISCARD";
  }
  return "UNKNOWN";
}

}  // namespace

void BusT4Component::setup() {
  rxQueue_ = xQueueCreate(32, sizeof(T4Packet));
  if (rxQueue_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create RX queue");
    this->mark_failed();
    return;
  }

  // 64 frames: boot is the busiest moment - cover init steps, every register sensor's first
  // poll and the restored template numbers all queue within a second, while the bus drains one
  // frame per ~200 ms. 32 was measurably too shallow: 6 frames of the boot burst were dropped.
  txQueue_ = xQueueCreate(64, sizeof(T4Frame));
  if (txQueue_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create TX queue");
    vQueueDelete(rxQueue_);
    rxQueue_ = nullptr;
    this->mark_failed();
    return;
  }

  if (xTaskCreate(busTaskThunk, "bus_t4", 8192, this, 10, &busTask_) != pdPASS) {
    ESP_LOGE(TAG, "Failed to create bus task");
    this->mark_failed();
    return;
  }

  // Cache UART port number for direct baud rate changes during break signal.
  auto *idf_uart = static_cast<uart::IDFUARTComponent *>(parent_);
  uart_num_ = static_cast<uart_port_t>(idf_uart->get_hw_serial_number());
}

void BusT4Component::loop() {
  // Process received packets and dispatch to registered devices
  T4Packet packet;
  while (xQueueReceive(rxQueue_, &packet, 0)) {
    // Ignore packets FROM ourselves (TX echo on half-duplex bus)
    if (packet.header.from == address_) {
      ESP_LOGV(TAG, "Ignoring TX echo");
      continue;
    }

    // Log foreign destinations without dropping them yet
    if (!t4_addressed_to(packet.header.to, address_)) {
      ESP_LOGD(TAG, "Packet addressed to 0x%02X.%02X, not us: %s",
               packet.header.to.address, packet.header.to.endpoint,
               format_hex_pretty(packet.data, packet.size).c_str());
    }

    // Dispatch to all registered devices
    for (auto *device : devices_) {
      device->on_packet(packet);
    }
  }
}

void BusT4Component::dump_config() {
  ESP_LOGCONFIG(TAG, "BusT4:");
  ESP_LOGCONFIG(TAG, "  Version: %s", VERSION);
  ESP_LOGCONFIG(TAG, "  Address: 0x%02X%02X", address_.address, address_.endpoint);
}

void BusT4Component::busTask() {
  T4Packet packet;
  uint8_t expected_size = 0;
  // Protocol format: [BREAK] SYNC SIZE DATA[N] SIZE
  // The size byte is sent twice (start and end), no separate checksum byte.
  // Internal checksums are within the DATA portion.
  RxState rx_state = WAIT_SYNC;
  uint32_t last_rx_ms = 0;
  // When the frame under assembly is given up on. Every byte received while not discarding
  // pushes it back; inside DISCARD nothing does, so the parser always recovers, even on a bus
  // that never goes quiet.
  uint32_t rx_deadline_ms = 0;
  TickType_t last_tx_time = 0;

  for (;;) {
    uint8_t byte;
    uint8_t drained = 0;
    while (drained < RX_DRAIN_LIMIT && parent_->available() && parent_->read_byte(&byte) == true) {
      drained++;
      last_rx_ms = millis();
      if (rx_state != DISCARD)
        rx_deadline_ms = millis() + FRAME_TIMEOUT_MS;
      switch (rx_state) {
        case WAIT_SYNC:
          // Wait for SYNC byte (0x55), ignore break bytes (0x00) and others
          if (byte == T4_SYNC) {
            rx_state = RX_SIZE;
          }
          break;

        case RX_SIZE:
          if (byte > 0 && byte <= 60) {
            expected_size = byte;
            packet.size = 0;
            rx_state = RX_DATA;
          } else {
            // Invalid size, go back to waiting for sync
            rx_state = WAIT_SYNC;
          }
          break;

        case RX_DATA:
          packet.data[packet.size++] = byte;
          if (packet.size == expected_size)
            rx_state = TRAILING_SIZE;
          break;

        case DISCARD:
          // Drop everything until the line goes quiet (checked after this loop). Do not look
          // for a break here: it arrives as 0x00, which is ordinary payload too.
          break;

        case TRAILING_SIZE:
          // Verify trailing size matches
          if (byte == expected_size) {
            // Verify header checksum (CRC1): XOR of bytes 0-5 should equal byte 6
            uint8_t header_check = packet.checksum(0, 6);
            if (header_check != packet.data[6]) {
              ESP_LOGW(TAG, "Header checksum mismatch: expected 0x%02X, got 0x%02X: %s",
                       header_check, packet.data[6],
                       format_hex_pretty(packet.data, packet.size).c_str());
              rx_discarded_++;
              rx_state = DISCARD;
              break;
            }

            // Verify payload checksum (CRC2): XOR of bytes 7 to size-2 should equal byte size-1
            if (packet.size >= 9) {  // Minimum: 7 header + 1 payload + 1 CRC2
              uint8_t payload_check = packet.data[7];
              for (size_t i = 8; i < packet.size - 1; i++) {
                payload_check ^= packet.data[i];
              }
              if (payload_check != packet.data[packet.size - 1]) {
                ESP_LOGW(TAG, "Payload checksum mismatch: expected 0x%02X, got 0x%02X: %s",
                         payload_check, packet.data[packet.size - 1],
                         format_hex_pretty(packet.data, packet.size).c_str());
                rx_discarded_++;
                rx_state = DISCARD;
                break;
              }
            }

            // Both checksums valid - accept packet
            rx_packets_++;
            ESP_LOGD(TAG, "Received packet: %s (%d bytes)",
                     format_hex_pretty(packet.data, packet.size).c_str(), packet.size);
            if (!xQueueSend(rxQueue_, &packet, pdMS_TO_TICKS(100))) {
              ESP_LOGW(TAG, "RX queue full, dropping packet");
            }
            rx_state = WAIT_SYNC;
          } else {
            ESP_LOGW(TAG, "Trailing size mismatch: expected 0x%02X, got 0x%02X", expected_size, byte);
            rx_discarded_++;
            rx_state = DISCARD;
          }
          break;
      }
    }

    // A frame that stopped arriving will never complete, and while the parser is not in
    // WAIT_SYNC the transmit gate below stays shut. Nothing is polled then, so no reply ever
    // arrives to resynchronise on, and the link deadlocks until the gate is moved by other
    // means - a remote, for instance. That is why commands can be ignored for hours while
    // Home Assistant still reaches the device.
    // End of a discarded frame: the line has been quiet long enough for it to be over.
    if (rx_state == DISCARD && millis() - last_rx_ms >= DISCARD_SILENCE_MS) {
      ESP_LOGD(TAG, "Line quiet after a discarded frame, resynchronising");
      rx_state = WAIT_SYNC;
      expected_size = 0;
      packet.size = 0;
    }

    // DISCARD does not push the deadline back, so on a bus that never goes quiet it still ends
    // here; it was already counted when it was entered, so it is not counted twice.
    if (rx_state == DISCARD && (int32_t) (millis() - rx_deadline_ms) >= 0) {
      ESP_LOGW(TAG, "Bus never went quiet after a discarded frame, resynchronising anyway");
      rx_state = WAIT_SYNC;
      expected_size = 0;
      packet.size = 0;
    }

    if (rx_state != WAIT_SYNC && (int32_t) (millis() - rx_deadline_ms) >= 0) {
      ESP_LOGW(TAG, "Discarding incomplete frame (state=%s, expected=%u, got=%u): %s",
               rx_state_name(rx_state), (unsigned) expected_size, (unsigned) packet.size,
               format_hex_pretty(packet.data, packet.size).c_str());
      rx_discarded_++;
      rx_state = WAIT_SYNC;
      expected_size = 0;
      packet.size = 0;
      last_rx_ms = millis();  // keep the transmit gate shut for BUS_IDLE_MS
    }

    // Transmit only between frames, and only once the bus has gone quiet
    if (rx_state == WAIT_SYNC && millis() - last_rx_ms >= BUS_IDLE_MS) {
      T4Frame frame;
      if (xQueueReceive(txQueue_, &frame, 0)) {
        TickType_t elapsed = xTaskGetTickCount() - last_tx_time;
        if (elapsed < TX_MIN_INTERVAL) {
          vTaskDelay(TX_MIN_INTERVAL - elapsed);
        }

        if (frame.raw) {
          ESP_LOGD(TAG, "Sending raw: %s", format_hex_pretty(frame.data, frame.size).c_str());
        } else {
          ESP_LOGD(TAG, "Sending packet: %s",
                   format_hex_pretty(frame.data + 2, frame.size - 3).c_str());
        }
        send_break();
        parent_->write_array(frame.data, frame.size);
        parent_->flush();

        tx_frames_++;
        last_tx_time = xTaskGetTickCount();
      }
    }

    vTaskDelay(1);
  }

  busTask_ = nullptr;
  vTaskDelete(nullptr);
}

bool BusT4Component::queue_frame(const T4Frame &frame, TickType_t xTicksToWait) {
  if (txQueue_ == nullptr)
    return false;
  return xQueueSend(txQueue_, &frame, xTicksToWait);
}

bool BusT4Component::write(T4Packet *packet, TickType_t xTicksToWait) {
  if (packet == nullptr || packet->size == 0 || packet->size > sizeof(T4Packet::data))
    return false;

  T4Frame frame;
  frame.data[0] = T4_SYNC;
  frame.data[1] = packet->size;
  std::copy_n(packet->data, packet->size, frame.data + 2);
  frame.data[packet->size + 2] = packet->size;
  frame.size = packet->size + 3;
  return queue_frame(frame, xTicksToWait);
}

void BusT4Component::write_raw(const uint8_t *data, size_t len) {
  // User-provided bytes are already framed, so they go out verbatim
  if (len == 0 || len > sizeof(T4Frame::data)) {
    ESP_LOGW(TAG, "Raw frame of %u bytes rejected", static_cast<unsigned>(len));
    return;
  }

  T4Frame frame;
  frame.raw = true;
  frame.size = static_cast<uint8_t>(len);
  std::copy_n(data, len, frame.data);
  queue_frame(frame, 0);
}

void BusT4Component::send_break() {
  // BusT4 protocol requires a break signal (~1ms low pulse) before each packet.
  // Achieved by sending 0x00 at a lower baud rate (9200 instead of 19200).
  // Reference: pruwait/Nice_BusT4 send_array_cmd() implementation.
  if (uart_num_ < UART_NUM_MAX) {
    uint32_t work_baud = parent_->get_baud_rate();
    uart_set_baudrate(uart_num_, T4_BAUD_BREAK);
    parent_->write_byte(T4_BREAK);
    parent_->flush();
    esp_rom_delay_us(100);
    uart_set_baudrate(uart_num_, work_baud);
  } else {
    // UART port not cached — fall back to simple break byte
    parent_->write_byte(T4_BREAK);
    parent_->flush();
  }
}

} // namespace esphome::bus_t4
