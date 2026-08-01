#pragma once

#include <QByteArray>
#include <QHostAddress>
#include <QJsonObject>
#include <QObject>
#include <QString>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

class QTcpServer;
class QTcpSocket;
class QTimer;

namespace mikhlink::moblink
{

class DiscoveryPublisher;

struct ServerConfig
{
    bool enabled = false;
    std::uint16_t port = 7777;
    QString discoveryId;
    QString password;
    QString name = "Mikhlink OBS";
    QString destinationHost;
    std::uint16_t destinationPort = 5000;
};

struct RelaySnapshot
{
    QString id;
    QString name;
    QHostAddress peerAddress;
    QHostAddress localAddress;
    std::uint16_t tunnelPort = 0;
    int batteryPercentage = -1;
    QString thermalState;
    bool tunnelReady = false;
};

class Server final : public QObject
{
public:
    explicit Server(QObject* parent = nullptr);
    ~Server() override;

    bool configure(const ServerConfig& config);
    void stop();

    bool isListening() const;
    bool isDiscoverable() const;
    QString lastError() const;
    std::uint16_t port() const;
    std::vector<RelaySnapshot> relays() const;

    std::function<void(const std::vector<RelaySnapshot>&)> onRelaysChanged;
    std::function<void(const QString&)> onLog;

private:
    struct Client;

    void startListening();
    void stopListening();
    void startDiscovery();
    void stopDiscovery();
    void updateDiscoveryStatus();
    void acceptConnections();
    void readClient(QTcpSocket* socket);
    bool processHandshake(Client& client);
    bool processFrames(Client& client);
    void processTextMessage(Client& client, const QByteArray& payload);
    void handleIdentify(Client& client, const QJsonObject& identify);
    void handleResponse(Client& client, const QJsonObject& response);
    void sendHello(Client& client);
    void sendStartTunnel(Client& client);
    void sendStatusRequest(Client& client);
    void sendJson(Client& client, const QJsonObject& object);
    void sendFrame(Client& client, std::uint8_t opcode, const QByteArray& payload = {});
    void disconnectClient(QTcpSocket* socket, const QString& reason);
    void clientDisconnected(QTcpSocket* socket);
    void timerTick();
    void notifyRelaysChanged();
    Client* findClient(QTcpSocket* socket);
    const Client* findClient(QTcpSocket* socket) const;

    std::unique_ptr<QTcpServer> tcpServer_;
    std::unique_ptr<QTimer> timer_;
    std::unique_ptr<DiscoveryPublisher> discovery_;
    std::vector<std::unique_ptr<Client>> clients_;
    ServerConfig config_;
    QString lastError_;
    int reportedDiscoveryStatus_ = -1;
};

} // namespace mikhlink::moblink
