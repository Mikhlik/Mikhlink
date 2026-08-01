#pragma once

#include <QString>

#include <cstdint>
#include <memory>

namespace mikhlink::moblink
{

enum class DiscoveryStatus
{
    Stopped,
    Registering,
    Published,
    Failed
};

struct DiscoveryConfig
{
    QString instanceId;
    QString name;
    std::uint16_t port = 7777;
};

// Moblin browses this exact fully-qualified DNS-SD service name.
QString discoveryServiceFqdn(const QString& instanceId);

class DiscoveryPublisher final
{
public:
    DiscoveryPublisher();
    ~DiscoveryPublisher();

    DiscoveryPublisher(const DiscoveryPublisher&) = delete;
    DiscoveryPublisher& operator=(const DiscoveryPublisher&) = delete;

    bool start(const DiscoveryConfig& config);
    void stop();

    DiscoveryStatus status() const;
    QString lastError() const;

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace mikhlink::moblink
