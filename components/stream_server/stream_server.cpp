#include "stream_server.h"

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/core/util.h"
#include "esphome/core/version.h"

#include "esphome/components/network/util.h"
#include "esphome/components/socket/socket.h"

#include "esphome/core/log.h"  // Ensure you include the logging header
#include <sstream>
#include <iomanip>

static const char *TAG = "stream_server";

using namespace esphome;

// Example values for Modbus Registers
int vsml1 = 1000;  // Example value for measurement 1
int vsml2 = 2000;  // Example value for measurement 2
int vsml3 = 3000;  // Example value for measurement 3

void StreamServerComponent::setup() {
    ESP_LOGCONFIG(TAG, "Setting up stream server...");

    this->buf = std::unique_ptr<uint8_t[]>{new uint8_t[this->buf_size_]};  // Change 'buf_' to 'buf'

    struct sockaddr_storage bind_addr;
#if ESPHOME_VERSION_CODE >= VERSION_CODE(2023, 4, 0)
    socklen_t bind_addrlen = socket::set_sockaddr_any(reinterpret_cast<struct sockaddr *>(&bind_addr), sizeof(bind_addr), this->port_);
#else
    socklen_t bind_addrlen = socket::set_sockaddr_any(reinterpret_cast<struct sockaddr *>(&bind_addr), sizeof(bind_addr), htons(this->port_));
#endif

    this->socket_ = socket::socket_ip(SOCK_STREAM, PF_INET);
    this->socket_->setblocking(false);
    this->socket_->bind(reinterpret_cast<struct sockaddr *>(&bind_addr), bind_addrlen);
    this->socket_->listen(8);
    this->publish_sensor();
}

void StreamServerComponent::loop() {
    this->accept();
    this->read();
    this->flush();
    this->write();
    this->cleanup();
}

void StreamServerComponent::dump_config() {
    ESP_LOGCONFIG(TAG, "Stream Server:");
    ESP_LOGCONFIG(TAG, "  Address: %s:%u", esphome::network::get_use_address().c_str(), this->port_);
}

void StreamServerComponent::accept() {
    struct sockaddr_storage client_addr;
    socklen_t client_addrlen = sizeof(client_addr);
    std::unique_ptr<socket::Socket> socket = this->socket_->accept(reinterpret_cast<struct sockaddr *>(&client_addr), &client_addrlen);
    if (!socket)
        return;

    socket->setblocking(false);
    std::string identifier = socket->getpeername();
    this->clients_.emplace_back(std::move(socket), identifier, this->buf_head_);
    ESP_LOGD(TAG, "New client connected from %s", identifier.c_str());
    this->publish_sensor();
}

void StreamServerComponent::read() {
    size_t len = 0;
    uint8_t buf[128];  // Declare a buffer to hold data
    ssize_t read;

    for (Client &client : this->clients_) {
        if (client.disconnected)
            continue;

        while ((read = client.socket->read(buf, sizeof(buf))) > 0) {
            // Log buffer data size first
            ESP_LOGD(TAG, "Buffer data (size: %d):", read);

            // Check Modbus request (assuming a valid request starts with at least 12 bytes)
            if (read >= 12) {
                uint8_t function_code = buf[7];
                uint16_t register_address = (buf[9] << 8) | buf[10];

                // Handle the Modbus request
                if (function_code == 3) {
                    parse_modbus_request(buf, read);
                }
            }

            // Optionally, insert into received data
            this->received_data_.insert(this->received_data_.end(), buf, buf + read);
        }

        if (read == 0 || errno == ECONNRESET) {
            ESP_LOGD(TAG, "Client %s disconnected", client.identifier.c_str());
            client.disconnected = true;
        } else if (errno == EWOULDBLOCK || errno == EAGAIN) {
            // Expected if the (TCP) receive buffer is empty, nothing to do.
        } else {
            ESP_LOGW(TAG, "Failed to read from client %s with error %d!", client.identifier.c_str(), errno);
        }
    }
}

void StreamServerComponent::handle_modbus_request(uint8_t *buf, ssize_t len) {
    uint16_t register_address = (buf[9] << 8) | buf[10];
    uint8_t num_registers = buf[12];

    ESP_LOGD(TAG, "Modbus Request - Register: %d, Num Registers: %d", register_address, num_registers);

    // Example Modbus register mapping, similar to your script
    uint8_t response[256];  // Ensure there's enough space for the response

    // Set common Modbus response headers
    response[0] = buf[0];  // Transaction ID
    response[1] = buf[1];
    response[2] = buf[2];  // Protocol ID
    response[3] = buf[3];
    response[4] = 0;  // Length (to be filled)
    response[5] = 0;  // Length (to be filled)
    response[6] = num_registers * 2 + 3;  // Byte count
    response[7] = buf[7];  // Device address
    response[8] = buf[8];  // Function code

    // Depending on register_address, set the response data
    switch (register_address) {
        case 40000:
            response[9] = 0x42;  // Example value for register 40000
            response[10] = 0x00;
            response[11] = 0x00;
            response[12] = 0x01;
            break;
        case 40002:
            response[9] = 0x01;  // Example for register 40002
            response[10] = 0x00;
            break;
        // Add more cases for other registers...
        default:
            ESP_LOGW(TAG, "Unknown register address: %d", register_address);
            break;
    }

    // Send Modbus response
    client.socket->write(response, 9 + num_registers * 2);
}

void StreamServerComponent::flush() {
    ssize_t written;
    this->buf_tail_ = this->buf_head_;
    for (Client &client : this->clients_) {
        if (client.disconnected || client.position == this->buf_head_)
            continue;

        struct iovec iov[2];
        iov[0].iov_base = &this->buf[this->buf_index(client.position)];
        iov[0].iov_len = std::min(this->buf_head_ - client.position, this->buf_ahead(client.position));
        iov[1].iov_base = &this->buf[0];
        iov[1].iov_len = this->buf_head_ - (client.position + iov[0].iov_len);
        if ((written = client.socket->writev(iov, 2)) > 0) {
            client.position += written;
        } else if (written == 0 || errno == ECONNRESET) {
            ESP_LOGD(TAG, "Client %s disconnected", client.identifier.c_str());
            client.disconnected = true;
            continue;
        } else if (errno == EWOULDBLOCK || errno == EAGAIN) {
            // Expected if the (TCP) transmit buffer is full, nothing to do.
        } else {
            ESP_LOGE(TAG, "Failed to write to client %s with error %d!", client.identifier.c_str(), errno);
        }
        this->buf_tail_ = std::min(this->buf_tail_, client.position);
    }
}

void StreamServerComponent::write() {
    // No UART stream handling needed here; implement custom logic if necessary.
}

void StreamServerComponent::on_shutdown() {
    for (const Client &client : this->clients_)
        client.socket->shutdown(SHUT_RDWR);
}
