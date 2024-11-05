#include "stream_server.h"

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/core/util.h"
#include "esphome/core/version.h"

#include "esphome/components/network/util.h"
#include "esphome/components/socket/socket.h"

#include "esphome/core/log.h"  // Ensure you include the logging header

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
    int available;
    // There is no UART stream anymore; this function now doesn't do anything.
    // You can replace it with a custom data source if necessary (e.g., network, file).
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
    uint8_t buf[128];
    ssize_t read;
    
    // Assuming len is the number of bytes you're reading from the client socket
    for (Client &client : this->clients_) {
        if (client.disconnected)
            continue;

        while ((read = client.socket->read(&buf, sizeof(buf))) > 0) {
            // Do something with the data
            // For example, send it to another output device
            
            // Set len to the number of bytes read from the client
            size_t len = read;  // This defines the number of bytes to process

            // Log the size of the data in the buffer
            ESP_LOGD(TAG, "Buffer data (size: %d):", len);

            // Assuming this is the read logic
            this->received_data_.insert(this->received_data_.end(), buf, buf + read);

            // Add more logic here to send the data to another device, process it, etc.
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


// Log the received data in a human-readable format (hex)
void StreamServerComponent::log_received_data() {
    ESP_LOGD(TAG, "Logging received data...");  // Confirm method is being called
    
    // Check if the received data container is empty
    if (received_data_.empty()) {
        ESP_LOGD(TAG, "No data to log, container is empty.");
        return;  // Exit if there's no data
    }

    // Get the size of the received data
    size_t bytes_to_log = std::min(received_data_.size(), size_t(128));  // Limit to first 128 bytes
    ESP_LOGD(TAG, "Received data size: %zu", bytes_to_log);  // Log the number of bytes to be logged

    // Loop through the received data and log it in hexadecimal format
    for (size_t i = 0; i < bytes_to_log; ++i) {
        // Log each byte directly
        ESP_LOGD(TAG, "Byte %zu: %02X", i, received_data_[i]);
    }

    // Now log the complete buffer as a single hex string
    std::string log_message = "Received data: ";
    for (size_t i = 0; i < bytes_to_log; ++i) {
        // Convert each byte to a 2-digit hex representation
        char byte_str[4];  // "XX " (2 characters for hex + space)
        snprintf(byte_str, sizeof(byte_str), "%02X ", received_data_[i]);
        log_message += byte_str;
    }

    // Log the complete message using ESPHome's logging system
    ESP_LOGD(TAG, "%s", log_message.c_str());
}

StreamServerComponent::Client::Client(std::unique_ptr<esphome::socket::Socket> socket, std::string identifier, size_t position)
    : socket(std::move(socket)), identifier{identifier}, position{position} {}
