#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QDialog>
#include <QDialogButtonBox>
#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QLineEdit>
#include <QMessageBox>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSpinBox>
#include <QTextStream>
#include <QUrl>
#include <QUrlQuery>
#include <QWidget>

#include "network/NetworkAdapter.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <new>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

OBS_DECLARE_MODULE()

namespace
{

constexpr const char* OutputId = "mikhlink_output";
constexpr const char* ServiceId = "mikhlink_service";
constexpr const char* NameSetting = "name";
constexpr const char* IngestKeySetting = "ingest_key";
constexpr const char* SrtIngestUrlSetting = "srt_ingest_url";
constexpr const char* LegacyAddressSetting = "address";
constexpr const char* PortSetting = "port";
constexpr const char* UseSrtlaSetting = "use_srtla";
constexpr int LocalSrtlaPort = 6000;

const char* SupportedVideoCodecs[] = {"h264", "hevc", nullptr};
const char* SupportedAudioCodecs[] = {"aac", "opus", nullptr};

struct MikhlinkService
{
    std::string name;
    std::string ingestKey;
    std::string srtIngestUrl;
    std::string effectiveSrtUrl;
    int port = 5000;
    bool useSrtla = false;
};

QProcess* srtlaSender = nullptr;

struct ParsedIngest
{
    bool valid = false;
    bool keyWasEmbedded = false;
    QString effectiveUrl;
    QString key;
    QString error;
};

bool isRussianLocale()
{
    const char* locale = obs_get_locale();
    return locale != nullptr && std::strncmp(locale, "ru", 2) == 0;
}

const char* localized(const char* english, const char* russian)
{
    return isRussianLocale() ? russian : english;
}

ParsedIngest parseIngest(const QString& rawUrl, const QString& rawKey)
{
    ParsedIngest result;
    QUrl url(rawUrl.trimmed());

    if (!url.isValid() || url.scheme().compare("srt", Qt::CaseInsensitive) != 0 ||
        url.host().isEmpty() || url.port() < 1 || url.port() > 65535)
    {
        result.error = localized(
            "Enter a complete SRT URL such as srt://host:port?streamid=key.",
            "Введите полный SRT URL вида srt://сервер:порт?streamid=ключ.");
        return result;
    }

    if (url.host().endsWith(".srt.belabox.net", Qt::CaseInsensitive) &&
        url.port() == 4001)
    {
        result.error = localized(
            "Port 4001 is the BELABOX watch endpoint. Use the SRT ingest URL on port 4000.",
            "Порт 4001 — endpoint просмотра BELABOX. Используйте SRT ingest URL с портом 4000.");
        return result;
    }

    QUrlQuery query(url);
    const QString embeddedKey =
        query.queryItemValue("streamid", QUrl::FullyDecoded).trimmed();
    const QString separateKey = rawKey.trimmed();

    if (!embeddedKey.isEmpty() && !separateKey.isEmpty() &&
        embeddedKey != separateKey)
    {
        result.error = localized(
            "The ingest key in the URL differs from the separate ingest key.",
            "Ключ ingest внутри URL отличается от отдельно введённого ключа.");
        return result;
    }

    result.key = embeddedKey.isEmpty() ? separateKey : embeddedKey;
    result.keyWasEmbedded = !embeddedKey.isEmpty();
    if (result.key.isEmpty())
    {
        result.error = localized(
            "No ingest key was found in the URL or the separate field.",
            "Ключ ingest не найден ни в URL, ни в отдельном поле.");
        return result;
    }

    // OBS 32 passes streamid separately to its native SRT output. Remove it
    // from the URL so a copied BELABOX URL and the separate field behave the
    // same way and the secret is not duplicated in the destination string.
    query.removeAllQueryItems("streamid");
    url.setQuery(query);

    result.effectiveUrl = url.toString(QUrl::FullyEncoded);
    result.valid = true;
    return result;
}

struct MikhlinkOutput
{
    obs_output_t* output = nullptr;
    std::atomic<std::uint64_t> videoPackets{0};
    std::atomic<std::uint64_t> audioPackets{0};
    std::atomic<std::uint64_t> keyframes{0};
    std::atomic<std::uint64_t> totalBytes{0};
    std::chrono::steady_clock::time_point startedAt;
};

void updateServiceData(MikhlinkService* service, obs_data_t* settings)
{
    service->name = obs_data_get_string(settings, NameSetting);
    service->ingestKey = obs_data_get_string(settings, IngestKeySetting);
    service->srtIngestUrl = obs_data_get_string(settings, SrtIngestUrlSetting);
    if (service->srtIngestUrl.empty())
    {
        service->srtIngestUrl =
            obs_data_get_string(settings, LegacyAddressSetting);
    }
    service->port = static_cast<int>(obs_data_get_int(settings, PortSetting));
    service->useSrtla = obs_data_get_bool(settings, UseSrtlaSetting);
    const ParsedIngest parsed = parseIngest(
        QString::fromUtf8(service->srtIngestUrl.c_str()),
        QString::fromUtf8(service->ingestKey.c_str()));
    if (parsed.valid)
    {
        service->effectiveSrtUrl = service->useSrtla
            ? "srt://[::1]:" + std::to_string(LocalSrtlaPort) +
                  "?connect_timeout=10000&timeout=10000000"
            : parsed.effectiveUrl.toUtf8().constData();
    }
    else
    {
        service->effectiveSrtUrl.clear();
    }
    if (parsed.valid)
    {
        service->ingestKey = parsed.key.toUtf8().constData();
    }
}

const char* serviceName(void*)
{
    return "Mikhlink";
}

void* createService(obs_data_t* settings, obs_service_t*)
{
    auto* service = new (std::nothrow) MikhlinkService;
    if (service != nullptr)
    {
        updateServiceData(service, settings);
    }

    return service;
}

void destroyService(void* data)
{
    delete static_cast<MikhlinkService*>(data);
}

void updateService(void* data, obs_data_t* settings)
{
    updateServiceData(static_cast<MikhlinkService*>(data), settings);
}

void serviceDefaults(obs_data_t* settings)
{
    obs_data_set_default_string(settings, "service", "Mikhlink");
    obs_data_set_default_string(settings, NameSetting, "BELABOX");
    obs_data_set_default_string(settings, IngestKeySetting, "");
    obs_data_set_default_string(settings, SrtIngestUrlSetting, "");
    obs_data_set_default_int(settings, PortSetting, 5000);
    obs_data_set_default_bool(settings, UseSrtlaSetting, false);
}

obs_properties_t* serviceProperties(void*)
{
    obs_properties_t* properties = obs_properties_create();

    obs_properties_add_text(
        properties,
        NameSetting,
        localized("Name", "Имя"),
        OBS_TEXT_DEFAULT);

    obs_properties_add_text(
        properties,
        IngestKeySetting,
        localized("Ingest key", "Ключ ingest"),
        OBS_TEXT_PASSWORD);

    obs_properties_add_text(
        properties,
        SrtIngestUrlSetting,
        localized("SRT ingest URL", "SRT ingest URL"),
        OBS_TEXT_DEFAULT);

    obs_properties_add_int(
        properties,
        PortSetting,
        localized("SRTLA port", "Порт SRTLA"),
        1,
        65535,
        1);

    obs_properties_add_bool(
        properties,
        UseSrtlaSetting,
        localized("Use SRTLA bonding (test)", "Использовать SRTLA bonding (тест)"));

    return properties;
}

const char* serviceUrl(void* data)
{
    return static_cast<MikhlinkService*>(data)->effectiveSrtUrl.c_str();
}

const char* serviceKey(void* data)
{
    return static_cast<MikhlinkService*>(data)->ingestKey.c_str();
}

const char* serviceOutputType(void*)
{
    return "ffmpeg_mpegts_muxer";
}

const char* serviceProtocol(void*)
{
    return "SRT";
}

const char** serviceVideoCodecs(void*)
{
    return SupportedVideoCodecs;
}

const char** serviceAudioCodecs(void*)
{
    return SupportedAudioCodecs;
}

const char* serviceConnectInfo(void* data, std::uint32_t type)
{
    if (type == OBS_SERVICE_CONNECT_INFO_SERVER_URL)
    {
        return serviceUrl(data);
    }

    if (type == OBS_SERVICE_CONNECT_INFO_STREAM_ID)
    {
        return serviceKey(data);
    }

    return nullptr;
}

bool serviceCanConnect(void* data)
{
    const auto* service = static_cast<MikhlinkService*>(data);
    return !service->name.empty() && !service->effectiveSrtUrl.empty() &&
           service->port > 0;
}

const char* outputName(void*)
{
    return "Mikhlink Output";
}

void* createOutput(obs_data_t*, obs_output_t* output)
{
    auto* state = new (std::nothrow) MikhlinkOutput;
    if (state != nullptr)
    {
        state->output = output;
    }

    return state;
}

void destroyOutput(void* data)
{
    delete static_cast<MikhlinkOutput*>(data);
}

bool startOutput(void* data)
{
    auto* state = static_cast<MikhlinkOutput*>(data);
    obs_service_t* service = obs_output_get_service(state->output);

    if (service == nullptr || !obs_service_can_try_to_connect(service))
    {
        blog(LOG_ERROR,
             "[Mikhlink] Name, ingest key, SRT ingest URL, and SRTLA port are not configured.");
        return false;
    }

    obs_encoder_t* videoEncoder =
        obs_output_get_video_encoder(state->output);
    obs_encoder_t* audioEncoder =
        obs_output_get_audio_encoder(state->output, 0);

    if (videoEncoder == nullptr || audioEncoder == nullptr)
    {
        blog(LOG_ERROR,
             "[Mikhlink] OBS did not assign video and audio encoders.");
        return false;
    }

    if (!obs_output_can_begin_data_capture(state->output, 0))
    {
        blog(LOG_ERROR, "[Mikhlink] OBS refused to begin encoded capture.");
        return false;
    }

    if (!obs_output_initialize_encoders(state->output, 0))
    {
        blog(LOG_ERROR, "[Mikhlink] Failed to initialize OBS encoders.");
        return false;
    }

    state->videoPackets.store(0, std::memory_order_relaxed);
    state->audioPackets.store(0, std::memory_order_relaxed);
    state->keyframes.store(0, std::memory_order_relaxed);
    state->totalBytes.store(0, std::memory_order_relaxed);
    state->startedAt = std::chrono::steady_clock::now();

    if (!obs_output_begin_data_capture(state->output, 0))
    {
        blog(LOG_ERROR, "[Mikhlink] Failed to begin encoded capture.");
        return false;
    }

    blog(LOG_INFO,
         "[Mikhlink] Encoded capture started using video=%s, audio=%s. Destination settings are hidden.",
         obs_encoder_get_codec(videoEncoder),
         obs_encoder_get_codec(audioEncoder));

    return true;
}

void stopOutput(void* data, std::uint64_t)
{
    auto* state = static_cast<MikhlinkOutput*>(data);
    obs_output_end_data_capture(state->output);

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - state->startedAt);

