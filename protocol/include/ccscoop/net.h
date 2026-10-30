#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ccscoop {

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
