#include "ccscoop/net.h"

#include <algorithm>
#include <utility>

namespace ccscoop {

ReliableSender::ReliableSender(NetFaults faults) : faults_(faults) {}

Packet ReliableSender::send(std::string payload, std::uint64_t now_ms) {
    Packet packet{next_seq_++, false, std::move(payload)};
    pending_[packet.seq] = Pending{packet, now_ms};
    return packet;
}

void ReliableSender::acknowledge(std::uint32_t seq) {
    pending_.erase(seq);
}

std::vector<Packet> ReliableSender::tick(std::uint64_t now_ms, std::uint64_t retry_ms) {
    std::vector<Packet> out;
    if (faults_.no_retransmit) return out;
    for (auto& [seq, pending] : pending_) {
        (void)seq;
        if (now_ms - pending.sent_at >= retry_ms) {
            pending.sent_at = now_ms;
            out.push_back(pending.packet);
        }
    }
    return out;
}

ReliableReceiver::ReliableReceiver(NetFaults faults) : faults_(faults) {}

std::pair<Packet, std::vector<std::string>> ReliableReceiver::receive(const Packet& packet) {
    Packet ack{packet.seq, true, {}};
    std::vector<std::string> ready;
    if (packet.ack) return {ack, ready};

    if (faults_.no_receive_dedupe) {
        ready.push_back(packet.payload);
        return {ack, ready};
    }
    if (packet.seq < next_seq_) return {ack, ready};
    buffered_.emplace(packet.seq, packet.payload);
    while (true) {
        const auto it = buffered_.find(next_seq_);
        if (it == buffered_.end()) break;
        ready.push_back(std::move(it->second));
        buffered_.erase(it);
        ++next_seq_;
    }
    return {ack, ready};
}

VirtualLink::VirtualLink(LinkConfig config, std::uint32_t seed)
    : config_(config), rng_(seed ? seed : 1) {}

std::uint32_t VirtualLink::random() {
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return rng_;
}

void VirtualLink::send(const Packet& packet, std::uint64_t now_ms) {
    ++sends_;
    const auto attempt = ++attempts_[packet.seq];
    if (config_.drop_first_attempt && attempt == 1) return;
    if (config_.drop_every && sends_ % config_.drop_every == 0) return;

    const auto copies = (config_.duplicate_every && sends_ % config_.duplicate_every == 0) ? 2 : 1;
    for (int copy = 0; copy < copies; ++copy) {
        const auto jitter = config_.jitter_ms ? random() % (config_.jitter_ms + 1) : 0;
        queue_.push_back(Queued{now_ms + config_.latency_ms + jitter, order_++, packet});
    }
}

std::vector<Packet> VirtualLink::deliver(std::uint64_t now_ms) {
    std::stable_sort(queue_.begin(), queue_.end(), [](const Queued& a, const Queued& b) {
        if (a.at != b.at) return a.at < b.at;
        return a.order < b.order;
    });
    std::vector<Packet> out;
    auto split = queue_.begin();
    while (split != queue_.end() && split->at <= now_ms) {
        out.push_back(split->packet);
        ++split;
    }
    queue_.erase(queue_.begin(), split);
    return out;
}

}  // namespace ccscoop
