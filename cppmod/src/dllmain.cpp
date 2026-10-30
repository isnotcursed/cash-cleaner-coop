#include <DynamicOutput/DynamicOutput.hpp>
#include <Mod/CppUserModBase.hpp>

namespace
{
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
        }

        auto on_unreal_init() -> void override
        {
            RC::Output::send(STR("[CashCleanerCoop] Unreal initialized; bootstrap is passive\n"));
        }
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