    blog(LOG_INFO,
         "[Mikhlink] Encoded capture stopped after %lld ms. Video packets: %llu. Audio packets: %llu. Keyframes: %llu. Bytes: %llu.",
         static_cast<long long>(elapsed.count()),
         static_cast<unsigned long long>(
             state->videoPackets.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(
             state->audioPackets.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(
             state->keyframes.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(
             state->totalBytes.load(std::memory_order_relaxed)));
}

void receivePacket(void* data, encoder_packet* packet)
{
    auto* state = static_cast<MikhlinkOutput*>(data);

    state->totalBytes.fetch_add(
        static_cast<std::uint64_t>(packet->size),
        std::memory_order_relaxed);

    if (packet->type == OBS_ENCODER_VIDEO)
    {
        state->videoPackets.fetch_add(1, std::memory_order_relaxed);

        if (packet->keyframe)
        {
            state->keyframes.fetch_add(1, std::memory_order_relaxed);
        }
    }
    else
    {
        state->audioPackets.fetch_add(1, std::memory_order_relaxed);
    }
}

std::uint64_t outputTotalBytes(void* data)
{
    return static_cast<MikhlinkOutput*>(data)->totalBytes.load(
        std::memory_order_relaxed);
}

QString srtlaExecutablePath()
{
#ifdef _WIN32
    static int moduleAnchor = 0;
    HMODULE module = nullptr;
    if (GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&moduleAnchor),
            &module) == 0)
    {
        return {};
    }

    wchar_t modulePath[MAX_PATH] = {};
    if (GetModuleFileNameW(module, modulePath, MAX_PATH) == 0)
    {
        return {};
    }

    return QFileInfo(QString::fromWCharArray(modulePath))
        .dir()
        .filePath("srtla_send.exe");
#else
    return {};
#endif
}

void stopSrtlaSender()
{
    if (srtlaSender == nullptr)
    {
        return;
    }

    if (srtlaSender->state() != QProcess::NotRunning)
    {
        srtlaSender->terminate();
        if (!srtlaSender->waitForFinished(2000))
        {
            srtlaSender->kill();
            srtlaSender->waitForFinished(1000);
        }
    }

    delete srtlaSender;
    srtlaSender = nullptr;
    blog(LOG_INFO, "[Mikhlink] SRTLA sender stopped.");
}

bool startSrtlaSender()
{
    obs_service_t* current = obs_frontend_get_streaming_service();
    if (current == nullptr ||
        std::strcmp(obs_service_get_type(current), ServiceId) != 0)
    {
        return true;
    }

    obs_data_t* settings = obs_service_get_settings(current);
    const bool useSrtla = obs_data_get_bool(settings, UseSrtlaSetting);
    if (!useSrtla)
    {
        obs_data_release(settings);
        return true;
    }

    if (srtlaSender != nullptr &&
        srtlaSender->state() != QProcess::NotRunning)
    {
        blog(LOG_INFO, "[Mikhlink] SRTLA sender is already running.");
        return true;
    }

    const ParsedIngest parsed = parseIngest(
        QString::fromUtf8(
            obs_data_get_string(settings, SrtIngestUrlSetting)),
        QString::fromUtf8(
            obs_data_get_string(settings, IngestKeySetting)));
    const int remotePort =
        static_cast<int>(obs_data_get_int(settings, PortSetting));
    obs_data_release(settings);

    if (!parsed.valid || remotePort < 1 || remotePort > 65535)
    {
        blog(LOG_ERROR, "[Mikhlink] Invalid SRTLA destination settings.");
        return false;
    }

    const QUrl remoteDestination(parsed.effectiveUrl);
    if (remoteDestination.host().endsWith(
            ".srt.belabox.net", Qt::CaseInsensitive) &&
        remotePort != 5000)
    {
        blog(LOG_ERROR,
             "[Mikhlink] BELABOX SRTLA requires remote port 5000; configured port is %d.",
             remotePort);
        return false;
    }

    stopSrtlaSender();

    const QString executable = srtlaExecutablePath();
    if (executable.isEmpty() || !QFileInfo::exists(executable))
    {
        blog(LOG_ERROR,
             "[Mikhlink] srtla_send.exe was not found next to mikhlink.dll.");
        return false;
    }

    QStringList preferredUplinks;
    QStringList fallbackUplinks;
    try
    {
        const auto adapters = mikhlink::network::getNetworkAdapters();
        for (const auto& adapter : adapters)
        {
            if (!adapter.isUp || adapter.type == "Loopback")
            {
                continue;
            }

            for (const auto& address : adapter.addresses)
            {
                if (address.find('.') == std::string::npos ||
                    address.rfind("127.", 0) == 0 ||
                    address.rfind("169.254.", 0) == 0)
                {
                    continue;
                }

                const QString ip = QString::fromUtf8(address.c_str());
                if (!fallbackUplinks.contains(ip))
                {
                    fallbackUplinks.push_back(ip);
                }
                if (adapter.hasGateway && !preferredUplinks.contains(ip))
                {
                    preferredUplinks.push_back(ip);
                }
            }
        }
    }
    catch (const std::exception& error)
    {
        blog(LOG_ERROR,
             "[Mikhlink] Failed to prepare SRTLA uplinks: %s",
             error.what());
        return false;
    }

    const QStringList uplinks =
        preferredUplinks.isEmpty() ? fallbackUplinks : preferredUplinks;
    if (uplinks.isEmpty())
    {
        blog(LOG_ERROR,
             "[Mikhlink] No active non-loopback IPv4 uplinks found.");
        return false;
    }

    if (preferredUplinks.isEmpty())
    {
        blog(LOG_WARNING,
             "[Mikhlink] Windows reported no default gateways; using all active non-loopback IPv4 addresses.");
    }

    for (const QString& uplink : uplinks)
    {
        blog(LOG_INFO,
             "[Mikhlink] SRTLA uplink selected: %s.",
             uplink.toUtf8().constData());
    }

    const QString uplinksPath =
        QDir(QDir::tempPath()).filePath("mikhlink-srtla-uplinks.txt");
    QFile uplinksFile(uplinksPath);
    if (!uplinksFile.open(
            QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate))
    {
        blog(LOG_ERROR, "[Mikhlink] Failed to write the SRTLA uplink list.");
        return false;
    }

    QTextStream stream(&uplinksFile);
    for (const QString& uplink : uplinks)
    {
        stream << uplink << '\n';
    }
    uplinksFile.close();

    srtlaSender = new QProcess;
    srtlaSender->setProcessChannelMode(QProcess::MergedChannels);
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert("RUST_LOG", "info");
    srtlaSender->setProcessEnvironment(environment);

    QObject::connect(
        srtlaSender,
        &QProcess::readyReadStandardOutput,
        [] {
            if (srtlaSender == nullptr)
            {
                return;
            }
            QString output = QString::fromUtf8(
                srtlaSender->readAllStandardOutput()).trimmed();
            output.remove(QRegularExpression("\\x1B\\[[0-9;]*m"));
            if (!output.isEmpty())
            {
                blog(LOG_INFO,
                     "[Mikhlink/SRTLA] %s",
                     output.toUtf8().constData());
            }
        });

    const QUrl destination(parsed.effectiveUrl);
    const QStringList arguments = {
        QString::number(LocalSrtlaPort),
        destination.host(),
        QString::number(remotePort),
        uplinksPath};

    srtlaSender->start(executable, arguments);
    if (!srtlaSender->waitForStarted(3000))
    {
        blog(LOG_ERROR,
             "[Mikhlink] Failed to start srtla_send.exe: %s",
             srtlaSender->errorString().toUtf8().constData());
        stopSrtlaSender();
        return false;
    }

    blog(LOG_INFO,
         "[Mikhlink] SRTLA sender started: local port %d, remote host %s, remote port %d, uplinks %d.",
         LocalSrtlaPort,
         destination.host().toUtf8().constData(),
         remotePort,
         static_cast<int>(uplinks.size()));
    return true;
}

void frontendEvent(obs_frontend_event event, void*)
{
    if (event == OBS_FRONTEND_EVENT_STREAMING_STARTING)
    {
        startSrtlaSender();
    }
    else if (event == OBS_FRONTEND_EVENT_STREAMING_STOPPED ||
             event == OBS_FRONTEND_EVENT_EXIT)
    {
        stopSrtlaSender();
    }
}


void openMikhlinkSettings(void*)
{
    QWidget* parent =
        static_cast<QWidget*>(obs_frontend_get_main_window());

    if (obs_frontend_streaming_active())
    {
        QMessageBox::warning(
            parent,
            "Mikhlink",
            localized(
                "Stop streaming before changing Mikhlink settings.",
                "Остановите трансляцию перед изменением настроек Mikhlink."));
        return;
    }

    QDialog dialog(parent);
    dialog.setWindowTitle("Mikhlink");
    dialog.setModal(true);

    auto* name = new QLineEdit(&dialog);
    name->setText("BELABOX");
    auto* ingestKey = new QLineEdit(&dialog);
    ingestKey->setEchoMode(QLineEdit::Password);
    auto* srtIngestUrl = new QLineEdit(&dialog);
    auto* port = new QSpinBox(&dialog);
    port->setRange(1, 65535);
    port->setValue(5000);
    auto* useSrtla = new QCheckBox(&dialog);
    useSrtla->setText(localized(
        "Use SRTLA bonding (test)",
        "Использовать SRTLA bonding (тест)"));

    obs_service_t* current = obs_frontend_get_streaming_service();
    if (current != nullptr &&
        std::strcmp(obs_service_get_type(current), ServiceId) == 0)
    {
        obs_data_t* currentSettings =
            obs_service_get_settings(current);

        name->setText(
            QString::fromUtf8(
                obs_data_get_string(currentSettings, NameSetting)));
        if (name->text().trimmed().isEmpty())
        {
            name->setText("BELABOX");
        }
        ingestKey->setText(
            QString::fromUtf8(
                obs_data_get_string(currentSettings, IngestKeySetting)));
        const char* savedUrl =
            obs_data_get_string(currentSettings, SrtIngestUrlSetting);
        if (savedUrl == nullptr || savedUrl[0] == '\0')
        {
            savedUrl =
                obs_data_get_string(currentSettings, LegacyAddressSetting);
        }
        srtIngestUrl->setText(QString::fromUtf8(savedUrl));
        port->setValue(
            static_cast<int>(
                obs_data_get_int(currentSettings, PortSetting)));
        useSrtla->setChecked(
            obs_data_get_bool(currentSettings, UseSrtlaSetting));

        obs_data_release(currentSettings);
    }

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Save | QDialogButtonBox::Cancel,
        &dialog);

