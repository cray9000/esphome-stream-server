#pragma once

#include "esphome/core/component.h"
#include "esphome/components/socket/socket.h"

#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif
#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif

#include <memory>
#include <string>
#include <vector>

class StreamServerComponent : public esphome::Component {
public:
    StreamServerComponent() = default;
    void set_buffer_size(size_t size) { this->buf_size_ = size; }

#ifdef USE_BINARY_SENSOR
    void set_connected_sensor(esphome::binary_sensor::BinarySensor *connected) { this->connected_sensor_ = connected; }
#endif
#ifdef USE_SENSOR
    void set_connection_count_sensor(esphome::sensor::Sensor *connection_count) { this->connection_count_sensor_ = connection_count; }
#endif

    void setup() override;
    void loop() override;
    void dump_config() override;
    void on_shutdown() override;

    float get_setup_priority() const override { return esphome::setup_priority::AFTER_WIFI; }

    void set_port(uint16_t port) { this->port_ = port; }

protected:
    void publish_sensor();
    void accept();
    void cleanup();
    void read();
    void flush();
    void write();

    size_t buf_index(size_t pos) { return pos & (this->buf_size_ - 1); }
    size_t buf_ahead(size_t pos) { return (pos | (this->buf_size_ - 1)) - pos + 1; }

    struct Client {
        Client(std::unique_ptr<esphome::socket::Socket> socket, std::string identifier, size_t position);

        std::unique_ptr<esphome::socket::Socket> socket{nullptr};
        std::string identifier{};
        bool disconnected{false};
        size_t position{0};
    };

    uint16_t port_;
    size_t buf_size_;

#ifdef USE_BINARY_SENSOR
    esphome::binary_sensor::BinarySensor *connected_sensor_;
#endif
#ifdef USE_SENSOR
    esphome::sensor::Sensor *connection_count_sensor_;
#endif

    std::unique_ptr<uint8_t[]> buf_{};  
    size_t buf_head_{0};
    size_t buf_tail_{0};

    std::unique_ptr<esphome::socket::Socket> server_socket_{};  // Server socket to listen for clients
    std::vector<Client> clients_;
};
