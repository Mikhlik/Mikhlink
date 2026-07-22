#include <obs-module.h>

#include "network/NetworkAdapter.h"

#include <exception>

OBS_DECLARE_MODULE()

MODULE_EXPORT const char* obs_module_description(void)
{
    return "Mikhlink bonded streaming output for OBS Studio";
}

bool obs_module_load(void)
{
    try
    {
        const auto adapters = mikhlink::network::getNetworkAdapters();

        blog(LOG_INFO,
             "[Mikhlink] Plugin loaded. Detected %zu network adapter(s).",
             adapters.size());

        for (const auto& adapter : adapters)
        {
            blog(LOG_INFO,
                 "[Mikhlink] Adapter: %s | Type: %s | Status: %s",
                 adapter.name.c_str(),
                 adapter.type.c_str(),
                 adapter.isUp ? "Up" : "Down");
        }
    }
    catch (const std::exception& error)
    {
        blog(LOG_ERROR,
             "[Mikhlink] Failed to enumerate network adapters: %s",
             error.what());
    }

    return true;
}
