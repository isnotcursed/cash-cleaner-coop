#include "ccscoop/net.h"

#include <chrono>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace ccscoop;

namespace {

void require_net(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

std::vector<std::string> run_link(NetFaults faults, LinkConfig config) {
    ReliableSender sender(faults);
    ReliableReceiver receiver(faults);
    VirtualLink forward(config, 7);
    VirtualLink reverse(LinkConfig{5, 5, 0, 0, false}, 11);
    std::vector<std::string> delivered;

    for (const char* value : {"pickup", "drop", "quest"}) {
        forward.send(sender.send(value, 0), 0);
    }

    for (std::uint64_t now = 0; now <= 1000; now += 5) {
        for (const auto& packet : sender.tick(now)) forward.send(packet, now);
        for (const auto& packet : forward.deliver(now)) {
            auto [ack, ready] = receiver.receive(packet);
            reverse.send(ack, now);
            delivered.insert(delivered.end(), ready.begin(), ready.end());
        }
        for (const auto& ack : reverse.deliver(now)) sender.acknowledge(ack.seq);
        if (sender.idle() && forward.empty() && reverse.empty()) break;
    }
    return delivered;
}

void loss_retransmit(NetFaults faults = {}) {
    const auto got = run_link(faults, LinkConfig{10, 25, 0, 0, true});
    require_net(got == std::vector<std::string>({"pickup", "drop", "quest"}),
                "retransmission did not recover ordered messages");
}

void duplicates_are_deduped(NetFaults faults = {}) {
    const auto got = run_link(faults, LinkConfig{5, 30, 0, 1, false});
    require_net(got == std::vector<std::string>({"pickup", "drop", "quest"}),
                "duplicates were delivered to the application");
}

void wire_codec_rejects_bad_packets() {
    const WireMessage expected{WireType::welcome, 0x10203040u, 0x0102030405060708ull};
    const auto encoded = encode_wire(expected);
    require_net(encoded.size() == 17, "wire packet size changed");
    const auto decoded = decode_wire(encoded);
    require_net(decoded && *decoded == expected, "wire packet did not round-trip");

    auto bad_magic = encoded;
    bad_magic[0] ^= 0xff;
    require_net(!decode_wire(bad_magic), "bad wire magic was accepted");
    auto bad_type = encoded;
    bad_type[4] = 99;
    require_net(!decode_wire(bad_type), "bad wire type was accepted");
    auto truncated = encoded;
    truncated.pop_back();
    require_net(!decode_wire(truncated), "truncated wire packet was accepted");
}

void udp_loopback_round_trip() {
    UdpEndpoint host(0);
    UdpEndpoint guest(0);
    require_net(host.valid() && guest.valid(), "could not open UDP endpoints");
    const WireMessage hello{WireType::hello, 0, 42};
    require_net(guest.send("127.0.0.1", host.local_port(), encode_wire(hello)),
                "could not send UDP datagram");

    std::optional<Datagram> received;
    for (int attempt = 0; attempt < 100 && !received; ++attempt) {
        received = host.receive();
        if (!received) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require_net(received.has_value(), "UDP datagram was not received");
    const auto decoded = decode_wire(received->payload);
    require_net(decoded && *decoded == hello, "UDP payload was corrupted");
    require_net(received->address == "127.0.0.1" && received->port == guest.local_port(),
                "UDP source address was wrong");
}

void player_transform_codec_is_bounded() {
    const PlayerTransform expected{7, 2, 99, 123.25f, -456.5f, 78.0f, 179.75f};
    const auto encoded = encode_player_transform(expected);
    require_net(encoded.size() == 33, "player transform size changed");
    const auto decoded = decode_player_transform(encoded);
    require_net(decoded && *decoded == expected, "player transform did not round-trip");

    auto wrong_type = encoded;
    wrong_type[4] = static_cast<std::uint8_t>(WireType::heartbeat);
    require_net(!decode_player_transform(wrong_type), "wrong transform type was accepted");
    require_net(!decode_player_transform(encode_player_transform(
                    {0, 2, 99, 1.0f, 2.0f, 3.0f, 4.0f})),
                "zero session transform was accepted");
    require_net(!decode_player_transform(encode_player_transform(
                    {7, 2, 99, std::numeric_limits<float>::quiet_NaN(), 2.0f, 3.0f, 4.0f})),
                "NaN transform was accepted");
    require_net(!decode_player_transform(encode_player_transform(
                    {7, 2, 99, 200000000.0f, 2.0f, 3.0f, 4.0f})),
                "unbounded transform was accepted");
}

template <class Scenario>
bool caught(const Scenario& scenario) {
    try { scenario(); return false; } catch (const std::exception&) { return true; }
}

}  // namespace

int run_net_tests() {
    loss_retransmit();
    duplicates_are_deduped();
    wire_codec_rejects_bad_packets();
    udp_loopback_round_trip();
    player_transform_codec_is_bounded();
    require_net(caught([] { loss_retransmit(NetFaults{true, false}); }),
                "mutation twin no_retransmit escaped");
    require_net(caught([] { duplicates_are_deduped(NetFaults{false, true}); }),
                "mutation twin no_receive_dedupe escaped");
    return 0;
}
