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

void StreamServerComponent::setup() {
    ESP_LOGCONFIG(TAG, "Setting up stream server...");

    // The make_unique() wrapper doesn't like arrays, so initialize the unique_ptr directly.
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
#ifdef USE_BINARY_SENSOR
    LOG_BINARY_SENSOR("  ", "Connected:", this->connected_sensor_);
#endif
#ifdef USE_SENSOR
    LOG_SENSOR("  ", "Connection count:", this->connection_count_sensor_);
#endif
}

void StreamServerComponent::on_shutdown() {
    for (const Client &client : this->clients_)
        client.socket->shutdown(SHUT_RDWR);
}

void StreamServerComponent::publish_sensor() {
#ifdef USE_BINARY_SENSOR
    if (this->connected_sensor_)
        this->connected_sensor_->publish_state(this->clients_.size() > 0);
#endif
#ifdef USE_SENSOR
    if (this->connection_count_sensor_)
        this->connection_count_sensor_->publish_state(this->clients_.size());
#endif
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

void StreamServerComponent::cleanup() {
    auto discriminator = [](const Client &client) { return !client.disconnected; };
    auto last_client = std::partition(this->clients_.begin(), this->clients_.end(), discriminator);
    if (last_client != this->clients_.end()) {
        this->clients_.erase(last_client, this->clients_.end());
        this->publish_sensor();
    }
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

            // Build a hex string of the data
            std::stringstream hex_data;
            for (size_t i = 0; i < read; ++i) {
                hex_data << std::hex << std::setw(2) << std::setfill('0') << (int)buf[i] << " ";
            }

            // Log all the bytes in one message
            ESP_LOGD(TAG, "%s", hex_data.str().c_str());

            // Optionally, insert into received data
            this->received_data_.insert(this->received_data_.end(), buf, buf + read);

            // Pass the data to the Modbus parser
            this->parse_modbus_request(buf, read);
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

void StreamServerComponent::flush() {
    ssize_t written;
    this->buf_tail_ = this->buf_head_;
    for (Client &client : this->clients_) {
        if (client.disconnected || client.position == this->buf_head_)
            continue;

        struct iovec iov[2];
        iov[0].iov_base = &this->buf[this->buf_index(client.position)];  // Change 'buf_' to 'buf'
        iov[0].iov_len = std::min(this->buf_head_ - client.position, this->buf_ahead(client.position));
        iov[1].iov_base = &this->buf[0];  // Change 'buf_' to 'buf'
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
    int available;
    // There is no UART stream anymore; this function now doesn't do anything.
    // You can replace it with a custom data source if necessary (e.g., network, file).
}

void StreamServerComponent::parse_modbus_request(uint8_t *buf, ssize_t len) {
    // Log the buffer size and content for debugging
    ESP_LOGD(TAG, "Buffer data (size: %d):", len);
    for (ssize_t i = 0; i < len; ++i) {
        ESP_LOGD(TAG, "%02x", buf[i]);
    }

    // Make sure the Modbus frame is at least long enough to process (minimum 12 bytes for Modbus TCP)
    if (len < 12) {
        ESP_LOGW(TAG, "Modbus request is too short to process.");
        return;
    }

    // Extract Modbus function code and request details
    uint8_t function_code = buf[7];  // Function code
    uint16_t register_address = (buf[9] << 8) | buf[10];  // Register address (big-endian)
    uint16_t num_registers = (buf[12] << 8) | buf[13];  // Number of registers (big-endian)

    // Log function code and details
    ESP_LOGD(TAG, "Modbus Request - Function Code: %d, Register Address: %d, Num Registers: %d",
             function_code, register_address, num_registers);

    // Handle based on function code
    switch (function_code) {
        case 3:  // Read Holding Registers
            // Validate register address and number of registers
            if (num_registers == 0) {
                ESP_LOGW(TAG, "No registers to read.");
                return;
            }

            // Here, you would process the read request and generate a response (you can simulate or interact with actual registers)
            // For example, sending a dummy response with a single register
            uint8_t response[256];
            response[0] = buf[0];  // Transaction ID
            response[1] = buf[1];
            response[2] = buf[2];  // Protocol ID
            response[3] = buf[3];
            response[4] = buf[4];  // Length (needs to be updated)
            response[5] = buf[5];
            response[6] = buf[6];
            response[7] = buf[7];  // Function Code (same as request)

            // Example of filling in a dummy register value (e.g., 0x1234)
            response[8] = num_registers * 2;  // Number of bytes to follow (2 bytes per register)
            response[9] = 0x12;  // First byte of register 0x1234
            response[10] = 0x34; // Second byte of register 0x1234

            // Send the response to the client (you might want to implement the actual sending logic)
            this->send_response(response, 11);  // Length of the response (dummy example)
            break;

        case 6:  // Write Single Register
            // Handle register write requests here (function code 0x06)
            ESP_LOGD(TAG, "Write Single Register Request.");
            break;

        default:
            ESP_LOGW(TAG, "Unsupported Modbus function code: %d", function_code);
            break;
    }
}

StreamServerComponent::Client::Client(std::unique_ptr<esphome::socket::Socket> socket, std::string identifier, size_t position)
    : socket(std::move(socket)), identifier{identifier}, position{position} {}