    QObject::connect(
        buttons,
        &QDialogButtonBox::accepted,
        &dialog,
        &QDialog::accept);
    QObject::connect(
        buttons,
        &QDialogButtonBox::rejected,
        &dialog,
        &QDialog::reject);

    auto* layout = new QFormLayout(&dialog);
    layout->addRow(localized("Name", "Имя"), name);
    layout->addRow(localized("Ingest key", "Ключ ingest"), ingestKey);
    layout->addRow("SRT ingest URL", srtIngestUrl);
    layout->addRow(localized("SRTLA port", "Порт SRTLA"), port);
    layout->addRow(useSrtla);
    layout->addRow(buttons);

    if (dialog.exec() != QDialog::Accepted)
    {
        return;
    }

    if (name->text().trimmed().isEmpty() ||
        srtIngestUrl->text().trimmed().isEmpty())
    {
        QMessageBox::warning(
            parent,
            "Mikhlink",
            localized(
                "Name and SRT ingest URL are required.",
                "Заполните имя и SRT ingest URL."));
        return;
    }

    const ParsedIngest parsed =
        parseIngest(srtIngestUrl->text(), ingestKey->text());
    if (!parsed.valid)
    {
        QMessageBox::warning(
            parent,
            "Mikhlink",
            parsed.error);
        return;
    }

