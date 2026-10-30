#include "ccscoop/net.h"

#include <stdexcept>
#include <string>
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

template <class Scenario>
bool caught(const Scenario& scenario) {
    try { scenario(); return false; } catch (const std::exception&) { return true; }
}

}  // namespace

int run_net_tests() {
    loss_retransmit();
    duplicates_are_deduped();
    require_net(caught([] { loss_retransmit(NetFaults{true, false}); }),
                "mutation twin no_retransmit escaped");
    require_net(caught([] { duplicates_are_deduped(NetFaults{false, true}); }),
                "mutation twin no_receive_dedupe escaped");
    return 0;
}
