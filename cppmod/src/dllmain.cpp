#include <DynamicOutput/DynamicOutput.hpp>
#include <Mod/CppUserModBase.hpp>
#include <Unreal/AActor.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/World.hpp>
#include <ccscoop/net.h>

#include <chrono>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>

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

        }

      private:
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
