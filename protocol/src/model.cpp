#include "ccscoop/model.h"

#include <algorithm>
#include <stdexcept>
#include <vector>

namespace ccscoop {

namespace {
void hash_bytes(std::uint64_t& hash, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }
}

std::uint64_t world_digest(std::uint32_t session,
                           const std::unordered_map<EntityId, ObjectState>& objects,
                           const std::unordered_map<std::string, std::uint32_t>& quests,
                           bool omit_quests) {
    std::uint64_t hash = 1469598103934665603ULL;
    hash_bytes(hash, &session, sizeof(session));
    std::vector<EntityId> ids;
    for (const auto& [id, state] : objects) { (void)state; ids.push_back(id); }
    std::sort(ids.begin(), ids.end());
    for (const auto id : ids) {
        const auto& s = objects.at(id);
        hash_bytes(hash, &s.id, sizeof(s.id));
        hash_bytes(hash, &s.owner, sizeof(s.owner));
        hash_bytes(hash, &s.holder, sizeof(s.holder));
        hash_bytes(hash, &s.epoch, sizeof(s.epoch));
        hash_bytes(hash, &s.x_mm, sizeof(s.x_mm));
    }
    if (!omit_quests) {
        std::vector<std::string> names;
        for (const auto& [name, stage] : quests) { (void)stage; names.push_back(name); }
        std::sort(names.begin(), names.end());
        for (const auto& name : names) {
            hash_bytes(hash, name.data(), name.size());
            const auto stage = quests.at(name);
            hash_bytes(hash, &stage, sizeof(stage));
        }
    }
    return hash;
}
}  // namespace

HostWorld::HostWorld(Faults faults) : faults_(faults) {}

const ObjectState& HostWorld::add_object(EntityId id, std::int32_t x_mm) {
    const auto [it, inserted] = objects_.emplace(id, ObjectState{id, kHost, kNobody, 1, x_mm});
    if (!inserted) {
        throw std::logic_error("duplicate entity id");
    }
    return it->second;
}

const ObjectState& HostWorld::object(EntityId id) const {
    return objects_.at(id);
}

bool HostWorld::seen(RequestId request) {
    if (faults_.no_dedupe) {
        return false;
    }
    return !requests_.insert(request).second;
}

ActionResult HostWorld::pickup(RequestId request, PlayerId player, EntityId id) {
    auto& state = objects_.at(id);
    if (seen(request)) {
        return {false, true, state};
    }
    if (state.holder != kNobody && !faults_.pickup_no_arbitration) {
        return {false, false, state};
    }
    state.holder = player;
    state.owner = player;
    ++state.epoch;
    return {true, false, state};
}

ActionResult HostWorld::drop(RequestId request, PlayerId player, EntityId id, std::int32_t x_mm) {
    auto& state = objects_.at(id);
    if (seen(request)) {
        return {false, true, state};
    }
    if (state.holder != player) {
        return {false, false, state};
    }
    state.holder = kNobody;
    state.owner = kHost;
    state.x_mm = x_mm;
    ++state.epoch;
    return {true, false, state};
}

bool HostWorld::quest_stage(RequestId request, std::string quest, std::uint32_t stage) {
    if (seen(request)) {
        return false;
    }
    auto& current = quests_[std::move(quest)];
    current = std::max(current, stage);
    return true;
}

std::uint32_t HostWorld::quest_stage(const std::string& quest) const {
    const auto it = quests_.find(quest);
    return it == quests_.end() ? 0 : it->second;
}

void HostWorld::load_new_session() {
    ++session_;
    requests_.clear();
    for (auto& [id, state] : objects_) {
        (void)id;
        state.owner = kHost;
        state.holder = kNobody;
        state.epoch = 1;
    }
}

WorldSnapshot HostWorld::snapshot() const {
    auto objects = objects_;
    if (faults_.join_omit_ownership) {
        for (auto& [id, state] : objects) {
            (void)id;
            state.owner = kHost;
            state.epoch = 1;
        }
    }
    return WorldSnapshot{session_, std::move(objects), quests_};
}

bool HostWorld::begin_handoff(EntityId id, PlayerId new_owner) {
    auto& state = objects_.at(id);
    if (new_owner == state.owner || handoffs_.count(id)) return false;
    const Handoff pending{state.owner, new_owner, state.epoch + 1};
    handoffs_.emplace(id, pending);
    if (faults_.handoff_commit_without_ack) {
        state.owner = new_owner;
        state.epoch = pending.epoch;
        handoffs_.erase(id);
    }
    return true;
}

bool HostWorld::acknowledge_handoff(EntityId id, PlayerId player) {
    const auto it = handoffs_.find(id);
    if (it == handoffs_.end() || it->second.next != player) return false;
    auto& state = objects_.at(id);
    state.owner = it->second.next;
    state.epoch = it->second.epoch;
    handoffs_.erase(it);
    return true;
}

bool HostWorld::handoff_pending(EntityId id) const {
    return handoffs_.count(id) != 0;
}

std::uint64_t HostWorld::digest() const {
    return world_digest(session_, objects_, quests_, faults_.digest_omit_quests);
}

Replica::Replica(std::uint32_t session, Faults faults) : faults_(faults), session_(session) {}

void Replica::seed(const ObjectState& state) {
    objects_[state.id] = state;
}

ApplyResult Replica::apply(const StateUpdate& update) {
    if (update.session != session_ && !faults_.accept_old_session) {
        return ApplyResult::wrong_session;
    }
    auto it = objects_.find(update.id);
    if (it == objects_.end()) {
        return ApplyResult::unknown_object;
    }
    auto& state = it->second;
    if (update.epoch < state.epoch && !faults_.accept_stale_epoch) {
        return ApplyResult::stale_epoch;
    }
    if (update.epoch > state.epoch) {
        return ApplyResult::future_epoch;
    }
    if (update.sender != state.owner) {
        return ApplyResult::not_owner;
    }
    if (state.x_mm == update.x_mm) {
        return ApplyResult::duplicate;
    }
    state.x_mm = update.x_mm;
    return ApplyResult::applied;
}

const ObjectState& Replica::object(EntityId id) const {
    return objects_.at(id);
}

void Replica::start_session(std::uint32_t session) {
    session_ = session;
    objects_.clear();
    quests_.clear();
}

void Replica::load_snapshot(const WorldSnapshot& snapshot) {
    session_ = snapshot.session;
    objects_ = snapshot.objects;
    quests_ = snapshot.quests;
}

std::uint32_t Replica::quest_stage(const std::string& quest) const {
    const auto it = quests_.find(quest);
    return it == quests_.end() ? 0 : it->second;
}

std::uint64_t Replica::digest() const {
    return world_digest(session_, objects_, quests_, faults_.digest_omit_quests);
}

}  // namespace ccscoop
