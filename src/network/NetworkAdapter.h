#pragma once

#include <string>
#include <vector>

namespace mikhlink::network
{

struct NetworkAdapter
{
    std::string name;
    std::string description;
    std::string type;
    bool isUp = false;
    std::vector<std::string> addresses;
};

std::vector<NetworkAdapter> getNetworkAdapters();

} // namespace mikhlink::network
