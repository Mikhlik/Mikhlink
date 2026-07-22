#include "MoblinkServer.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <utility>

namespace mikhlink::moblink
{
namespace
{

constexpr auto WebSocketGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
constexpr qsizetype MaxHttpHeaderBytes = 16 * 1024;
constexpr quint64 MaxFramePayloadBytes = 64 * 1024;
constexpr std::size_t MaxClientCount = 32;
constexpr qint64 HandshakeTimeoutMs = 10000;
constexpr qint64 StatusIntervalMs = 1000;
constexpr qint64 RequestTimeoutMs = 5000;
constexpr qint64 PingIntervalMs = 10000;

QByteArray passwordHash(
    const QString& password,
    const QString& salt,
    const QString& challenge)
{
    const QByteArray first = QCryptographicHash::hash(
        (password + salt).toUtf8(), QCryptographicHash::Sha256).toBase64();
    return QCryptographicHash::hash(
        first + challenge.toUtf8(), QCryptographicHash::Sha256).toBase64();
}

bool constantTimeEquals(const QByteArray& left, const QByteArray& right)
{
    if (left.size() != right.size())
    {
        return false;
    }

    unsigned char difference = 0;
    for (qsizetype index = 0; index < left.size(); ++index)
    {
        difference |= static_cast<unsigned char>(left.at(index)) ^
                      static_cast<unsigned char>(right.at(index));
    }
    return difference == 0;
}

QJsonObject present()
{
    return {};
}

QString cleanRelayName(const QString& name)
{
    QString cleaned = name;
    const qsizetype newline = cleaned.indexOf(QRegularExpression("[\\r\\n]"));
    if (newline >= 0)
    {
        cleaned.truncate(newline);
    }
    cleaned = cleaned.trimmed().left(30);
    return cleaned.isEmpty() ? QStringLiteral("Moblink phone") : cleaned;
}

} // namespace

struct Server::Client
{
    explicit Client(QTcpSocket* value)
        : socket(value),
          peerAddress(value->peerAddress()),
          localAddress(value->localAddress())
    {
    }

