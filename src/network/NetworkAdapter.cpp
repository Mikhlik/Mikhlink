#include "NetworkAdapter.h"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <ws2tcpip.h>

#include <stdexcept>
#include <utility>
#include <vector>

namespace mikhlink::network
{
namespace
{

std::string toUtf8(const wchar_t* text)
{
    if (text == nullptr || *text == L'\0')
    {
        return {};
    }

    const int size = WideCharToMultiByte(
        CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);

    if (size <= 1)
    {
        return {};
    }

    std::string result(static_cast<std::size_t>(size), '\0');

    if (WideCharToMultiByte(
            CP_UTF8,
            0,
            text,
            -1,
            result.data(),
            size,
            nullptr,
            nullptr) == 0)
    {
        return {};
    }

    result.pop_back();
    return result;
}

std::string adapterType(ULONG type)
{
    switch (type)
    {
    case IF_TYPE_ETHERNET_CSMACD:
        return "Ethernet";
    case IF_TYPE_IEEE80211:
        return "Wi-Fi";
    case IF_TYPE_PPP:
        return "PPP";
    case IF_TYPE_SOFTWARE_LOOPBACK:
        return "Loopback";
    case IF_TYPE_TUNNEL:
        return "Tunnel";
    case IF_TYPE_WWANPP:
    case IF_TYPE_WWANPP2:
        return "Cellular";
    default:
        return "Other";
    }
}

std::string numericAddress(const SOCKADDR* address)
{
    if (address == nullptr)
    {
        return {};
    }

    char buffer[INET6_ADDRSTRLEN] = {};

    if (address->sa_family == AF_INET)
    {
        const auto* ipv4 = reinterpret_cast<const SOCKADDR_IN*>(address);
        if (InetNtopA(AF_INET, &ipv4->sin_addr, buffer, sizeof(buffer)) != nullptr)
        {
            return buffer;
        }
    }
    else if (address->sa_family == AF_INET6)
    {
        const auto* ipv6 = reinterpret_cast<const SOCKADDR_IN6*>(address);
        if (InetNtopA(AF_INET6, &ipv6->sin6_addr, buffer, sizeof(buffer)) != nullptr)
        {
            return buffer;
        }
    }

    return {};
}

} // namespace

std::vector<NetworkAdapter> getNetworkAdapters()
{
    ULONG bufferSize = 15 * 1024;
    std::vector<unsigned char> buffer(bufferSize);

    auto* addresses =
        reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());

    ULONG result = GetAdaptersAddresses(
        AF_UNSPEC,
        GAA_FLAG_INCLUDE_PREFIX | GAA_FLAG_INCLUDE_GATEWAYS,
        nullptr,
        addresses,
        &bufferSize);

    if (result == ERROR_BUFFER_OVERFLOW)
    {
        buffer.resize(bufferSize);
        addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());

        result = GetAdaptersAddresses(
            AF_UNSPEC,
            GAA_FLAG_INCLUDE_PREFIX | GAA_FLAG_INCLUDE_GATEWAYS,
            nullptr,
            addresses,
            &bufferSize);
    }

    if (result != NO_ERROR)
    {
        throw std::runtime_error(
            "GetAdaptersAddresses failed with error " + std::to_string(result));
    }

    std::vector<NetworkAdapter> adapters;

    for (auto* current = addresses;
         current != nullptr;
         current = current->Next)
    {
        NetworkAdapter adapter;
        adapter.id = current->AdapterName != nullptr
            ? current->AdapterName
            : "";
        adapter.name = toUtf8(current->FriendlyName);
        adapter.description = toUtf8(current->Description);
        adapter.type = adapterType(current->IfType);
        adapter.isUp = current->OperStatus == IfOperStatusUp;
        adapter.hasGateway = current->FirstGatewayAddress != nullptr;

        MIB_IF_ROW2 interfaceRow = {};
        interfaceRow.InterfaceLuid = current->Luid;
        if (GetIfEntry2(&interfaceRow) == NO_ERROR)
        {
            adapter.isHardware =
                interfaceRow.InterfaceAndOperStatusFlags.HardwareInterface != 0;
        }

        for (auto* unicast = current->FirstUnicastAddress;
             unicast != nullptr;
             unicast = unicast->Next)
        {
            const std::string address =
                numericAddress(unicast->Address.lpSockaddr);

            if (!address.empty())
            {
                adapter.addresses.push_back(address);
            }
        }

        adapters.push_back(std::move(adapter));
    }

    return adapters;
}

} // namespace mikhlink::network
