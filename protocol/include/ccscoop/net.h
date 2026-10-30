#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ccscoop {

enum class WireType : std::uint8_t {
    hello = 1, welcome = 2, heartbeat = 3, player_transform = 4, object_action = 5,
    object_transform = 6
};

struct WireMessage {
    WireType type{WireType::hello};
    std::uint32_t session{};
    std::uint64_t nonce{};

    bool operator==(const WireMessage& other) const {
        return type == other.type && session == other.session && nonce == other.nonce;
    }
};

std::vector<std::uint8_t> encode_wire(const WireMessage& message);
std::optional<WireMessage> decode_wire(const std::vector<std::uint8_t>& bytes);

struct PlayerTransform {
    std::uint32_t session{};
    std::uint32_t player{};
    std::uint32_t sequence{};
    float x{};
    float y{};
    float z{};
    float yaw{};

    bool operator==(const PlayerTransform& other) const {
        return session == other.session && player == other.player && sequence == other.sequence &&
               x == other.x && y == other.y && z == other.z && yaw == other.yaw;
    }
};

std::vector<std::uint8_t> encode_player_transform(const PlayerTransform& transform);
std::optional<PlayerTransform> decode_player_transform(const std::vector<std::uint8_t>& bytes);

enum class ObjectActionKind : std::uint8_t {
    pickup_request = 1,
    pickup_granted = 2,
    pickup_denied = 3,
    drop = 4,
};

struct ObjectAction {
    ObjectActionKind kind{ObjectActionKind::pickup_request};
    std::uint32_t session{};
    std::uint32_t request{};
    std::uint64_t object{};
    std::uint32_t player{};
    std::uint32_t epoch{};

    bool operator==(const ObjectAction& other) const {
        return kind == other.kind && session == other.session && request == other.request &&
               object == other.object && player == other.player && epoch == other.epoch;
    }
};

std::vector<std::uint8_t> encode_object_action(const ObjectAction& action);
std::optional<ObjectAction> decode_object_action(const std::vector<std::uint8_t>& bytes);

struct ObjectTransform {
    std::uint32_t session{};
    std::uint64_t object{};
    std::uint32_t player{};
    std::uint32_t epoch{};
    std::uint32_t sequence{};
    float x{};
    float y{};
    float z{};
    float qx{};
    float qy{};
    float qz{};
    float qw{1.0f};
    float vx{};
    float vy{};
    float vz{};
    float wx{};
    float wy{};
    float wz{};

    bool operator==(const ObjectTransform& other) const = default;
};

std::vector<std::uint8_t> encode_object_transform(const ObjectTransform& transform);
std::optional<ObjectTransform> decode_object_transform(const std::vector<std::uint8_t>& bytes);

class ObjectTransformOrder {
public:
    bool should_apply(const ObjectTransform& transform);
    void clear();

private:
    struct Cursor { std::uint32_t epoch{}; std::uint32_t sequence{}; };
    std::unordered_map<std::uint64_t, Cursor> cursors_;
};

struct Datagram {
    std::string address;
    std::uint16_t port{};
    std::vector<std::uint8_t> payload;
};

class UdpEndpoint {
public:
    explicit UdpEndpoint(std::uint16_t local_port = 0);
    ~UdpEndpoint();
    UdpEndpoint(const UdpEndpoint&) = delete;
    UdpEndpoint& operator=(const UdpEndpoint&) = delete;

    bool valid() const;
    std::uint16_t local_port() const;
    bool send(const std::string& address, std::uint16_t port,
              const std::vector<std::uint8_t>& payload);
    std::optional<Datagram> receive();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class ConnectionWatchdog {
public:
    explicit ConnectionWatchdog(std::uint64_t timeout_ms = 3000) : timeout_ms_(timeout_ms) {}
    void connect(std::uint64_t now_ms) { connected_ = true; last_activity_ms_ = now_ms; }
    void activity(std::uint64_t now_ms) { if (connected_) last_activity_ms_ = now_ms; }
    void disconnect() { connected_ = false; }
    bool connected() const { return connected_; }
    bool expired(std::uint64_t now_ms) const {
        return connected_ && now_ms - last_activity_ms_ >= timeout_ms_;
    }

private:
    std::uint64_t timeout_ms_;
    std::uint64_t last_activity_ms_{};
    bool connected_{};
};

struct Packet {
    std::uint32_t seq{};
    bool ack{};
    std::string payload;
};

struct NetFaults {
    bool no_retransmit{};
    bool no_receive_dedupe{};
};

class ReliableSender {
public:
    explicit ReliableSender(NetFaults faults = {});
    Packet send(std::string payload, std::uint64_t now_ms);
    void acknowledge(std::uint32_t seq);
    std::vector<Packet> tick(std::uint64_t now_ms, std::uint64_t retry_ms = 50);
    bool idle() const { return pending_.empty(); }

private:
    struct Pending { Packet packet; std::uint64_t sent_at{}; };
    NetFaults faults_{};
    std::uint32_t next_seq_{1};
    std::map<std::uint32_t, Pending> pending_;
};

class ReliableReceiver {
public:
    explicit ReliableReceiver(NetFaults faults = {});
    std::pair<Packet, std::vector<std::string>> receive(const Packet& packet);

private:
    NetFaults faults_{};
    std::uint32_t next_seq_{1};
    std::map<std::uint32_t, std::string> buffered_;
};

struct LinkConfig {
    std::uint64_t latency_ms{15};
    std::uint64_t jitter_ms{20};
    std::uint32_t drop_every{};
    std::uint32_t duplicate_every{};
    bool drop_first_attempt{};
};

class VirtualLink {
public:
    explicit VirtualLink(LinkConfig config = {}, std::uint32_t seed = 1);
    void send(const Packet& packet, std::uint64_t now_ms);
    std::vector<Packet> deliver(std::uint64_t now_ms);
    bool empty() const { return queue_.empty(); }

private:
    struct Queued { std::uint64_t at{}; std::uint64_t order{}; Packet packet; };
    std::uint32_t random();

    LinkConfig config_{};
    std::uint32_t rng_{};
    std::uint32_t sends_{};
    std::uint64_t order_{};
    std::map<std::uint32_t, std::uint32_t> attempts_;
    std::vector<Queued> queue_;
};

}  // namespace ccscoop
