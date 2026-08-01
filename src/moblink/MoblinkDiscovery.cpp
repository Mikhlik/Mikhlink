#include "MoblinkDiscovery.h"

#include <QRegularExpression>
#include <QUuid>

#include <atomic>
#include <mutex>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <windns.h>

#include <array>
#include <string>
#include <vector>
#endif

namespace mikhlink::moblink
{
namespace
{

QString normalizedInstanceId(const QString& value)
{
    const QUuid uuid(value.trimmed());
    if (!uuid.isNull())
    {
        return uuid.toString(QUuid::WithoutBraces);
    }

    QString result = value.trimmed().toLower();
    result.remove(QRegularExpression(QStringLiteral("[^a-z0-9-]")));
    if (result.isEmpty())
    {
        result = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    return result.left(63);
}

#ifdef _WIN32

constexpr DWORD DiscoveryStopTimeoutMs = 3000;

QString windowsError(DWORD status)
{
    wchar_t* rawMessage = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER |
            FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        status,
        0,
        reinterpret_cast<wchar_t*>(&rawMessage),
        0,
        nullptr);
    QString message = length != 0 && rawMessage != nullptr
        ? QString::fromWCharArray(rawMessage, static_cast<int>(length)).trimmed()
        : QStringLiteral("Windows error %1").arg(status);
    if (rawMessage != nullptr)
    {
        LocalFree(rawMessage);
    }
    return message;
}

std::wstring computerMdnsName()
{
    DWORD size = 0;
    GetComputerNameExW(ComputerNameDnsHostname, nullptr, &size);
    if (size == 0)
    {
        return L"mikhlink.local";
    }

    std::wstring hostname(size, L'\0');
    if (!GetComputerNameExW(
            ComputerNameDnsHostname,
            hostname.data(),
            &size) ||
        size == 0)
    {
        return L"mikhlink.local";
    }
    hostname.resize(size);
    constexpr auto LocalSuffix = L".local";
    if (hostname.size() < 6 ||
        hostname.compare(hostname.size() - 6, 6, LocalSuffix) != 0)
    {
        hostname += LocalSuffix;
    }
    return hostname;
}

#endif

} // namespace

QString discoveryServiceFqdn(const QString& instanceId)
{
    return normalizedInstanceId(instanceId) +
        QStringLiteral("._moblink._tcp.local");
}

struct DiscoveryPublisher::State
{
    std::atomic<DiscoveryStatus> status{DiscoveryStatus::Stopped};
    mutable std::mutex errorMutex;
    QString error;

#ifdef _WIN32
    std::wstring instanceName;
    std::wstring hostName;
    std::wstring propertyKey = L"name";
    std::wstring propertyValue;
    std::array<PWSTR, 1> keys{};
    std::array<PWSTR, 1> values{};
    DNS_SERVICE_INSTANCE instance{};
    DNS_SERVICE_REGISTER_REQUEST request{};
    DNS_SERVICE_CANCEL cancel{};
    HANDLE deregisteredEvent = nullptr;
    bool registrationRequested = false;

    ~State()
    {
        if (deregisteredEvent != nullptr)
        {
            CloseHandle(deregisteredEvent);
        }
    }

    void setError(const QString& value)
    {
        std::lock_guard<std::mutex> lock(errorMutex);
        error = value;
    }

    static void WINAPI registrationComplete(
        DWORD result,
        void* context,
        DNS_SERVICE_INSTANCE* returnedInstance)
    {
        auto* state = static_cast<State*>(context);
        if (result == ERROR_SUCCESS)
        {
            state->setError({});
        }
        else
        {
            state->setError(windowsError(result));
        }
        if (returnedInstance != nullptr)
        {
            DnsServiceFreeInstance(returnedInstance);
        }

        // This store is deliberately the final access to state. stop() can
        // safely observe a completed callback through acquire ordering.
        state->status.store(
            result == ERROR_SUCCESS
                ? DiscoveryStatus::Published
                : DiscoveryStatus::Failed,
            std::memory_order_release);
    }

