#include "stream_server.h"

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/core/version.h"

#include "esphome/components/network/util.h"
#include "esphome/components/socket/socket.h"
// #include "esphome/core/select/select.h" // For select() to check if data is available for reading

static const char *TAG = "stream_server";

using namespace esphome;

void StreamServerComponent::setup() {
    ESP_LOGCONFIG(TAG, "Setting up stream server...");

    // Initialize the buffer for storing data
    this->buf_ = std::make_unique<uint8_t[]>(this->buf_size_);

    struct sockaddr_storage bind_addr;
    socklen_t bind_addrlen = socket::set_sockaddr_any(reinterpret_cast<struct sockaddr *>(&bind_addr), sizeof(bind_addr), this->port_);

    this->server_socket_ = socket::socket_ip(SOCK_STREAM, PF_INET);
    this->server_socket_->setblocking(false);
    this->server_socket_->bind(reinterpret_cast<struct sockaddr *>(&bind_addr), bind_addrlen);
    this->server_socket_->listen(8);  // Listen for incoming client connections

    this->publish_sensor();  // Publish sensor state if needed
}

void StreamServerComponent::loop() {
    this->accept();  // Accept new client connections
    this->read();    // Read data from clients
    this->flush();   // Flush data to clients
    this->write();   // Write data back to the stream
    this->cleanup(); // Clean up any disconnected clients
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
    std::unique_ptr<socket::Socket> client_socket = this->server_socket_->accept(reinterpret_cast<struct sockaddr *>(&client_addr), &client_addrlen);

    if (!client_socket)
        return;

    client_socket->setblocking(false);
    std::string identifier = client_socket->getpeername();
    this->clients_.emplace_back(std::move(client_socket), identifier, this->buf_head_);
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
    
    // Use select() to check if the socket has data ready to be read
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(this->server_socket_->get_fd(), &read_fds);

    struct timeval timeout = { 0, 0 }; // No timeout, we just want to check if data is ready
    int result = select(this->server_socket_->get_fd() + 1, &read_fds, nullptr, nullptr, &timeout);

    if (result > 0 && FD_ISSET(this->server_socket_->get_fd(), &read_fds)) {
        // If the socket is readable, we try to read data
        while ((available = this->server_socket_->read(&this->buf_[this->buf_index(this->buf_head_)], this->buf_size_)) > 0) {
            size_t free = this->buf_size_ - (this->buf_head_ - this->buf_tail_);
            if (free == 0) {
                // If buffer is full, handle accordingly (could overwrite or flush)
                if (len > 0)
                    return;

                ESP_LOGE(TAG, "Incoming bytes available, but outgoing buffer is full: stream will be corrupted!");
                free = std::min<size_t>(available, this->buf_size_);
                this->buf_tail_ += free;
                for (Client &client : this->clients_) {
                    if (client.position < this->buf_tail_) {
                        ESP_LOGW(TAG, "Dropped %u pending bytes for client %s", this->buf_tail_ - client.position, client.identifier.c_str());
                        client.position = this->buf_tail_;
                    }
                }
            }

            len = std::min<size_t>(available, std::min<size_t>(this->buf_ahead(this->buf_head_), free));
            this->buf_head_ += len;
        }
    }
    else if (result == 0) {
        // No data available at the moment
        ESP_LOGD(TAG, "No data available to read");
    }
    else {
        // An error occurred during select(), handle it
        ESP_LOGE(TAG, "Error in select() for socket: %d", errno);
    }
}

void StreamServerComponent::flush() {
    ssize_t written;
    this->buf_tail_ = this->buf_head_;
    for (Client &client : this->clients_) {
        if (client.disconnected || client.position == this->buf_head_)
            continue;

        struct iovec iov[2];
        iov[0].iov_base = &this->buf_[this->buf_index(client.position)];
        iov[0].iov_len = std::min(this->buf_head_ - client.position, this->buf_ahead(client.position));
        iov[1].iov_base = &this->buf_[0];
        iov[1].iov_len = this->buf_head_ - (client.position + iov[0].iov_len);
        if ((written = client.socket->writev(iov, 2)) > 0) {
            client.position += written;
        } else if (written == 0 || errno == ECONNRESET) {
            ESP_LOGD(TAG, "Client %s disconnected", client.identifier.c_str());
            client.disconnected = true;
            continue;
        } else if (errno == EWOULDBLOCK || errno == EAGAIN) {
        } else {
            ESP_LOGE(TAG, "Failed to write to client %s with error %d!", client.identifier.c_str(), errno);
        }

        this->buf_tail_ = std::min(this->buf_tail_, client.position);
    }
}

void StreamServerComponent::write() {
    uint8_t buf[128];
    ssize_t read;
    for (Client &client : this->clients_) {
        if (client.disconnected)
            continue;

        while ((read = client.socket->read(&buf, sizeof(buf))) > 0)
            this->server_socket_->write_array(buf, read);

        if (read == 0 || errno == ECONNRESET) {
            ESP_LOGD(TAG, "Client %s disconnected", client.identifier.c_str());
            client.disconnected = true;
        } else if (errno == EWOULDBLOCK || errno == EAGAIN) {
        } else {
            ESP_LOGW(TAG, "Failed to read from client %s with error %d!", client.identifier.c_str(), errno);
        }
    }
}

StreamServerComponent::Client::Client(std::unique_ptr<esphome::socket::Socket> socket, std::string identifier, size_t position)
    : socket(std::move(socket)), identifier{identifier}, position{position} {}
