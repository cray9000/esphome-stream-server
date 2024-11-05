class StreamServerComponent : public esphome::Component {
public:
    StreamServerComponent() = default;

    void setup() override;
    void loop() override;
    void dump_config() override;
    void on_shutdown() override;
    float get_setup_priority() const override { return esphome::setup_priority::AFTER_WIFI; }

    void set_port(uint16_t port) { this->port_ = port; }

#ifdef USE_BINARY_SENSOR
    void set_connected_sensor(esphome::binary_sensor::BinarySensor *connected) { this->connected_sensor_ = connected; }
#endif
#ifdef USE_SENSOR
    void set_connection_count_sensor(esphome::sensor::Sensor *connection_count) { this->connection_count_sensor_ = connection_count; }
#endif

protected:
    void publish_sensor();

    void accept();
    void cleanup();
    void read();
    void flush();
    void write();

    struct Client {
        Client(std::unique_ptr<esphome::socket::Socket> socket, std::string identifier, size_t position);

        std::unique_ptr<esphome::socket::Socket> socket{nullptr};
        std::string identifier{};
        bool disconnected{false};
        size_t position{0};
    };

    uint16_t port_;
#ifdef USE_BINARY_SENSOR
    esphome::binary_sensor::BinarySensor *connected_sensor_;
#endif
#ifdef USE_SENSOR
    esphome::sensor::Sensor *connection_count_sensor_;
#endif

    std::unique_ptr<esphome::socket::Socket> socket_{};
    std::vector<Client> clients_{};
};