    const QUrl parsedUrl(parsed.effectiveUrl);
    if (useSrtla->isChecked() &&
        parsedUrl.host().endsWith(".srt.belabox.net", Qt::CaseInsensitive) &&
        port->value() != 5000)
    {
        QMessageBox::warning(
            parent,
            "Mikhlink",
            localized(
                "BELABOX SRTLA uses port 5000. Port 4000 is only for direct SRT ingest.",
                "BELABOX SRTLA использует порт 5000. Порт 4000 предназначен только для прямого SRT ingest."));
        return;
    }

    obs_data_t* settings = obs_data_create();
    const QByteArray nameUtf8 = name->text().trimmed().toUtf8();
    const QByteArray ingestKeyUtf8 = parsed.key.toUtf8();
    const QByteArray srtIngestUrlUtf8 =
        srtIngestUrl->text().trimmed().toUtf8();

    obs_data_set_string(
        settings,
        "service",
        "Mikhlink");
    obs_data_set_string(
        settings,
        NameSetting,
        nameUtf8.constData());
    obs_data_set_string(
        settings,
        IngestKeySetting,
        ingestKeyUtf8.constData());
    obs_data_set_string(
        settings,
        SrtIngestUrlSetting,
        srtIngestUrlUtf8.constData());
    obs_data_set_int(
        settings,
        PortSetting,
        port->value());
    obs_data_set_bool(
        settings,
        UseSrtlaSetting,
        useSrtla->isChecked());

