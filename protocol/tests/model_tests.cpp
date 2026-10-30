#include "ccscoop/model.h"

#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace ccscoop;

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void pickup_race(Faults faults = {}) {
    HostWorld host(faults);
    host.add_object(10, 0);
    const auto first = host.pickup(100, 1, 10);
    const auto second = host.pickup(101, 2, 10);
    require(first.accepted, "first pickup must win");
    require(!second.accepted, "second pickup must lose");
    require(host.object(10).holder == 1, "winner must remain holder");
}

void duplicate_drop(Faults faults = {}) {
    HostWorld host(faults);
    host.add_object(10, 0);
    require(host.pickup(100, 1, 10).accepted, "pickup failed");
    const auto first = host.drop(200, 1, 10, 500);
    const auto duplicate = host.drop(200, 1, 10, 900);
    require(first.accepted, "drop failed");
    require(duplicate.duplicate, "duplicate request not recognized");
    require(host.object(10).x_mm == 500, "duplicate moved object twice");
}

void stale_owner_update(Faults faults = {}) {
    ObjectState state{10, 2, kNobody, 4, 100};
    Replica replica(1, faults);
    replica.seed(state);
    const auto result = replica.apply(StateUpdate{1, 10, 1, 3, 999});
    require(result == ApplyResult::stale_epoch, "stale epoch was accepted");
    require(replica.object(10).x_mm == 100, "stale owner changed state");
}

void old_session_update(Faults faults = {}) {
    ObjectState state{10, 1, kNobody, 1, 100};
    Replica replica(2, faults);
    replica.seed(state);
    const auto result = replica.apply(StateUpdate{1, 10, 1, 1, 999});
    require(result == ApplyResult::wrong_session, "old session was accepted");
    require(replica.object(10).x_mm == 100, "old session changed state");
}

void quest_is_monotonic_and_idempotent() {
    HostWorld host;
    require(host.quest_stage(300, "first_contract", 3), "quest effect rejected");
    require(!host.quest_stage(300, "first_contract", 4), "duplicate effect applied");
    require(host.quest_stage(301, "first_contract", 2), "older stage event rejected");
    require(host.quest_stage("first_contract") == 3, "quest stage went backwards");
}

bool catches(const std::function<void()>& scenario) {
    try {
        scenario();
        return false;
    } catch (const std::exception&) {
        return true;
    }
}

}  // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"pickup_race", [] { pickup_race(); }},
        {"duplicate_drop", [] { duplicate_drop(); }},
        {"stale_owner_update", [] { stale_owner_update(); }},
        {"old_session_update", [] { old_session_update(); }},
        {"quest_idempotency", quest_is_monotonic_and_idempotent},
    };

    int failed = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            ++failed;
            std::cout << "FAIL " << name << ": " << error.what() << '\n';
        }
    }

    struct Twin { const char* name; std::function<void()> run; };
    const std::vector<Twin> twins = {
        {"pickup_no_arbitration", [] { pickup_race(Faults{false, true, false, false}); }},
        {"no_dedupe", [] { duplicate_drop(Faults{true, false, false, false}); }},
        {"accept_stale_epoch", [] { stale_owner_update(Faults{false, false, true, false}); }},
        {"accept_old_session", [] { old_session_update(Faults{false, false, false, true}); }},
    };
    for (const auto& twin : twins) {
        if (catches(twin.run)) {
            std::cout << "CAUGHT " << twin.name << '\n';
        } else {
            ++failed;
            std::cout << "MISSED " << twin.name << '\n';
        }
    }

    std::cout << tests.size() << " scenarios, " << twins.size() << " twins, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
