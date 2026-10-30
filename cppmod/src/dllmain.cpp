#include <DynamicOutput/DynamicOutput.hpp>
#include <Mod/CppUserModBase.hpp>
#include <ccscoop/net.h>

#include <chrono>
#include <cstdlib>
#include <memory>
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
            RC::Output::send(STR("[CashCleanerCoop] Unreal initialized; bootstrap is passive\n"));
        }

        auto on_update() -> void override
        {
            if (!endpoint_ || !endpoint_->valid()) return;
            const auto now = now_ms();
            if (mode_ == CoopMode::join && !connected_ && now - last_hello_ms_ >= 500)
            {
                endpoint_->send(host_, port_, ccscoop::encode_wire(
                    {ccscoop::WireType::hello, 0, nonce_}));
                last_hello_ms_ = now;
            }

            for (int count = 0; count < 32; ++count)
            {
                const auto datagram = endpoint_->receive();
                if (!datagram) break;
                const auto message = ccscoop::decode_wire(datagram->payload);
                if (!message) continue;
                if (mode_ == CoopMode::host && message->type == ccscoop::WireType::hello)
                {
                    endpoint_->send(datagram->address, datagram->port, ccscoop::encode_wire(
                        {ccscoop::WireType::welcome, session_, message->nonce}));
                    RC::Output::send(STR("[CashCleanerCoop] Guest handshake accepted\n"));
                }
                else if (mode_ == CoopMode::join && message->type == ccscoop::WireType::welcome &&
                         message->nonce == nonce_)
                {
                    session_ = message->session;
                    if (!connected_) RC::Output::send(STR("[CashCleanerCoop] Connected to host\n"));
                    connected_ = true;
                }
            }
        }

      private:
        CoopMode mode_{CoopMode::passive};
        std::unique_ptr<ccscoop::UdpEndpoint> endpoint_;
        std::string host_{"127.0.0.1"};
        std::uint16_t port_{env_port()};
        std::uint32_t session_{};
        std::uint64_t nonce_{};
        std::uint64_t last_hello_ms_{};
        bool connected_{};
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