    obs_service_t* service = obs_service_create(
        ServiceId,
        "mikhlink_streaming_service",
        settings,
        nullptr);

    obs_data_release(settings);

    if (service == nullptr)
    {
        QMessageBox::critical(
            parent,
            "Mikhlink",
            localized(
                "Failed to create the Mikhlink streaming service.",
                "Не удалось создать службу трансляции Mikhlink."));
        return;
    }

    obs_frontend_set_streaming_service(service);
    obs_frontend_save_streaming_service();
    obs_service_release(service);

    // Do not keep an idle SRTLA registration alive while OBS is not
    // streaming. It can time out immediately before the first SRT packet.
    // The frontend STREAMING_STARTING event starts the sender just in time;
    // the local SRT connection has a 10-second timeout for registration.
    stopSrtlaSender();

    const QString keySource = parsed.keyWasEmbedded
        ? localized(
              "The ingest key was found inside the SRT URL.",
              "Ключ ingest найден внутри SRT URL.")
        : localized(
              "The ingest key was taken from the separate field.",
              "Ключ ingest взят из отдельного поля.");
    const QString transport = useSrtla->isChecked()
        ? localized(
              "SRTLA test mode is enabled. OBS will send SRT locally and Mikhlink will forward it to the SRTLA relay.",
              "Включён тестовый режим SRTLA. OBS отправит SRT локально, а Mikhlink перенаправит его на SRTLA relay.")
        : localized(
              "Direct SRT test mode is enabled.",
              "Включён режим прямого SRT-теста.");
    const QString information =
        QString::fromUtf8(localized(
            "Mikhlink is now the active OBS streaming service.\n",
            "Mikhlink теперь выбран как служба трансляции OBS.\n")) +
        keySource + "\n" + transport + "\n" +
        QString::fromUtf8(localized(
            "Use the normal Start Streaming button.\n\n"
            "Edit its connection settings only through Service > Mikhlink.",
            "Используйте обычную кнопку «Запустить трансляцию».\n\n"
            "Настройки подключения изменяйте только через Сервис → Mikhlink."));

