#include "moblink/MoblinkServer.h"
#include "moblink/MoblinkDiscovery.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpSocket>
#include <QTimer>

#include <cstdint>

namespace
{

QByteArray authentication(
    const QString& password,
    const QString& salt,
    const QString& challenge)
{
    const QByteArray first = QCryptographicHash::hash(
        (password + salt).toUtf8(), QCryptographicHash::Sha256).toBase64();
    return QCryptographicHash::hash(
        first + challenge.toUtf8(), QCryptographicHash::Sha256).toBase64();
}

QByteArray maskedFrame(std::uint8_t opcode, const QByteArray& payload)
{
    static constexpr unsigned char Mask[] = {0x12, 0x34, 0x56, 0x78};

    QByteArray frame;
    frame.append(static_cast<char>(0x80U | opcode));
    if (payload.size() <= 125)
    {
        frame.append(static_cast<char>(0x80U | payload.size()));
    }
    else
    {
        frame.append(static_cast<char>(0x80U | 126U));
        frame.append(static_cast<char>((payload.size() >> 8) & 0xFF));
        frame.append(static_cast<char>(payload.size() & 0xFF));
    }
    for (const unsigned char byte : Mask)
    {
        frame.append(static_cast<char>(byte));
    }
    for (qsizetype index = 0; index < payload.size(); ++index)
    {
        frame.append(static_cast<char>(
            static_cast<unsigned char>(payload.at(index)) ^ Mask[index % 4]));
    }
    return frame;
}

void sendJson(QTcpSocket& socket, const QJsonObject& object)
{
    socket.write(maskedFrame(
        0x1,
        QJsonDocument(object).toJson(QJsonDocument::Compact)));
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    constexpr auto Password = "correct horse battery staple";
    constexpr auto RelayId = "550e8400-e29b-41d4-a716-446655440000";

    if (mikhlink::moblink::discoveryServiceFqdn(RelayId) !=
        QStringLiteral(
            "550e8400-e29b-41d4-a716-446655440000._moblink._tcp.local"))
    {
        qCritical("Moblink DNS-SD service name mismatch");
        return 1;
    }

    mikhlink::moblink::Server server;
    mikhlink::moblink::ServerConfig config;
    config.enabled = true;
    config.port = 0;
    config.discoveryId = RelayId;
    config.name = "Mikhlink integration test";
    config.password = Password;
    config.destinationHost = "srtla.example.test";
    config.destinationPort = 5000;

    int result = 1;
    bool sawHello = false;
    bool sawIdentified = false;
    bool sawStartTunnel = false;
    bool sawStatus = false;
    server.onRelaysChanged = [&](
        const std::vector<mikhlink::moblink::RelaySnapshot>& relays) {
        if (relays.size() != 1 || !relays.front().tunnelReady ||
            relays.front().batteryPercentage != 73 ||
            relays.front().thermalState != "yellow")
        {
            return;
        }
        if (relays.front().id != RelayId ||
            relays.front().name != "Test phone" ||
            relays.front().tunnelPort != 45678 ||
            !sawHello || !sawIdentified || !sawStartTunnel || !sawStatus)
        {
            qCritical("Moblink relay snapshot did not preserve protocol identity");
            application.exit(1);
            return;
        }
        result = 0;
        application.quit();
    };

    if (!server.configure(config) || server.port() == 0)
    {
        qCritical("Moblink test server failed to listen");
        return 1;
    }

    QTcpSocket relay;
    QByteArray input;
    bool handshakeComplete = false;

    QObject::connect(
        &relay,
        &QTcpSocket::connected,
        [&] {
            relay.write(
                "GET / HTTP/1.1\r\n"
                "Host: 127.0.0.1\r\n"
                "Upgrade: websocket\r\n"
                "Connection: Upgrade\r\n"
                "Sec-WebSocket-Version: 13\r\n"
                "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n");
        });

    QObject::connect(
        &relay,
        &QTcpSocket::readyRead,
        [&] {
            input += relay.readAll();
            if (!handshakeComplete)
            {
                const qsizetype headerEnd = input.indexOf("\r\n\r\n");
                if (headerEnd < 0)
                {
                    return;
                }
                const QByteArray header = input.left(headerEnd + 4);
                input.remove(0, headerEnd + 4);
                if (!header.startsWith("HTTP/1.1 101"))
                {
                    qCritical("Moblink WebSocket upgrade failed");
                    application.exit(1);
                    return;
                }
                handshakeComplete = true;
            }

            while (input.size() >= 2)
            {
                const auto first = static_cast<unsigned char>(input.at(0));
                const auto second = static_cast<unsigned char>(input.at(1));
                quint64 length = second & 0x7FU;
                qsizetype offset = 2;
                if ((second & 0x80U) != 0)
                {
                    qCritical("Server WebSocket frames must not be masked");
                    application.exit(1);
                    return;
                }
                if (length == 126)
                {
                    if (input.size() < 4)
                    {
                        return;
                    }
                    length =
                        (static_cast<unsigned char>(input.at(2)) << 8U) |
                        static_cast<unsigned char>(input.at(3));
                    offset = 4;
                }
                if (length > 65535 ||
                    input.size() < offset + static_cast<qsizetype>(length))
                {
                    return;
                }

                const QByteArray payload = input.mid(
                    offset, static_cast<qsizetype>(length));
                input.remove(0, offset + static_cast<qsizetype>(length));
                const std::uint8_t opcode = first & 0x0FU;
                if (opcode == 0x9)
                {
                    relay.write(maskedFrame(0xA, payload));
                    continue;
                }
                if (opcode != 0x1)
                {
                    continue;
                }

                const QJsonDocument document = QJsonDocument::fromJson(payload);
                if (!document.isObject())
                {
                    qCritical("Moblink server sent invalid JSON");
                    application.exit(1);
                    return;
                }
                const QJsonObject object = document.object();
                if (object.value("hello").isObject())
                {
                    const QJsonObject hello = object.value("hello").toObject();
                    const QJsonObject auth =
                        hello.value("authentication").toObject();
                    if (hello.value("apiVersion").toString() != "0.1")
                    {
                        qCritical("Moblink API version mismatch");
                        application.exit(1);
                        return;
                    }
                    sawHello = true;
                    sendJson(relay, QJsonObject{{
                        "identify",
                        QJsonObject{
                            {"id", RelayId},
                            {"name", "Test phone\nignored"},
                            {"authentication",
                             QString::fromUtf8(authentication(
                                 Password,
                                 auth.value("salt").toString(),
                                 auth.value("challenge").toString()))}}}});
                }
                else if (object.value("identified").isObject())
                {
                    sawIdentified = object.value("identified")
                        .toObject()
                        .value("result")
                        .toObject()
                        .value("ok")
                        .isObject();
                }
                else if (object.value("request").isObject())
                {
                    const QJsonObject request = object.value("request").toObject();
                    const int id = request.value("id").toInt();
                    const QJsonObject data = request.value("data").toObject();
                    if (data.value("startTunnel").isObject())
                    {
                        const QJsonObject start =
                            data.value("startTunnel").toObject();
                        sawStartTunnel =
                            start.value("address").toString() ==
                                "srtla.example.test" &&
                            start.value("port").toInt() == 5000;
                        sendJson(relay, QJsonObject{{
                            "response",
                            QJsonObject{
                                {"id", id},
                                {"result", QJsonObject{{"ok", QJsonObject{}}}},
                                {"data",
                                 QJsonObject{{
                                     "startTunnel",
                                     QJsonObject{{"port", 45678}}}}}}}});
                    }
                    else if (data.value("status").isObject())
                    {
                        sawStatus = true;
                        sendJson(relay, QJsonObject{{
                            "response",
                            QJsonObject{
                                {"id", id},
                                {"result", QJsonObject{{"ok", QJsonObject{}}}},
                                {"data",
                                 QJsonObject{{
                                     "status",
                                     QJsonObject{
                                         {"batteryPercentage", 73},
                                         {"thermalState", "yellow"}}}}}}}});
                    }
                }
            }
        });

    QTimer::singleShot(10000, [&] {
        qCritical("Moblink protocol test timed out");
        application.exit(1);
    });
    relay.connectToHost(QHostAddress::LocalHost, server.port());
    application.exec();
    return result;
}
