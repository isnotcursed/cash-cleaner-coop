#include "ccscoop/net.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace ccscoop {

namespace {
constexpr std::array<std::uint8_t, 4> kMagic{{'C', 'C', 'S', 1}};
constexpr std::size_t kWireSize = 17;

void write_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<std::uint8_t>(value >> shift));
}

void write_u64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) out.push_back(static_cast<std::uint8_t>(value >> shift));
}

std::uint32_t read_u32(const std::uint8_t* p) {
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) value = (value << 8) | p[i];
    return value;
}

std::uint64_t read_u64(const std::uint8_t* p) {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) value = (value << 8) | p[i];
    return value;
}
}  // namespace

std::vector<std::uint8_t> encode_wire(const WireMessage& message) {
    std::vector<std::uint8_t> out(kMagic.begin(), kMagic.end());
    out.push_back(static_cast<std::uint8_t>(message.type));
    write_u32(out, message.session);
    write_u64(out, message.nonce);
    return out;
}

std::optional<WireMessage> decode_wire(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() != kWireSize || !std::equal(kMagic.begin(), kMagic.end(), bytes.begin())) return std::nullopt;
    const auto raw_type = bytes[4];
    if (raw_type < static_cast<std::uint8_t>(WireType::hello) ||
        raw_type > static_cast<std::uint8_t>(WireType::heartbeat)) return std::nullopt;
    return WireMessage{static_cast<WireType>(raw_type), read_u32(bytes.data() + 5), read_u64(bytes.data() + 9)};
}

struct UdpEndpoint::Impl {
#ifdef _WIN32
    SOCKET socket{INVALID_SOCKET};
    bool winsock{};
#else
    int socket{-1};
#endif
    std::uint16_t port{};
};

UdpEndpoint::UdpEndpoint(std::uint16_t local_port) : impl_(std::make_unique<Impl>()) {
#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return;
    impl_->winsock = true;
    impl_->socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (impl_->socket == INVALID_SOCKET) return;
    u_long nonblocking = 1;
    if (ioctlsocket(impl_->socket, FIONBIO, &nonblocking) != 0) return;
#else
    impl_->socket = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (impl_->socket < 0) return;
    const int flags = fcntl(impl_->socket, F_GETFL, 0);
    if (flags < 0 || fcntl(impl_->socket, F_SETFL, flags | O_NONBLOCK) < 0) return;
#endif
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(local_port);
    if (::bind(impl_->socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) return;
    socklen_t size = sizeof(address);
    if (getsockname(impl_->socket, reinterpret_cast<sockaddr*>(&address), &size) != 0) return;
    impl_->port = ntohs(address.sin_port);
}

UdpEndpoint::~UdpEndpoint() {
    if (!impl_) return;
#ifdef _WIN32
    if (impl_->socket != INVALID_SOCKET) closesocket(impl_->socket);
    if (impl_->winsock) WSACleanup();
#else
    if (impl_->socket >= 0) close(impl_->socket);
#endif
}

bool UdpEndpoint::valid() const { return impl_ && impl_->port != 0; }
std::uint16_t UdpEndpoint::local_port() const { return impl_ ? impl_->port : 0; }

bool UdpEndpoint::send(const std::string& address, std::uint16_t port,
                       const std::vector<std::uint8_t>& payload) {
    if (!valid() || payload.empty()) return false;
    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(port);
    if (inet_pton(AF_INET, address.c_str(), &destination.sin_addr) != 1) return false;
    const auto sent = ::sendto(impl_->socket, reinterpret_cast<const char*>(payload.data()),
                               static_cast<int>(payload.size()), 0,
                               reinterpret_cast<sockaddr*>(&destination), sizeof(destination));
    return sent == static_cast<decltype(sent)>(payload.size());
}

std::optional<Datagram> UdpEndpoint::receive() {
    if (!valid()) return std::nullopt;
    std::array<std::uint8_t, 2048> buffer{};
    sockaddr_in source{};
    socklen_t size = sizeof(source);
    const auto received = ::recvfrom(impl_->socket, reinterpret_cast<char*>(buffer.data()),
                                     static_cast<int>(buffer.size()), 0,
                                     reinterpret_cast<sockaddr*>(&source), &size);
    if (received <= 0) return std::nullopt;
    std::array<char, INET_ADDRSTRLEN> text{};
    if (!inet_ntop(AF_INET, &source.sin_addr, text.data(), text.size())) return std::nullopt;
    return Datagram{text.data(), ntohs(source.sin_port),
                    std::vector<std::uint8_t>(buffer.begin(), buffer.begin() + received)};
}

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
