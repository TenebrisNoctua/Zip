#pragma once

#include "ILuauModule.h"
#include "ILuauHost.h"
#include "lua.h"

namespace Luwow::Zip {
    using ILuauModule = Luwow::Engine::ILuauModule;
    using ILuauHost = Luwow::Engine::ILuauHost;
    using RunMode = Luwow::Engine::RunMode;

    class Library : public ILuauModule {
    public:
        Library() = default;
        ~Library() override = default;

        ILuauModule* initialize(ILuauHost* host) override;
        RunMode getRunMode() const override;

        const char* getModuleName() const override;
        const char* getModuleAlias() const override;
        const LuauExport* getExports() const override;
    };
} // namespace Luwow::Zip
