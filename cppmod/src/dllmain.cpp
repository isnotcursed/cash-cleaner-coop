#include <DynamicOutput/DynamicOutput.hpp>
#include <Mod/CppUserModBase.hpp>
#include <Unreal/AActor.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/UFunctionStructs.hpp>
#include <Unreal/UnrealCoreStructs.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/World.hpp>
#include <ccscoop/net.h>

#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    enum class CoopMode { passive, host, join };

    auto env_port() -> std::uint16_t
    {
        const auto* value = std::getenv("CCSCOOP_PORT");
        if (!value) return 24883;
        const auto parsed = std::strtoul(value, nullptr, 10);
        return parsed > 0 && parsed <= 65535 ? static_cast<std::uint16_t>(parsed) : 24883;
    }

    auto now_ms() -> std::uint64_t
    {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    auto stable_object_id(RC::Unreal::UObject* object) -> std::uint64_t
    {
        if (!object) return 0;
        std::uint64_t hash = 14695981039346656037ull;
        const auto hash_text = [&hash](const auto& text)
        {
            for (const auto character : text)
            {
                hash ^= static_cast<std::uint64_t>(character);
                hash *= 1099511628211ull;
            }
        };
        if (auto* unique_id = object->GetValuePtrByPropertyNameInChain<RC::Unreal::FGuid>(STR("UniqueID"));
            unique_id && (unique_id->A || unique_id->B || unique_id->C || unique_id->D))
        {
            hash_text(object->GetClassPrivate()->GetFullName());
            for (const auto part : {unique_id->A, unique_id->B, unique_id->C, unique_id->D})
            {
                for (int shift = 0; shift < 32; shift += 8)
                {
                    hash ^= static_cast<std::uint8_t>(part >> shift);
                    hash *= 1099511628211ull;
                }
            }
        }
        else
        {
            hash_text(object->GetFullName());
        }
        return hash == 0 ? 1 : hash;
    }

    class CashCleanerCoopMod final : public RC::CppUserModBase
    {
      public:
        CashCleanerCoopMod()
        {
            ModName = STR("CashCleanerCoop");
            ModVersion = STR("0.1.0-dev");
            ModDescription = STR("Experimental co-op bootstrap for Cash Cleaner Simulator");
            ModAuthors = STR("Cash Cleaner Co-op contributors");

            RC::Output::send(STR("[CashCleanerCoop] C++ bootstrap loaded\n"));

            const auto* mode = std::getenv("CCSCOOP_MODE");
            const auto* ghost = std::getenv("CCSCOOP_GHOST");
            ghost_enabled_ = ghost && std::string(ghost) == "1";
            if (mode && std::string(mode) == "host")
            {
                mode_ = CoopMode::host;
                endpoint_ = std::make_unique<ccscoop::UdpEndpoint>(port_);
                session_ = static_cast<std::uint32_t>(now_ms()) | 1u;
                RC::Output::send(endpoint_->valid()
                    ? STR("[CashCleanerCoop] UDP host ready\n")
                    : STR("[CashCleanerCoop] ERROR: UDP host bind failed\n"));
            }
            else if (mode && std::string(mode) == "join")
            {
                mode_ = CoopMode::join;
                const auto* address = std::getenv("CCSCOOP_HOST");
                host_ = address && *address ? address : "127.0.0.1";
                endpoint_ = std::make_unique<ccscoop::UdpEndpoint>(0);
                nonce_ = now_ms() | 1ull;
                RC::Output::send(endpoint_->valid()
                    ? STR("[CashCleanerCoop] UDP guest ready\n")
                    : STR("[CashCleanerCoop] ERROR: UDP guest bind failed\n"));
            }
        }

        auto on_unreal_init() -> void override
        {
            unreal_ready_ = true;
            register_interaction_hooks();
            RC::Output::send(STR("[CashCleanerCoop] Unreal initialized; bootstrap is passive\n"));
        }

        auto on_update() -> void override
        {
            if (!endpoint_ || !endpoint_->valid()) return;
            const auto now = now_ms();
            if (mode_ == CoopMode::join && !watchdog_.connected() && now - last_hello_ms_ >= 500)
            {
                endpoint_->send(host_, port_, ccscoop::encode_wire(
                    {ccscoop::WireType::hello, 0, nonce_}));
                last_hello_ms_ = now;
            }

            for (int count = 0; count < 32; ++count)
            {
                const auto datagram = endpoint_->receive();
                if (!datagram) break;
                const bool from_peer = mode_ == CoopMode::host
                    ? datagram->address == peer_address_ && datagram->port == peer_port_
                    : datagram->address == host_ && datagram->port == port_;
                const auto object_action = ccscoop::decode_object_action(datagram->payload);
                if (object_action)
                {
                    if (from_peer && object_action->session == session_)
                    {
                        watchdog_.activity(now);
                        if (mode_ == CoopMode::host) handle_object_action(*object_action);
                        else
                        {
                            auto& lease = object_leases_[object_action->object];
                            lease.epoch = object_action->epoch;
                            lease.holder = object_action->kind == ccscoop::ObjectActionKind::pickup_granted
                                ? object_action->player : 0;
                            const auto pending = pending_pickups_.find(object_action->request);
                            if (object_action->player == 2 && pending != pending_pickups_.end())
                            {
                                if (object_action->kind == ccscoop::ObjectActionKind::pickup_denied)
                                    rollback_pickup(pending->second);
                                else if (object_action->kind == ccscoop::ObjectActionKind::pickup_granted)
                                    RC::Output::send(STR("[CashCleanerCoop] Local pickup granted by host\n"));
                                pending_pickups_.erase(pending);
                            }
                            if (object_action->player == 1)
                                apply_remote_object_state(object_action->object,
                                    object_action->kind == ccscoop::ObjectActionKind::pickup_granted);
                            if (object_action->kind == ccscoop::ObjectActionKind::drop)
                                RC::Output::send(STR("[CashCleanerCoop] Local drop accepted by host\n"));
                            if (!received_object_action_)
                            {
                                RC::Output::send(STR("[CashCleanerCoop] Object authority response received\n"));
                                received_object_action_ = true;
                            }
                        }
                    }
                    continue;
                }
                const auto transform = ccscoop::decode_player_transform(datagram->payload);
                if (transform)
                {
                    if (from_peer && transform->session == session_ && transform->sequence > remote_sequence_)
                    {
                        watchdog_.activity(now);
                        remote_sequence_ = transform->sequence;
                        remote_transform_ = *transform;
                        if (!received_transform_)
                        {
                            RC::Output::send(STR("[CashCleanerCoop] Remote player transform received\n"));
                            received_transform_ = true;
                        }
                    }
                    continue;
                }
                const auto object_transform = ccscoop::decode_object_transform(datagram->payload);
                if (object_transform)
                {
                    if (from_peer && object_transform->session == session_)
                    {
                        watchdog_.activity(now);
                        apply_remote_object_transform(*object_transform);
                    }
                    continue;
                }
                const auto message = ccscoop::decode_wire(datagram->payload);
                if (!message) continue;
                if (mode_ == CoopMode::host && message->type == ccscoop::WireType::hello)
                {
                    peer_address_ = datagram->address;
                    peer_port_ = datagram->port;
                    watchdog_.connect(now);
                    endpoint_->send(datagram->address, datagram->port, ccscoop::encode_wire(
                        {ccscoop::WireType::welcome, session_, message->nonce}));
                    RC::Output::send(STR("[CashCleanerCoop] Guest handshake accepted\n"));
                }
                else if (mode_ == CoopMode::join && message->type == ccscoop::WireType::welcome &&
                         message->nonce == nonce_)
                {
                    session_ = message->session;
                    if (!watchdog_.connected()) RC::Output::send(STR("[CashCleanerCoop] Connected to host\n"));
                    watchdog_.connect(now);
                }
                else if (message->type == ccscoop::WireType::heartbeat && from_peer &&
                         message->session == session_)
                {
                    watchdog_.activity(now);
                    if (mode_ == CoopMode::host)
                    {
                        endpoint_->send(peer_address_, peer_port_, ccscoop::encode_wire(*message));
                    }
                }
            }

            if (watchdog_.connected() && mode_ == CoopMode::join && now - last_heartbeat_ms_ >= 500)
            {
                endpoint_->send(host_, port_, ccscoop::encode_wire(
                    {ccscoop::WireType::heartbeat, session_, now}));
                last_heartbeat_ms_ = now;
            }

            if (watchdog_.expired(now))
            {
                watchdog_.disconnect();
                remote_sequence_ = 0;
                transform_sequence_ = 0;
                remote_transform_.reset();
                object_transform_order_.clear();
                local_object_cache_.clear();
                received_transform_ = false;
                sent_transform_ = false;
                if (ghost_actor_) ghost_actor_->K2_DestroyActor();
                ghost_actor_ = nullptr;
                if (mode_ == CoopMode::host) { peer_address_.clear(); peer_port_ = 0; }
                else nonce_ = now | 1ull;
                RC::Output::send(STR("[CashCleanerCoop] Peer timed out; waiting for reconnect\n"));
            }

            if (watchdog_.connected() && unreal_ready_ && now - last_transform_ms_ >= 100)
            {
                auto* object = RC::Unreal::UObjectGlobals::FindFirstOf(STR("PlayerCharacter"));
                if (object && !object->HasAnyFlags(RC::Unreal::RF_ClassDefaultObject))
                {
                    auto* actor = static_cast<RC::Unreal::AActor*>(object);
                    local_actor_ = actor;
                    const auto location = actor->K2_GetActorLocation();
                    const auto rotation = actor->K2_GetActorRotation();
                    const auto transform = ccscoop::PlayerTransform{
                        session_, mode_ == CoopMode::host ? 1u : 2u, ++transform_sequence_,
                        static_cast<float>(location.X()), static_cast<float>(location.Y()),
                        static_cast<float>(location.Z()), static_cast<float>(rotation.GetYaw())};
                    const auto& address = mode_ == CoopMode::host ? peer_address_ : host_;
                    const auto port = mode_ == CoopMode::host ? peer_port_ : port_;
                    endpoint_->send(address, port, ccscoop::encode_player_transform(transform));
                    if (!sent_transform_)
                    {
                        RC::Output::send(STR("[CashCleanerCoop] Local player transform streaming\n"));
                        sent_transform_ = true;
                    }
                }
                last_transform_ms_ = now;
            }

            update_remote_ghost();
            flush_pending_object_transforms(now);
            restore_remote_object_collisions(now);
            stream_host_world(now);

        }

      private:
        struct RawObjectArray
        {
            RC::Unreal::UObject** data;
            std::int32_t count;
            std::int32_t capacity;
        };

        struct PickupParams { RawObjectArray objects; };
        struct DropParams { RC::Unreal::UObject* object; };

        auto register_interaction_hooks() -> void
        {
            const RC::Unreal::UnrealScriptFunctionCallable no_op =
                [](RC::Unreal::UnrealScriptFunctionCallableContext&, void*) {};
            const auto pickup = RC::Unreal::UObjectGlobals::RegisterHook(
                STR("/Script/CashCleanerSim.PickupCharacterComponent:Pickup"),
                [](RC::Unreal::UnrealScriptFunctionCallableContext& context, void* data)
                {
                    static_cast<CashCleanerCoopMod*>(data)->on_local_pickup(context.GetParams<PickupParams>().objects);
                }, no_op, this);
            const auto grab = RC::Unreal::UObjectGlobals::RegisterHook(
                STR("/Script/CashCleanerSim.PickupCharacterComponent:Grab"),
                [](RC::Unreal::UnrealScriptFunctionCallableContext& context, void* data)
                {
                    static_cast<CashCleanerCoopMod*>(data)->on_local_pickup(context.GetParams<PickupParams>().objects);
                }, no_op, this);
            const auto drop = RC::Unreal::UObjectGlobals::RegisterHook(
                STR("/Script/CashCleanerSim.PickupCharacterComponent:Drop"),
                [](RC::Unreal::UnrealScriptFunctionCallableContext& context, void* data)
                {
                    static_cast<CashCleanerCoopMod*>(data)->prepare_local_drop(
                        context.Context, context.GetParams<DropParams>().object);
                }, [](RC::Unreal::UnrealScriptFunctionCallableContext&, void* data)
                {
                    static_cast<CashCleanerCoopMod*>(data)->finish_local_drop();
                }, this);
            const auto throw_object = RC::Unreal::UObjectGlobals::RegisterHook(
                STR("/Script/CashCleanerSim.PickupCharacterComponent:Throw"),
                [](RC::Unreal::UnrealScriptFunctionCallableContext& context, void* data)
                {
                    static_cast<CashCleanerCoopMod*>(data)->prepare_local_drop(context.Context, nullptr);
                }, [](RC::Unreal::UnrealScriptFunctionCallableContext&, void* data)
                {
                    static_cast<CashCleanerCoopMod*>(data)->finish_local_drop();
                }, this);
            const auto auto_pickup = RC::Unreal::UObjectGlobals::RegisterHook(
                STR("/Script/CashCleanerSim.PickupCharacterComponent:AutoPickUp"), no_op,
                [](RC::Unreal::UnrealScriptFunctionCallableContext& context, void* data)
                {
                    static_cast<CashCleanerCoopMod*>(data)->on_automatic_pickup(
                        context.Context, STR("/Script/CashCleanerSim.PickupCharacterComponent:GetFirstPickedUpObject"));
                }, this);
            const auto auto_grab = RC::Unreal::UObjectGlobals::RegisterHook(
                STR("/Script/CashCleanerSim.PickupCharacterComponent:AutoGrab"), no_op,
                [](RC::Unreal::UnrealScriptFunctionCallableContext& context, void* data)
                {
                    static_cast<CashCleanerCoopMod*>(data)->on_automatic_pickup(
                        context.Context, STR("/Script/CashCleanerSim.PickupCharacterComponent:GetCarryingLargeObject"));
                }, this);
            interaction_hooks_ = {pickup, grab, drop, throw_object, auto_pickup, auto_grab};
            RC::Output::send(STR("[CashCleanerCoop] Pickup/drop bridge hooks registered\n"));
        }

        auto on_local_pickup(const RawObjectArray& objects) -> void
        {
            if (!objects.data || objects.count <= 0 || objects.count > 128 || objects.capacity < objects.count) return;
            for (std::int32_t index = 0; index < objects.count; ++index)
            {
                on_local_pickup_object(objects.data[index]);
            }
            RC::Output::send(STR("[CashCleanerCoop] Local pickup observed\n"));
        }

        auto on_automatic_pickup(RC::Unreal::UObject* component, const CharType* getter_path) -> void
        {
            auto* object = get_held_object(component, getter_path);
            if (object) on_local_pickup_object(object);
        }

        auto get_held_object(RC::Unreal::UObject* component, const CharType* getter_path)
            -> RC::Unreal::UObject*
        {
            auto* function = RC::Unreal::UObjectGlobals::StaticFindObject<RC::Unreal::UFunction*>(
                nullptr, nullptr, getter_path);
            if (!component || !function || !function->GetReturnProperty()) return nullptr;
            std::vector<std::uint8_t> params(function->GetParmsSize());
            component->ProcessEvent(function, params.data());
            return *reinterpret_cast<RC::Unreal::UObject**>(
                params.data() + function->GetReturnProperty()->GetOffset_Internal());
        }

        auto prepare_local_drop(RC::Unreal::UObject* component, RC::Unreal::UObject* fallback) -> void
        {
            if (suppress_local_drop_) return;
            auto* object = get_held_object(
                component, STR("/Script/CashCleanerSim.PickupCharacterComponent:GetCarryingLargeObject"));
            if (!object)
                object = get_held_object(
                    component, STR("/Script/CashCleanerSim.PickupCharacterComponent:GetFirstPickedUpObject"));
            pending_local_drop_ = object ? object : fallback;
        }

        auto finish_local_drop() -> void
        {
            if (suppress_local_drop_ || !pending_local_drop_) return;
            auto* object = pending_local_drop_;
            pending_local_drop_ = nullptr;
            on_local_drop(object);
        }

        auto on_local_pickup_object(RC::Unreal::UObject* value) -> void
        {
            const auto object = stable_object_id(value);
            if (object == 0) return;
            if (std::getenv("CCSCOOP_TRACE_OBJECTS"))
            {
                RC::Output::send(STR("[CashCleanerCoop] Pickup identity "));
                RC::Output::send(value->GetFullName());
                RC::Output::send(STR("\n"));
                if (auto* unique_id = value->GetValuePtrByPropertyNameInChain<RC::Unreal::FGuid>(STR("UniqueID")))
                {
                    RC::Output::send(STR("[CashCleanerCoop] Pickup UniqueID {} {} {} {}\n"),
                        unique_id->A, unique_id->B, unique_id->C, unique_id->D);
                }
            }
            const auto observed_at = now_ms();
            auto& previous = local_pickup_ms_[object];
            if (previous != 0 && observed_at - previous < 250) return;
            previous = observed_at;
            if (mode_ == CoopMode::host)
            {
                auto& lease = object_leases_[object];
                if (lease.epoch == 0) lease.epoch = 1;
                lease.holder = 1;
                ++lease.epoch;
                if (watchdog_.connected())
                {
                    const ccscoop::ObjectAction action{ccscoop::ObjectActionKind::pickup_granted,
                        session_, ++object_request_, object, 1, lease.epoch};
                    endpoint_->send(peer_address_, peer_port_, ccscoop::encode_object_action(action));
                }
            }
            else if (mode_ == CoopMode::join && watchdog_.connected())
            {
                const ccscoop::ObjectAction action{ccscoop::ObjectActionKind::pickup_request,
                    session_, ++object_request_, object, 2, 0};
                pending_pickups_[action.request] = value;
                endpoint_->send(host_, port_, ccscoop::encode_object_action(action));
            }
            RC::Output::send(STR("[CashCleanerCoop] Local pickup object observed\n"));
        }

        auto on_local_drop(RC::Unreal::UObject* value) -> void
        {
            if (suppress_local_drop_) return;
            const auto object = stable_object_id(value);
            if (object == 0) return;
            auto& lease = object_leases_[object];
            if (mode_ == CoopMode::host && lease.holder == 1)
            {
                lease.holder = 0;
                ++lease.epoch;
                if (watchdog_.connected())
                {
                    const ccscoop::ObjectAction action{ccscoop::ObjectActionKind::drop,
                        session_, ++object_request_, object, 1, lease.epoch};
                    endpoint_->send(peer_address_, peer_port_, ccscoop::encode_object_action(action));
                    queue_object_transform(value, object, 1, lease.epoch);
                }
            }
            else if (mode_ == CoopMode::join && watchdog_.connected() && lease.holder == 2 && lease.epoch != 0)
            {
                const ccscoop::ObjectAction action{ccscoop::ObjectActionKind::drop,
                    session_, ++object_request_, object, 2, lease.epoch};
                endpoint_->send(host_, port_, ccscoop::encode_object_action(action));
                queue_object_transform(value, object, 2, lease.epoch);
            }
            RC::Output::send(STR("[CashCleanerCoop] Local drop observed\n"));
        }

        auto rollback_pickup(RC::Unreal::UObject* object) -> void
        {
            if (!object) return;
            auto* component = RC::Unreal::UObjectGlobals::FindFirstOf(STR("PickupCharacterComponent"));
            auto* function = RC::Unreal::UObjectGlobals::StaticFindObject<RC::Unreal::UFunction*>(
                nullptr, nullptr, STR("/Script/CashCleanerSim.PickupCharacterComponent:Drop"));
            if (!component || !function) return;
            auto* object_property = function->FindProperty(
                RC::Unreal::FName(STR("Object"), RC::Unreal::FNAME_Find));
            if (!object_property) return;

            std::vector<std::uint8_t> params(function->GetParmsSize());
            *reinterpret_cast<RC::Unreal::UObject**>(params.data() + object_property->GetOffset_Internal()) = object;
            suppress_local_drop_ = true;
            component->ProcessEvent(function, params.data());
            suppress_local_drop_ = false;
            RC::Output::send(STR("[CashCleanerCoop] Denied pickup rolled back\n"));
        }

        auto handle_object_action(const ccscoop::ObjectAction& action) -> void
        {
            const auto duplicate = object_responses_.find(action.request);
            if (duplicate != object_responses_.end())
            {
                endpoint_->send(peer_address_, peer_port_, ccscoop::encode_object_action(duplicate->second));
                return;
            }

            auto& lease = object_leases_[action.object];
            if (lease.epoch == 0) lease.epoch = 1;
            ccscoop::ObjectAction response{ccscoop::ObjectActionKind::pickup_denied,
                session_, action.request, action.object, action.player, lease.epoch};
            if (action.player == 2 && action.kind == ccscoop::ObjectActionKind::pickup_request && lease.holder == 0)
            {
                lease.holder = action.player;
                ++lease.epoch;
                response.kind = ccscoop::ObjectActionKind::pickup_granted;
                response.epoch = lease.epoch;
            }
            else if (action.player == 2 && action.kind == ccscoop::ObjectActionKind::drop &&
                     lease.holder == action.player && lease.epoch == action.epoch)
            {
                lease.holder = 0;
                ++lease.epoch;
                response.kind = ccscoop::ObjectActionKind::drop;
                response.epoch = lease.epoch;
            }
            object_responses_.emplace(action.request, response);
            endpoint_->send(peer_address_, peer_port_, ccscoop::encode_object_action(response));
            if (response.kind == ccscoop::ObjectActionKind::pickup_granted)
                apply_remote_object_state(action.object, true);
            else if (response.kind == ccscoop::ObjectActionKind::drop)
                apply_remote_object_state(action.object, false);
            RC::Output::send(response.kind == ccscoop::ObjectActionKind::pickup_granted
                ? STR("[CashCleanerCoop] Object pickup granted\n")
                : response.kind == ccscoop::ObjectActionKind::drop
                    ? STR("[CashCleanerCoop] Object drop accepted\n")
                    : STR("[CashCleanerCoop] Object action denied\n"));
        }

        auto apply_remote_object_state(std::uint64_t object_id, bool hidden) -> void
        {
            auto* match = find_local_object(object_id);
            if (!match)
            {
                RC::Output::send(STR("[CashCleanerCoop] Remote object not found locally\n"));
                return;
            }
            match->SetActorHiddenInGame(hidden);
            match->SetActorEnableCollision(!hidden);
            RC::Output::send(hidden
                ? STR("[CashCleanerCoop] Remote-held object hidden\n")
                : STR("[CashCleanerCoop] Remote-held object restored\n"));
        }

        auto find_local_object(std::uint64_t object_id) -> RC::Unreal::AActor*
        {
            const auto cached = local_object_cache_.find(object_id);
            if (cached != local_object_cache_.end()) return cached->second;
            RC::Unreal::AActor* match = nullptr;
            RC::Unreal::UObjectGlobals::ForEachUObject(
                [&](RC::Unreal::UObject* object, std::int32_t, std::int32_t)
                {
                    if (!object || object->HasAnyFlags(RC::Unreal::RF_ClassDefaultObject) ||
                        !object->IsA(RC::Unreal::AActor::StaticClass()))
                        return RC::LoopAction::Continue;
                    if (stable_object_id(object) != object_id)
                        return RC::LoopAction::Continue;
                    match = static_cast<RC::Unreal::AActor*>(object);
                    local_object_cache_[object_id] = match;
                    return RC::LoopAction::Break;
                });
            return match;
        }

        auto apply_remote_object_transform(const ccscoop::ObjectTransform& transform) -> void
        {
            if (!object_transform_order_.should_apply(transform)) return;
            auto* actor = find_local_object(transform.object);
            if (!actor) return;
            RC::Unreal::FVector location{};
            location.SetX(transform.x);
            location.SetY(transform.y);
            location.SetZ(transform.z);
            constexpr double radians_to_degrees = 57.29577951308232;
            const double sin_pitch = std::clamp(
                2.0 * (transform.qw * transform.qy - transform.qz * transform.qx), -1.0, 1.0);
            const double pitch = std::asin(sin_pitch) * radians_to_degrees;
            const double yaw = std::atan2(
                2.0 * (transform.qw * transform.qz + transform.qx * transform.qy),
                1.0 - 2.0 * (transform.qy * transform.qy + transform.qz * transform.qz)) * radians_to_degrees;
            const double roll = std::atan2(
                2.0 * (transform.qw * transform.qx + transform.qy * transform.qz),
                1.0 - 2.0 * (transform.qx * transform.qx + transform.qy * transform.qy)) * radians_to_degrees;
            RC::Unreal::FRotator rotation{pitch, yaw, roll};
            RC::Unreal::FHitResult hit{};
            actor->SetActorHiddenInGame(false);
            actor->SetActorEnableCollision(false);
            set_object_physics(actor, false);
            actor->K2_SetActorLocationAndRotation(location, rotation, false, hit, true);
            remote_transform_activity_[transform.object] = {actor, now_ms()};
        }

        auto stream_host_world(std::uint64_t now) -> void
        {
            if (mode_ != CoopMode::host || !watchdog_.connected() || now - last_world_stream_ms_ < 100)
                return;
            last_world_stream_ms_ = now;
            auto* interactive_class = RC::Unreal::UObjectGlobals::StaticFindObject<RC::Unreal::UClass*>(
                nullptr, nullptr, STR("/Script/CashCleanerSim.InteractiveObject"));
            if (!interactive_class) return;
            std::uint32_t sent = 0;
            RC::Unreal::UObjectGlobals::ForEachUObject(
                [&](RC::Unreal::UObject* object, std::int32_t, std::int32_t)
                {
                    if (sent >= 32) return RC::LoopAction::Break;
                    if (!object || object->HasAnyFlags(RC::Unreal::RF_ClassDefaultObject) ||
                        !object->IsA(interactive_class) || !object->IsA(RC::Unreal::AActor::StaticClass()))
                        return RC::LoopAction::Continue;
                    auto* unique_id = object->GetValuePtrByPropertyNameInChain<RC::Unreal::FGuid>(STR("UniqueID"));
                    if (!unique_id || !(unique_id->A || unique_id->B || unique_id->C || unique_id->D))
                        return RC::LoopAction::Continue;
                    const auto id = stable_object_id(object);
                    const auto lease = object_leases_.find(id);
                    if (lease != object_leases_.end() && lease->second.holder != 0)
                        return RC::LoopAction::Continue;
                    auto* actor = static_cast<RC::Unreal::AActor*>(object);
                    const auto location = actor->K2_GetActorLocation();
                    auto actor_transform = actor->GetTransform();
                    const auto& rotation = actor_transform.GetRotation();
                    WorldPose pose{static_cast<float>(location.X()), static_cast<float>(location.Y()),
                        static_cast<float>(location.Z()), static_cast<float>(rotation.GetX()),
                        static_cast<float>(rotation.GetY()), static_cast<float>(rotation.GetZ()),
                        static_cast<float>(rotation.GetW())};
                    const auto previous = last_world_poses_.find(id);
                    const auto last_sent = last_world_pose_sent_ms_.find(id);
                    if (previous != last_world_poses_.end() && pose.nearly_equals(previous->second) &&
                        last_sent != last_world_pose_sent_ms_.end() && now - last_sent->second < 2000)
                        return RC::LoopAction::Continue;
                    last_world_poses_[id] = pose;
                    last_world_pose_sent_ms_[id] = now;
                    auto& authority = object_leases_[id];
                    if (authority.epoch == 0) authority.epoch = 1;
                    const ccscoop::ObjectTransform packet{session_, id, 1, authority.epoch,
                        ++object_transform_sequence_, pose.x, pose.y, pose.z,
                        pose.qx, pose.qy, pose.qz, pose.qw,
                        0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
                    endpoint_->send(peer_address_, peer_port_, ccscoop::encode_object_transform(packet));
                    ++sent;
                    return RC::LoopAction::Continue;
                });
        }

        auto queue_object_transform(RC::Unreal::UObject* value, std::uint64_t object,
                                    std::uint32_t player, std::uint32_t epoch) -> void
        {
            if (!value || !value->IsA(RC::Unreal::AActor::StaticClass())) return;
            const auto now = now_ms();
            pending_object_transforms_.push_back(
                {static_cast<RC::Unreal::AActor*>(value), object, player, epoch, now + 50, now + 5000});
        }

        auto flush_pending_object_transforms(std::uint64_t now) -> void
        {
            for (auto it = pending_object_transforms_.begin(); it != pending_object_transforms_.end();)
            {
                if (now < it->send_at) { ++it; continue; }
                const auto location = it->actor->K2_GetActorLocation();
                auto actor_transform = it->actor->GetTransform();
                const auto& rotation = actor_transform.GetRotation();
                const ccscoop::ObjectTransform transform{session_, it->object, it->player, it->epoch,
                    ++object_transform_sequence_,
                    static_cast<float>(location.X()), static_cast<float>(location.Y()),
                    static_cast<float>(location.Z()), static_cast<float>(rotation.GetX()),
                    static_cast<float>(rotation.GetY()), static_cast<float>(rotation.GetZ()),
                    static_cast<float>(rotation.GetW()), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
                const auto& address = mode_ == CoopMode::host ? peer_address_ : host_;
                const auto port = mode_ == CoopMode::host ? peer_port_ : port_;
                endpoint_->send(address, port, ccscoop::encode_object_transform(transform));
                if (now >= it->send_until)
                    it = pending_object_transforms_.erase(it);
                else
                {
                    it->send_at = now + 50;
                    ++it;
                }
            }
        }

        auto restore_remote_object_collisions(std::uint64_t now) -> void
        {
            for (auto it = remote_transform_activity_.begin(); it != remote_transform_activity_.end();)
            {
                if (now - it->second.last_update < 250) { ++it; continue; }
                it->second.actor->SetActorEnableCollision(true);
                RC::Output::send(STR("[CashCleanerCoop] Remote object trajectory settled\n"));
                it = remote_transform_activity_.erase(it);
            }
        }

        auto set_object_physics(RC::Unreal::AActor* actor, bool enabled) -> void
        {
            if (!actor) return;
            auto* function = RC::Unreal::UObjectGlobals::StaticFindObject<RC::Unreal::UFunction*>(
                nullptr, nullptr, STR("/Script/CashCleanerSim.InteractiveObject:SetSimulatePhysics"));
            if (!function) return;
            struct SetSimulatePhysicsParams { bool simulate; } params{enabled};
            actor->ProcessEvent(function, &params);
        }

        auto update_remote_ghost() -> void
        {
            if (!ghost_enabled_ || !remote_transform_ || !local_actor_) return;
            RC::Unreal::FVector location{};
            location.SetX(remote_transform_->x);
            location.SetY(remote_transform_->y);
            location.SetZ(remote_transform_->z);
            RC::Unreal::FRotator rotation{0.0, remote_transform_->yaw, 0.0};

            if (!ghost_actor_)
            {
                try
                {
                    ghost_stage_ = 1;
                    auto* ghost_class = RC::Unreal::UObjectGlobals::StaticFindObject<RC::Unreal::UClass*>(
                        nullptr, nullptr, STR("/Script/Engine.SkeletalMeshActor"));
                    auto* world = local_actor_->GetWorld();
                    if (!ghost_class || !world) return;
                    ghost_stage_ = 2;
                    ghost_actor_ = world->SpawnActor(ghost_class, &location, &rotation);
                    if (!ghost_actor_) return;
                    ghost_actor_->SetActorEnableCollision(false);
                    ghost_actor_->SetActorTickEnabled(false);

                    ghost_stage_ = 3;
                    auto* component_class = RC::Unreal::UObjectGlobals::StaticFindObject<RC::Unreal::UClass*>(
                        nullptr, nullptr, STR("/Script/Engine.SkeletalMeshComponent"));
                    auto** target_component = ghost_actor_->GetValuePtrByPropertyNameInChain<RC::Unreal::UObject*>(STR("SkeletalMeshComponent"));
                    if (!component_class || !target_component || !*target_component)
                        throw std::runtime_error("mesh component unavailable");

                    ghost_stage_ = 4;
                    RC::Unreal::UObject* mesh_asset = nullptr;
                    for (auto* component : local_actor_->GetComponentsByClass(component_class))
                    {
                        RC::Unreal::UObject** mesh = nullptr;
                        if (component->GetPropertyByNameInChain(STR("SkeletalMeshAsset")))
                            mesh = component->GetValuePtrByPropertyNameInChain<RC::Unreal::UObject*>(STR("SkeletalMeshAsset"));
                        else if (component->GetPropertyByNameInChain(STR("SkeletalMesh")))
                            mesh = component->GetValuePtrByPropertyNameInChain<RC::Unreal::UObject*>(STR("SkeletalMesh"));
                        if (mesh && *mesh) { mesh_asset = *mesh; break; }
                    }
                    if (!mesh_asset) throw std::runtime_error("skeletal mesh unavailable");

                    ghost_stage_ = 5;
                    auto* function = RC::Unreal::UObjectGlobals::StaticFindObject<RC::Unreal::UFunction*>(
                        nullptr, nullptr, STR("/Script/Engine.SkeletalMeshComponent:SetSkeletalMeshAsset"));
                    if (!function)
                        function = RC::Unreal::UObjectGlobals::StaticFindObject<RC::Unreal::UFunction*>(
                            nullptr, nullptr, STR("/Script/Engine.SkinnedMeshComponent:SetSkeletalMesh"));
                    if (!function) throw std::runtime_error("mesh setter unavailable");
                    struct SetMeshParams { RC::Unreal::UObject* NewMesh; } params{mesh_asset};
                    (*target_component)->ProcessEvent(function, &params);
                    ghost_stage_ = 6;
                    RC::Output::send(STR("[CashCleanerCoop] Remote ghost spawned\n"));
                }
                catch (...)
                {
                    if (ghost_actor_) ghost_actor_->K2_DestroyActor();
                    ghost_actor_ = nullptr;
                    if (!ghost_error_logged_)
                    {
                        RC::Output::send(STR("[CashCleanerCoop] ERROR: remote ghost setup failed\n"));
                        if (ghost_stage_ == 1) RC::Output::send(STR("[CashCleanerCoop] Ghost failure stage 1: class/world\n"));
                        else if (ghost_stage_ == 2) RC::Output::send(STR("[CashCleanerCoop] Ghost failure stage 2: spawn\n"));
                        else if (ghost_stage_ == 3) RC::Output::send(STR("[CashCleanerCoop] Ghost failure stage 3: target component\n"));
                        else if (ghost_stage_ == 4) RC::Output::send(STR("[CashCleanerCoop] Ghost failure stage 4: source mesh\n"));
                        else if (ghost_stage_ == 5) RC::Output::send(STR("[CashCleanerCoop] Ghost failure stage 5: mesh setter\n"));
                        ghost_error_logged_ = true;
                    }
                }
            }
            else
            {
                RC::Unreal::FHitResult hit{};
                ghost_actor_->K2_SetActorLocationAndRotation(location, rotation, false, hit, true);
            }
        }

        CoopMode mode_{CoopMode::passive};
        std::unique_ptr<ccscoop::UdpEndpoint> endpoint_;
        std::string host_{"127.0.0.1"};
        std::string peer_address_;
        std::uint16_t port_{env_port()};
        std::uint16_t peer_port_{};
        std::uint32_t session_{};
        std::uint64_t nonce_{};
        std::uint64_t last_hello_ms_{};
        std::uint64_t last_transform_ms_{};
        std::uint64_t last_heartbeat_ms_{};
        std::uint32_t transform_sequence_{};
        std::uint32_t remote_sequence_{};
        std::optional<ccscoop::PlayerTransform> remote_transform_;
        RC::Unreal::AActor* local_actor_{};
        RC::Unreal::AActor* ghost_actor_{};
        ccscoop::ConnectionWatchdog watchdog_{3000};
        bool unreal_ready_{};
        bool sent_transform_{};
        bool received_transform_{};
        bool ghost_enabled_{};
        bool ghost_error_logged_{};
        int ghost_stage_{};
        struct ObjectLease { std::uint32_t holder{}; std::uint32_t epoch{}; };
        std::unordered_map<std::uint64_t, ObjectLease> object_leases_;
        std::unordered_map<std::uint32_t, ccscoop::ObjectAction> object_responses_;
        std::unordered_map<std::uint64_t, std::uint64_t> local_pickup_ms_;
        std::unordered_map<std::uint32_t, RC::Unreal::UObject*> pending_pickups_;
        std::vector<std::pair<int, int>> interaction_hooks_;
        std::uint32_t object_request_{};
        bool received_object_action_{};
        bool suppress_local_drop_{};
        RC::Unreal::UObject* pending_local_drop_{};
        struct PendingObjectTransform {
            RC::Unreal::AActor* actor;
            std::uint64_t object;
            std::uint32_t player;
            std::uint32_t epoch;
            std::uint64_t send_at;
            std::uint64_t send_until;
        };
        std::vector<PendingObjectTransform> pending_object_transforms_;
        struct RemoteTransformActivity {
            RC::Unreal::AActor* actor;
            std::uint64_t last_update;
        };
        std::unordered_map<std::uint64_t, RemoteTransformActivity> remote_transform_activity_;
        ccscoop::ObjectTransformOrder object_transform_order_;
        std::unordered_map<std::uint64_t, RC::Unreal::AActor*> local_object_cache_;
        struct WorldPose {
            float x, y, z, qx, qy, qz, qw;
            auto nearly_equals(const WorldPose& other) const -> bool {
                const float position_error = std::fabs(x - other.x) + std::fabs(y - other.y) +
                                             std::fabs(z - other.z);
                const float quaternion_dot = std::fabs(qx * other.qx + qy * other.qy +
                                                       qz * other.qz + qw * other.qw);
                return position_error < 0.01f && quaternion_dot > 0.999999f;
            }
        };
        std::unordered_map<std::uint64_t, WorldPose> last_world_poses_;
        std::unordered_map<std::uint64_t, std::uint64_t> last_world_pose_sent_ms_;
        std::uint32_t object_transform_sequence_{};
        std::uint64_t last_world_stream_ms_{};
    };
}

#define CCSCOOP_MOD_API __declspec(dllexport)

extern "C"
{
    CCSCOOP_MOD_API RC::CppUserModBase* start_mod()
    {
        return new CashCleanerCoopMod();
    }

    CCSCOOP_MOD_API void uninstall_mod(RC::CppUserModBase* mod)
    {
        delete mod;
    }
}