    static void WINAPI deregistrationComplete(
        DWORD,
        void* context,
        DNS_SERVICE_INSTANCE* returnedInstance)
    {
        auto* state = static_cast<State*>(context);
        if (returnedInstance != nullptr)
        {
            DnsServiceFreeInstance(returnedInstance);
        }
        state->status.store(
            DiscoveryStatus::Stopped,
            std::memory_order_release);
        SetEvent(state->deregisteredEvent);
    }
#endif
};

DiscoveryPublisher::DiscoveryPublisher() = default;

DiscoveryPublisher::~DiscoveryPublisher()
{
    stop();
}

bool DiscoveryPublisher::start(const DiscoveryConfig& config)
{
    stop();
    state_ = std::make_unique<State>();

#ifdef _WIN32
    State& state = *state_;
    state.instanceName = discoveryServiceFqdn(config.instanceId).toStdWString();
    state.hostName = computerMdnsName();
    state.propertyValue = config.name.trimmed().left(63).toStdWString();
    if (state.propertyValue.empty())
    {
        state.propertyValue = L"Mikhlink OBS";
    }
    state.keys[0] = state.propertyKey.data();
    state.values[0] = state.propertyValue.data();

    state.instance.pszInstanceName = state.instanceName.data();
    state.instance.pszHostName = state.hostName.data();
    state.instance.ip4Address = nullptr;
    state.instance.ip6Address = nullptr;
    state.instance.wPort = config.port;
    state.instance.wPriority = 0;
    state.instance.wWeight = 0;
    state.instance.dwPropertyCount = 1;
    state.instance.keys = state.keys.data();
    state.instance.values = state.values.data();
    state.instance.dwInterfaceIndex = 0;

    state.request.Version = DNS_QUERY_REQUEST_VERSION1;
    state.request.InterfaceIndex = 0;
    state.request.pServiceInstance = &state.instance;
    state.request.pRegisterCompletionCallback = State::registrationComplete;
    state.request.pQueryContext = &state;
    state.request.hCredentials = nullptr;
    state.request.unicastEnabled = FALSE;
    state.deregisteredEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (state.deregisteredEvent == nullptr)
    {
        state.setError(windowsError(GetLastError()));
        state.status.store(DiscoveryStatus::Failed, std::memory_order_release);
        return false;
    }

    state.status.store(DiscoveryStatus::Registering, std::memory_order_release);
    const DWORD result = DnsServiceRegister(&state.request, &state.cancel);
    if (result != DNS_REQUEST_PENDING)
    {
        state.setError(windowsError(result));
        state.status.store(DiscoveryStatus::Failed, std::memory_order_release);
        return false;
    }
    state.registrationRequested = true;
    return true;
#else
    state_->error = QStringLiteral(
        "Moblink automatic discovery is currently available on Windows only");
    state_->status.store(DiscoveryStatus::Failed, std::memory_order_release);
    return false;
#endif
}

void DiscoveryPublisher::stop()
{
    if (!state_)
    {
        return;
    }

#ifdef _WIN32
    State& state = *state_;
    const DiscoveryStatus current =
        state.status.load(std::memory_order_acquire);
    if (!state.registrationRequested || current == DiscoveryStatus::Failed)
    {
        state_.reset();
        return;
    }

    if (current == DiscoveryStatus::Registering)
    {
        // Windows cancellation is synchronous. It prevents a late
        // registration callback from targeting state after this point.
        DnsServiceRegisterCancel(&state.cancel);
    }

    state.request.pRegisterCompletionCallback =
        State::deregistrationComplete;
    state.request.pQueryContext = &state;
    ResetEvent(state.deregisteredEvent);
    const DWORD result = DnsServiceDeRegister(&state.request, nullptr);
    if (result == DNS_REQUEST_PENDING)
    {
        const DWORD waitResult = WaitForSingleObject(
            state.deregisteredEvent,
            DiscoveryStopTimeoutMs);
        if (waitResult != WAIT_OBJECT_0)
        {
            // A DNS callback must never target freed state. A timeout is
            // highly unusual, but preserving this tiny object is safer than
            // risking a use-after-free during OBS shutdown.
            static std::mutex preservedStatesMutex;
            static std::vector<std::unique_ptr<State>> preservedStates;
            std::lock_guard<std::mutex> lock(preservedStatesMutex);
            preservedStates.push_back(std::move(state_));
            return;
        }
    }
    state_.reset();
#else
    state_.reset();
#endif
}

DiscoveryStatus DiscoveryPublisher::status() const
{
    return state_
        ? state_->status.load(std::memory_order_acquire)
        : DiscoveryStatus::Stopped;
}

QString DiscoveryPublisher::lastError() const
{
    if (!state_)
    {
        return {};
    }
    std::lock_guard<std::mutex> lock(state_->errorMutex);
    return state_->error;
}

} // namespace mikhlink::moblink
