#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace ccscoop {

using EntityId = std::uint64_t;
using RequestId = std::uint64_t;
using PlayerId = std::uint8_t;

constexpr PlayerId kHost = 0;
constexpr PlayerId kNobody = 0xff;

struct Faults {
    bool no_dedupe{};
    bool pickup_no_arbitration{};
    bool accept_stale_epoch{};
    bool accept_old_session{};
    bool join_omit_ownership{};
    bool handoff_commit_without_ack{};
    bool digest_omit_quests{};
};

struct ObjectState {
    EntityId id{};
    PlayerId owner{kHost};
    PlayerId holder{kNobody};
    std::uint32_t epoch{1};
    std::int32_t x_mm{};
};

struct WorldSnapshot {
    std::uint32_t session{};
    std::unordered_map<EntityId, ObjectState> objects;
    std::unordered_map<std::string, std::uint32_t> quests;
};

struct StateUpdate {
    std::uint32_t session{};
    EntityId id{};
    PlayerId sender{kNobody};
    std::uint32_t epoch{};
    std::int32_t x_mm{};
};

enum class ApplyResult {
    applied,
    duplicate,
    wrong_session,
    stale_epoch,
    future_epoch,
    not_owner,
    unknown_object,
};

struct ActionResult {
    bool accepted{};
    bool duplicate{};
    ObjectState state{};
};

class HostWorld {
public:
    explicit HostWorld(Faults faults = {});

    std::uint32_t session() const { return session_; }
    const ObjectState& add_object(EntityId id, std::int32_t x_mm);
    const ObjectState& object(EntityId id) const;

    ActionResult pickup(RequestId request, PlayerId player, EntityId id);
    ActionResult drop(RequestId request, PlayerId player, EntityId id, std::int32_t x_mm);
    bool quest_stage(RequestId request, std::string quest, std::uint32_t stage);
    std::uint32_t quest_stage(const std::string& quest) const;

    void load_new_session();
    WorldSnapshot snapshot() const;
    bool begin_handoff(EntityId id, PlayerId new_owner);
    bool acknowledge_handoff(EntityId id, PlayerId player);
    bool handoff_pending(EntityId id) const;
    std::uint64_t digest() const;

private:
    bool seen(RequestId request);

    Faults faults_{};
    std::uint32_t session_{1};
    std::unordered_map<EntityId, ObjectState> objects_;
    std::unordered_map<std::string, std::uint32_t> quests_;
    std::unordered_set<RequestId> requests_;
    struct Handoff { PlayerId previous; PlayerId next; std::uint32_t epoch; };
    std::unordered_map<EntityId, Handoff> handoffs_;
};

class Replica {
public:
    Replica(std::uint32_t session, Faults faults = {});

    void seed(const ObjectState& state);
    ApplyResult apply(const StateUpdate& update);
    const ObjectState& object(EntityId id) const;
    void start_session(std::uint32_t session);
    void load_snapshot(const WorldSnapshot& snapshot);
    std::uint32_t quest_stage(const std::string& quest) const;
    std::uint64_t digest() const;

private:
    Faults faults_{};
    std::uint32_t session_{};
    std::unordered_map<EntityId, ObjectState> objects_;
    std::unordered_map<std::string, std::uint32_t> quests_;
};

}  // namespace ccscoop