    QTcpSocket* socket = nullptr;
    QByteArray input;
    QByteArray fragmentedText;
    bool handshakeComplete = false;
    bool fragmentedTextActive = false;
    bool identified = false;
    bool closing = false;
    bool pongReceived = true;
    QString challenge;
    QString salt;
    QString id;
    QString name;
    QHostAddress peerAddress;
    QHostAddress localAddress;
    std::uint16_t tunnelPort = 0;
    int batteryPercentage = -1;
    QString thermalState;
    int nextRequestId = 1;
    int startTunnelRequestId = 0;
    int statusRequestId = 0;
    qint64 connectedAt = QDateTime::currentMSecsSinceEpoch();
    qint64 startTunnelRequestAt = 0;
    qint64 statusRequestAt = 0;
    qint64 nextPingAt = 0;
};

Server::Server(QObject* parent)
    : QObject(parent),
      tcpServer_(std::make_unique<QTcpServer>(this)),
      timer_(std::make_unique<QTimer>(this))
{
    QObject::connect(
        tcpServer_.get(),
        &QTcpServer::newConnection,
        this,
        [this] { acceptConnections(); });
    QObject::connect(
        timer_.get(),
        &QTimer::timeout,
        this,
        [this] { timerTick(); });
    timer_->setInterval(250);
    timer_->start();
}

Server::~Server()
{
    stop();
}

bool Server::configure(const ServerConfig& config)
{
    const bool listenerChanged =
        config_.enabled != config.enabled ||
        config_.port != config.port ||
        config_.password != config.password;
    const bool destinationChanged =
        config_.destinationHost != config.destinationHost ||
        config_.destinationPort != config.destinationPort;

    config_ = config;
    config_.name = config_.name.trimmed().left(63);
    config_.destinationHost = config_.destinationHost.trimmed();

    if (!config_.enabled)
    {
        stopListening();
        lastError_.clear();
        return true;
    }

    if (config_.password.isEmpty())
    {
        stopListening();
        lastError_ = QStringLiteral("Moblink password is empty");
        if (onLog)
        {
            onLog(lastError_);
        }
        return false;
    }

    if (listenerChanged || !tcpServer_->isListening())
    {
        stopListening();
        startListening();
    }
    else if (destinationChanged)
    {
        for (const auto& client : clients_)
        {
            if (client->identified)
            {
                sendStartTunnel(*client);
            }
        }
    }

    return tcpServer_->isListening();
}

void Server::stop()
{
    stopListening();
    if (timer_)
    {
        timer_->stop();
    }
}

bool Server::isListening() const
{
    return tcpServer_->isListening();
}

QString Server::lastError() const
{
    return lastError_;
}

std::uint16_t Server::port() const
{
    return config_.port;
}

std::vector<RelaySnapshot> Server::relays() const
{
    std::vector<RelaySnapshot> result;
    result.reserve(clients_.size());
    for (const auto& client : clients_)
    {
        if (!client->identified)
        {
            continue;
        }
        result.push_back({
            client->id,
            client->name,
            client->peerAddress,
            client->localAddress,
            client->tunnelPort,
            client->batteryPercentage,
            client->thermalState,
            client->tunnelPort != 0});
    }
    std::sort(
        result.begin(),
        result.end(),
        [](const RelaySnapshot& left, const RelaySnapshot& right) {
            return left.name.compare(right.name, Qt::CaseInsensitive) < 0;
        });
    return result;
}

void Server::startListening()
{
    lastError_.clear();
    if (!tcpServer_->listen(QHostAddress::AnyIPv4, config_.port))
    {
        lastError_ = tcpServer_->errorString();
        if (onLog)
        {
            onLog(QStringLiteral("Moblink server failed to listen: ") + lastError_);
        }
        return;
    }

    if (onLog)
    {
        onLog(QStringLiteral("Moblink server listening on port %1").arg(config_.port));
    }
    notifyRelaysChanged();
}

void Server::stopListening()
{
    if (tcpServer_->isListening())
    {
        tcpServer_->close();
    }

    auto clients = std::move(clients_);
    clients_.clear();
    for (const auto& client : clients)
    {
        if (client->socket != nullptr)
        {
            client->socket->disconnect(this);
            client->socket->close();
            client->socket->deleteLater();
        }
    }
    notifyRelaysChanged();
}

void Server::acceptConnections()
{
    while (QTcpSocket* socket = tcpServer_->nextPendingConnection())
    {
        if (clients_.size() >= MaxClientCount)
        {
            if (onLog)
            {
                onLog(QStringLiteral("Moblink connection limit reached"));
            }
            socket->disconnectFromHost();
            socket->deleteLater();
            continue;
        }

        clients_.push_back(std::make_unique<Client>(socket));
        QObject::connect(
            socket,
            &QTcpSocket::readyRead,
            this,
            [this, socket] { readClient(socket); });
        QObject::connect(
            socket,
            &QTcpSocket::disconnected,
            this,
            [this, socket] { clientDisconnected(socket); });
    }
}

Server::Client* Server::findClient(QTcpSocket* socket)
{
    const auto iterator = std::find_if(
        clients_.begin(),
        clients_.end(),
        [socket](const std::unique_ptr<Client>& client) {
            return client->socket == socket;
        });
    return iterator == clients_.end() ? nullptr : iterator->get();
}

const Server::Client* Server::findClient(QTcpSocket* socket) const
{
    const auto iterator = std::find_if(
        clients_.cbegin(),
        clients_.cend(),
        [socket](const std::unique_ptr<Client>& client) {
            return client->socket == socket;
        });
    return iterator == clients_.cend() ? nullptr : iterator->get();
}

void Server::readClient(QTcpSocket* socket)
{
    Client* client = findClient(socket);
    if (client == nullptr || client->closing)
    {
        return;
    }

    client->input += socket->readAll();
    if (!client->handshakeComplete && !processHandshake(*client))
    {
        return;
    }
    processFrames(*client);
}

bool Server::processHandshake(Client& client)
{
    const qsizetype end = client.input.indexOf("\r\n\r\n");
    if (end < 0)
    {
        if (client.input.size() > MaxHttpHeaderBytes)
        {
            disconnectClient(client.socket, QStringLiteral("HTTP header is too large"));
        }
        return false;
    }

    const QByteArray header = client.input.left(end + 4);
    client.input.remove(0, end + 4);
    const QList<QByteArray> lines = header.split('\n');
    if (lines.isEmpty() || !lines.first().trimmed().startsWith("GET "))
    {
        disconnectClient(client.socket, QStringLiteral("Invalid WebSocket request"));
        return false;
    }

    QByteArray webSocketKey;
    QByteArray version;
    QByteArray upgrade;
    QByteArray connection;
    for (qsizetype index = 1; index < lines.size(); ++index)
    {
        const QByteArray line = lines.at(index).trimmed();
        const qsizetype separator = line.indexOf(':');
        if (separator <= 0)
        {
            continue;
        }
        const QByteArray name = line.left(separator).trimmed().toLower();
        const QByteArray value = line.mid(separator + 1).trimmed();
        if (name == "sec-websocket-key")
        {
            webSocketKey = value;
        }
        else if (name == "sec-websocket-version")
        {
            version = value;
        }
        else if (name == "upgrade")
        {
            upgrade = value.toLower();
        }
        else if (name == "connection")
        {
            connection = value.toLower();
        }
    }

    if (webSocketKey.isEmpty() || version != "13" || upgrade != "websocket" ||
        !connection.contains("upgrade"))
    {
        disconnectClient(client.socket, QStringLiteral("Unsupported WebSocket handshake"));
        return false;
    }

    const QByteArray accept = QCryptographicHash::hash(
        webSocketKey + WebSocketGuid,
        QCryptographicHash::Sha1).toBase64();
    client.socket->write(
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + accept + "\r\n\r\n");
    client.handshakeComplete = true;
    client.challenge = QUuid::createUuid().toString(QUuid::WithoutBraces);
    client.salt = QUuid::createUuid().toString(QUuid::WithoutBraces);
    client.nextPingAt = QDateTime::currentMSecsSinceEpoch() + PingIntervalMs;
    sendHello(client);
    return true;
}

bool Server::processFrames(Client& client)
{
    while (client.input.size() >= 2)
    {
        const auto first = static_cast<unsigned char>(client.input.at(0));
        const auto second = static_cast<unsigned char>(client.input.at(1));
        const bool final = (first & 0x80U) != 0;
        const std::uint8_t opcode = first & 0x0FU;
        const bool masked = (second & 0x80U) != 0;
        quint64 payloadLength = second & 0x7FU;
        qsizetype offset = 2;

        if (payloadLength == 126)
        {
            if (client.input.size() < 4)
            {
                return true;
            }
            payloadLength =
                (static_cast<unsigned char>(client.input.at(2)) << 8U) |
                static_cast<unsigned char>(client.input.at(3));
            offset = 4;
        }
        else if (payloadLength == 127)
        {
            if (client.input.size() < 10)
            {
                return true;
            }
            payloadLength = 0;
            for (int index = 0; index < 8; ++index)
            {
                payloadLength = (payloadLength << 8U) |
                    static_cast<unsigned char>(client.input.at(2 + index));
            }
            offset = 10;
        }

        if (!masked || payloadLength > MaxFramePayloadBytes ||
            payloadLength > static_cast<quint64>(std::numeric_limits<qsizetype>::max()))
        {
            disconnectClient(client.socket, QStringLiteral("Invalid WebSocket frame"));
            return false;
        }
        if ((opcode & 0x08U) != 0 && (!final || payloadLength > 125))
        {
            disconnectClient(client.socket, QStringLiteral("Invalid WebSocket control frame"));
            return false;
        }
        if (client.input.size() < offset + 4 + static_cast<qsizetype>(payloadLength))
        {
            return true;
        }

        const QByteArray mask = client.input.mid(offset, 4);
        offset += 4;
        QByteArray payload = client.input.mid(
            offset, static_cast<qsizetype>(payloadLength));
        client.input.remove(
            0, offset + static_cast<qsizetype>(payloadLength));
        for (qsizetype index = 0; index < payload.size(); ++index)
        {
            payload[index] = payload.at(index) ^ mask.at(index % 4);
        }

        if (opcode == 0x8)
        {
            sendFrame(client, 0x8, payload.left(125));
            client.socket->disconnectFromHost();
            return false;
        }
        if (opcode == 0x9)
        {
            sendFrame(client, 0xA, payload);
            continue;
        }
        if (opcode == 0xA)
        {
            client.pongReceived = true;
            continue;
        }
        if (opcode == 0x1)
        {
            if (client.fragmentedTextActive)
            {
                disconnectClient(client.socket, QStringLiteral("Overlapping WebSocket fragments"));
                return false;
            }
            if (final)
            {
                QTcpSocket* socket = client.socket;
                processTextMessage(client, payload);
                if (findClient(socket) == nullptr)
                {
                    return false;
                }
            }
            else
            {
                client.fragmentedText = payload;
                client.fragmentedTextActive = true;
            }
            continue;
        }
        if (opcode == 0x0 && client.fragmentedTextActive)
        {
            client.fragmentedText += payload;
            if (client.fragmentedText.size() > MaxFramePayloadBytes)
            {
                disconnectClient(client.socket, QStringLiteral("WebSocket message is too large"));
                return false;
            }
            if (final)
            {
                const QByteArray message = std::move(client.fragmentedText);
                client.fragmentedText.clear();
                client.fragmentedTextActive = false;
                QTcpSocket* socket = client.socket;
                processTextMessage(client, message);
                if (findClient(socket) == nullptr)
                {
                    return false;
                }
            }
            continue;
        }

        disconnectClient(client.socket, QStringLiteral("Unsupported WebSocket frame"));
        return false;
    }
    return true;
}

void Server::processTextMessage(Client& client, const QByteArray& payload)
{
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
    {
        disconnectClient(client.socket, QStringLiteral("Invalid Moblink JSON"));
        return;
    }

    const QJsonObject object = document.object();
    if (object.value("identify").isObject())
    {
        handleIdentify(client, object.value("identify").toObject());
    }
    else if (object.value("response").isObject())
    {
        handleResponse(client, object.value("response").toObject());
    }
    else
    {
        disconnectClient(client.socket, QStringLiteral("Unknown Moblink message"));
    }
}

void Server::handleIdentify(Client& client, const QJsonObject& identify)
{
    if (client.identified)
    {
        disconnectClient(client.socket, QStringLiteral("Relay identified twice"));
        return;
    }

    const QString rawId = identify.value("id").toString().trimmed();
    const QUuid uuid(rawId);
    const QByteArray received = identify.value("authentication").toString().toUtf8();
    const QByteArray expected = passwordHash(
        config_.password, client.salt, client.challenge);
    if (uuid.isNull() || !constantTimeEquals(received, expected))
    {
        sendJson(client, QJsonObject{{
            "identified",
            QJsonObject{{"result", QJsonObject{{"wrongPassword", present()}}}}}});
        disconnectClient(client.socket, QStringLiteral("Moblink authentication failed"));
        return;
    }

    const QString canonicalId = uuid.toString(QUuid::WithoutBraces);
    Client* duplicate = nullptr;
    for (const auto& other : clients_)
    {
        if (other.get() != &client && other->identified && other->id == canonicalId)
        {
            duplicate = other.get();
            break;
        }
    }
    if (duplicate != nullptr)
    {
        // Remove the previous connection from snapshots immediately. Waiting
        // for its asynchronous disconnected() signal would briefly publish
        // two descriptors with the same stable UUID.
        duplicate->identified = false;
        duplicate->tunnelPort = 0;
        disconnectClient(duplicate->socket, QStringLiteral("Relay reconnected"));
    }

    client.id = canonicalId;
    client.name = cleanRelayName(identify.value("name").toString());
    client.identified = true;
    sendJson(client, QJsonObject{{
        "identified",
        QJsonObject{{"result", QJsonObject{{"ok", present()}}}}}});

    if (onLog)
    {
        onLog(QStringLiteral("Moblink relay authenticated: %1 (%2)")
                  .arg(client.name, client.peerAddress.toString()));
    }
    sendStartTunnel(client);
    sendStatusRequest(client);
    notifyRelaysChanged();
}

void Server::handleResponse(Client& client, const QJsonObject& response)
{
    if (!client.identified)
    {
        disconnectClient(client.socket, QStringLiteral("Response before identification"));
        return;
    }

    const int id = response.value("id").toInt();
    const bool ok = response.value("result").toObject().value("ok").isObject();
    if (!ok)
    {
        if (id == client.startTunnelRequestId)
        {
            client.startTunnelRequestId = 0;
            client.tunnelPort = 0;
            client.startTunnelRequestAt = QDateTime::currentMSecsSinceEpoch();
        }
        if (id == client.statusRequestId)
        {
            client.statusRequestId = 0;
        }
        notifyRelaysChanged();
        return;
    }

    const QJsonObject data = response.value("data").toObject();
    if (id == client.startTunnelRequestId)
    {
        client.startTunnelRequestId = 0;
        const int port = data.value("startTunnel").toObject().value("port").toInt();
        if (port < 1 || port > 65535)
        {
            client.tunnelPort = 0;
            if (onLog)
            {
                onLog(QStringLiteral("Moblink relay returned an invalid UDP port"));
            }
        }
        else
        {
            client.tunnelPort = static_cast<std::uint16_t>(port);
            if (onLog)
            {
                onLog(QStringLiteral("Moblink tunnel ready: %1 at %2:%3")
                          .arg(client.name, client.peerAddress.toString())
                          .arg(port));
            }
        }
        notifyRelaysChanged();
    }
    else if (id == client.statusRequestId)
    {
        client.statusRequestId = 0;
        const QJsonObject status = data.value("status").toObject();
        if (status.value("batteryPercentage").isDouble())
        {
            client.batteryPercentage = std::clamp(
                status.value("batteryPercentage").toInt(), 0, 100);
        }
        else
        {
            client.batteryPercentage = -1;
        }
        const QString thermal = status.value("thermalState").toString();
        client.thermalState =
            thermal == "white" || thermal == "yellow" || thermal == "red"
                ? thermal
                : QString();
        notifyRelaysChanged();
    }
}

void Server::sendHello(Client& client)
{
    sendJson(client, QJsonObject{{
        "hello",
        QJsonObject{
            {"apiVersion", "0.1"},
            {"authentication",
             QJsonObject{
                 {"challenge", client.challenge},
                 {"salt", client.salt}}}}}});
}

void Server::sendStartTunnel(Client& client)
{
    client.tunnelPort = 0;
    client.startTunnelRequestId = 0;
    if (!client.identified || config_.destinationHost.isEmpty() ||
        config_.destinationPort == 0)
    {
        notifyRelaysChanged();
        return;
    }

    const int id = client.nextRequestId++;
    client.startTunnelRequestId = id;
    client.startTunnelRequestAt = QDateTime::currentMSecsSinceEpoch();
    sendJson(client, QJsonObject{{
        "request",
        QJsonObject{
            {"id", id},
            {"data",
             QJsonObject{{
                 "startTunnel",
                 QJsonObject{
                     {"address", config_.destinationHost},
                     {"port", static_cast<int>(config_.destinationPort)}}}}}}}});
    notifyRelaysChanged();
}

void Server::sendStatusRequest(Client& client)
{
    if (!client.identified || client.statusRequestId != 0)
    {
        return;
    }
    const int id = client.nextRequestId++;
    client.statusRequestId = id;
    client.statusRequestAt = QDateTime::currentMSecsSinceEpoch();
    sendJson(client, QJsonObject{{
        "request",
        QJsonObject{
            {"id", id},
            {"data", QJsonObject{{"status", present()}}}}}});
}

void Server::sendJson(Client& client, const QJsonObject& object)
{
    sendFrame(
        client,
        0x1,
        QJsonDocument(object).toJson(QJsonDocument::Compact));
}

void Server::sendFrame(
    Client& client,
    std::uint8_t opcode,
    const QByteArray& payload)
{
    if (client.socket == nullptr ||
        client.socket->state() == QAbstractSocket::UnconnectedState)
    {
        return;
    }

    QByteArray frame;
    frame.reserve(payload.size() + 10);
    frame.append(static_cast<char>(0x80U | opcode));
    if (payload.size() <= 125)
    {
        frame.append(static_cast<char>(payload.size()));
    }
    else if (payload.size() <= 65535)
    {
        frame.append(static_cast<char>(126));
        frame.append(static_cast<char>((payload.size() >> 8) & 0xFF));
        frame.append(static_cast<char>(payload.size() & 0xFF));
    }
    else
    {
        frame.append(static_cast<char>(127));
        const quint64 size = static_cast<quint64>(payload.size());
        for (int shift = 56; shift >= 0; shift -= 8)
        {
            frame.append(static_cast<char>((size >> shift) & 0xFFU));
        }
    }
    frame += payload;
    client.socket->write(frame);
}

void Server::disconnectClient(QTcpSocket* socket, const QString& reason)
{
    Client* client = findClient(socket);
    if (client == nullptr || client->closing)
    {
        return;
    }
    client->closing = true;
    if (onLog && !reason.isEmpty())
    {
        onLog(QStringLiteral("Moblink connection closed: ") + reason);
    }
    if (client->handshakeComplete)
    {
        sendFrame(*client, 0x8);
    }
    socket->disconnectFromHost();
    if (socket->state() != QAbstractSocket::UnconnectedState)
    {
        // Give the WebSocket close frame (and, for authentication failures,
        // the protocol response immediately before it) time to reach the
        // relay. The socket is the timer context, so the callback is dropped
        // automatically if disconnected() deletes it first.
        QTimer::singleShot(2000, socket, [socket] {
            if (socket->state() != QAbstractSocket::UnconnectedState)
            {
                socket->abort();
            }
        });
    }
}

void Server::clientDisconnected(QTcpSocket* socket)
{
    const auto iterator = std::find_if(
        clients_.begin(),
        clients_.end(),
        [socket](const std::unique_ptr<Client>& client) {
            return client->socket == socket;
        });
    if (iterator == clients_.end())
    {
        return;
    }

    const QString relayName = (*iterator)->name;
    clients_.erase(iterator);
    socket->deleteLater();
    if (onLog && !relayName.isEmpty())
    {
        onLog(QStringLiteral("Moblink relay disconnected: ") + relayName);
    }
    notifyRelaysChanged();
}

void Server::timerTick()
{
    if (!tcpServer_->isListening())
    {
        return;
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    std::vector<QTcpSocket*> handshakeTimeouts;
    std::vector<QTcpSocket*> pingTimeouts;
    for (const auto& client : clients_)
    {
        if (client->closing)
        {
            continue;
        }
        if (!client->handshakeComplete)
        {
            if (now - client->connectedAt >= HandshakeTimeoutMs)
            {
                handshakeTimeouts.push_back(client->socket);
            }
            continue;
        }
        if (now >= client->nextPingAt)
        {
            if (!client->pongReceived)
            {
                pingTimeouts.push_back(client->socket);
                continue;
            }
            client->pongReceived = false;
            client->nextPingAt = now + PingIntervalMs;
            sendFrame(*client, 0x9);
        }
        if (!client->identified)
        {
            continue;
        }
        if (client->startTunnelRequestId != 0 &&
            now - client->startTunnelRequestAt >= RequestTimeoutMs)
        {
            client->startTunnelRequestId = 0;
            sendStartTunnel(*client);
        }
        else if (client->tunnelPort == 0 &&
                 client->startTunnelRequestId == 0 &&
                 !config_.destinationHost.isEmpty() &&
                 now - client->startTunnelRequestAt >= RequestTimeoutMs)
        {
            sendStartTunnel(*client);
        }
        if (client->statusRequestId != 0 &&
            now - client->statusRequestAt >= RequestTimeoutMs)
        {
            client->statusRequestId = 0;
        }
        if (client->statusRequestId == 0 &&
            now - client->statusRequestAt >= StatusIntervalMs)
        {
            sendStatusRequest(*client);
        }
    }
    for (QTcpSocket* socket : handshakeTimeouts)
    {
        disconnectClient(socket, QStringLiteral("WebSocket handshake timeout"));
    }
    for (QTcpSocket* socket : pingTimeouts)
    {
        disconnectClient(socket, QStringLiteral("ping timeout"));
    }
}

void Server::notifyRelaysChanged()
{
    if (onRelaysChanged)
    {
        onRelaysChanged(relays());
    }
}

} // namespace mikhlink::moblink
