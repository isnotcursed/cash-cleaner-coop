#include "ccscoop/model.h"

#include <algorithm>
#include <stdexcept>

namespace ccscoop {

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
}

}  // namespace ccscoop
