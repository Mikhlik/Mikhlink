#include "network/NetworkAdapter.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <exception>
#include <iostream>

int main()
{
    SetConsoleOutputCP(CP_UTF8);

    try
    {
        const auto adapters = mikhlink::network::getNetworkAdapters();

        std::cout << "Mikhlink detected " << adapters.size()
                  << " network adapter(s).\n\n";

        for (const auto& adapter : adapters)
        {
            std::cout << adapter.name << '\n'
                      << "  Type: " << adapter.type << '\n'
                      << "  Status: " << (adapter.isUp ? "Up" : "Down") << '\n';

            if (!adapter.description.empty())
            {
                std::cout << "  Description: " << adapter.description << '\n';
            }

            if (adapter.addresses.empty())
            {
                std::cout << "  Addresses: none\n";
            }
            else
            {
                std::cout << "  Addresses:\n";
                for (const auto& address : adapter.addresses)
                {
                    std::cout << "    " << address << '\n';
                }
            }

            std::cout << '\n';
        }
    }
    catch (const std::exception& error)
    {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }

    return 0;
}