    QMessageBox::information(parent, "Mikhlink", information);
}

void registerService()
{
    obs_service_info info = {};
    info.id = ServiceId;
    info.get_name = serviceName;
    info.create = createService;
    info.destroy = destroyService;
    info.update = updateService;
    info.get_defaults = serviceDefaults;
    info.get_properties = serviceProperties;
    info.get_url = serviceUrl;
    info.get_key = serviceKey;
    info.get_output_type = serviceOutputType;
    info.get_supported_video_codecs = serviceVideoCodecs;
    info.get_protocol = serviceProtocol;
    info.get_supported_audio_codecs = serviceAudioCodecs;
    info.get_connect_info = serviceConnectInfo;
    info.can_try_to_connect = serviceCanConnect;

    obs_register_service(&info);
}

void registerOutput()
{
    obs_output_info info = {};
    info.id = OutputId;
    info.flags = OBS_OUTPUT_AV | OBS_OUTPUT_ENCODED | OBS_OUTPUT_SERVICE;
    info.get_name = outputName;
    info.create = createOutput;
    info.destroy = destroyOutput;
    info.start = startOutput;
    info.stop = stopOutput;
    info.encoded_packet = receivePacket;
    info.get_total_bytes = outputTotalBytes;
    info.encoded_video_codecs = "h264;hevc";
    info.encoded_audio_codecs = "aac;opus";
    info.protocols = "SRTLA";

    obs_register_output(&info);
}

} // namespace

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

    registerOutput();
    registerService();
    obs_frontend_add_event_callback(frontendEvent, nullptr);
    obs_frontend_add_tools_menu_item(
        "Mikhlink", openMikhlinkSettings, nullptr);

    blog(LOG_INFO,
         "[Mikhlink] Output, streaming service, and settings menu registered.");
    return true;
}

void obs_module_unload(void)
{
    obs_frontend_remove_event_callback(frontendEvent, nullptr);
    stopSrtlaSender();
}
