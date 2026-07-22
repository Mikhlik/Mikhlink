#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QAbstractItemView>
#include <QAbstractSocket>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHash>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QSet>
#include <QSlider>
#include <QSpinBox>
#include <QStringList>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextStream>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>
#include <QWidget>

#include "moblink/MoblinkServer.h"
#include "network/NetworkAdapter.h"

#include <util/bmem.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <new>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

OBS_DECLARE_MODULE()

namespace
{

constexpr const char* OutputId = "mikhlink_output";
constexpr const char* ServiceId = "mikhlink_service";
constexpr const char* TelemetryDockId = "mikhlink_telemetry";
constexpr const char* StatsJsonPrefix = "SRTLA_STATS ";
constexpr const char* NameSetting = "name";
constexpr const char* IngestKeySetting = "ingest_key";
constexpr const char* SrtIngestUrlSetting = "srt_ingest_url";
constexpr const char* LegacyAddressSetting = "address";
constexpr const char* PortSetting = "port";
constexpr const char* UseSrtlaSetting = "use_srtla";
constexpr const char* MoblinkEnabledSetting = "moblink_enabled";
constexpr const char* MoblinkNameSetting = "moblink_name";
constexpr const char* MoblinkPasswordSetting = "moblink_password";
constexpr const char* MoblinkPortSetting = "moblink_port";
constexpr int LocalSrtlaPort = 6000;
constexpr int DefaultMoblinkPort = 7777;

enum TelemetryColumn
{
    ConnectionColumn,
    TypeColumn,
    IpColumn,
    BatteryColumn,
    ThermalColumn,
    ModeColumn,
    PriorityColumn,
    StateColumn,
    BitrateColumn,
    ShareColumn,
    RttColumn,
    JitterColumn,
    TelemetryColumnCount
};

enum class UplinkMode
{
    Bonding,
    Backup,
    Disabled
};

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
    bool moblinkEnabled = false;
    std::string moblinkName;
    std::string moblinkPassword;
    int moblinkPort = DefaultMoblinkPort;
};

struct UplinkPreference
{
    UplinkMode mode = UplinkMode::Bonding;
    int priorityPercent = 50;
};

struct DetectedUplink
{
    QString id;
    QString name;
    QString type;
    QString ip;
    bool hasGateway = false;
};

QProcess* srtlaSender = nullptr;
std::unique_ptr<mikhlink::moblink::Server> moblinkServer;
QWidget* telemetryWidget = nullptr;
QLabel* telemetryStatus = nullptr;
QLabel* telemetrySummary = nullptr;
QLabel* telemetryMoblinkStatus = nullptr;
QTableWidget* telemetryTable = nullptr;
QHash<QString, QString> uplinkNames;
QHash<QString, QString> uplinkTypes;
QHash<QString, QString> uplinkAddresses;
QHash<QString, QString> uplinkIds;
QHash<QString, int> uplinkRows;
QHash<QString, UplinkPreference> uplinkPreferences;
QHash<QString, QComboBox*> uplinkModeControls;
QHash<QString, QSlider*> uplinkPriorityControls;
QHash<QString, QLabel*> uplinkPriorityLabels;
QPushButton* telemetryRefreshButton = nullptr;
QStringList activeSrtlaUplinks;
QString activeSrtlaWeightsPath;
QString lastTelemetryIp;
QString srtlaOutputBuffer;
std::vector<mikhlink::moblink::RelaySnapshot> moblinkRelays;
bool rebuildingTelemetryTable = false;
bool structuredTelemetryActive = false;
bool telemetryJsonErrorReported = false;

bool writeActiveUplinkWeights();
void configureMoblinkServerFromCurrentService();
void synchronizeMoblinkRelays(
    const std::vector<mikhlink::moblink::RelaySnapshot>& relays);

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

void setTelemetryStatus(const char* english, const char* russian)
{
    if (telemetryStatus != nullptr)
    {
        telemetryStatus->setText(
            QString::fromUtf8(localized(english, russian)));
    }
}

QString uplinkPreferencesPath()
{
    char* rawPath = obs_module_config_path("uplinks.ini");
    if (rawPath == nullptr)
    {
        return QDir(QDir::tempPath()).filePath("mikhlink-uplinks.ini");
    }

    const QString path = QString::fromUtf8(rawPath);
    bfree(rawPath);
    QDir().mkpath(QFileInfo(path).absolutePath());
    return path;
}

QString moblinkRelaysPath()
{
    static const QString path = QDir(QDir::tempPath()).filePath(
        "mikhlink-moblink-relays-" +
        QString::number(QCoreApplication::applicationPid()) +
        ".json");
    return path;
}

QString moblinkRelayKey(const mikhlink::moblink::RelaySnapshot& relay)
{
    return "moblink:" + relay.id;
}

QString moblinkThermalText(const QString& thermalState)
{
    return thermalState == "white"
        ? localized("Normal", "Норма")
        : thermalState == "yellow"
              ? localized("Warm", "Нагрев")
              : thermalState == "red"
                    ? localized("Hot", "Перегрев")
                    : "—";
}

bool writeMoblinkRelaysFile()
{
    QJsonArray relayArray;
    for (const auto& relay : moblinkRelays)
    {
        if (!relay.tunnelReady ||
            relay.localAddress.protocol() != QAbstractSocket::IPv4Protocol ||
            relay.peerAddress.protocol() != QAbstractSocket::IPv4Protocol)
        {
            continue;
        }

        relayArray.append(QJsonObject{
            {"id", relay.id},
            {"name", relay.name},
            {"bind_ip", relay.localAddress.toString()},
            {"remote_ip", relay.peerAddress.toString()},
            {"remote_port", static_cast<int>(relay.tunnelPort)}});
    }

    QSaveFile file(moblinkRelaysPath());
    if (!file.open(QIODevice::WriteOnly))
    {
        blog(LOG_ERROR, "[Mikhlink] Failed to write Moblink relay state.");
        return false;
    }
    file.write(QJsonDocument(QJsonObject{
        {"version", 1},
        {"relays", relayArray}}).toJson(QJsonDocument::Compact));
    if (!file.commit())
    {
        blog(LOG_ERROR, "[Mikhlink] Failed to atomically replace Moblink relay state.");
        return false;
    }
    return true;
}

QString uplinkPreferenceKey(const QString& id)
{
    return QString::fromLatin1(QUrl::toPercentEncoding(id));
}

QString uplinkModeValue(UplinkMode mode)
{
    switch (mode)
    {
    case UplinkMode::Backup:
        return "backup";
    case UplinkMode::Disabled:
        return "disabled";
    case UplinkMode::Bonding:
    default:
        return "bonding";
    }
}

UplinkMode uplinkModeFromValue(const QString& value)
{
    if (value == "backup")
    {
        return UplinkMode::Backup;
    }
    if (value == "disabled")
    {
        return UplinkMode::Disabled;
    }
    return UplinkMode::Bonding;
}

UplinkPreference loadUplinkPreference(const QString& id)
{
    QSettings settings(uplinkPreferencesPath(), QSettings::IniFormat);
    settings.beginGroup("uplinks");
    settings.beginGroup(uplinkPreferenceKey(id));

    UplinkPreference preference;
    preference.mode = uplinkModeFromValue(
        settings.value("mode", "bonding").toString());
    if (settings.contains("priority_percent"))
    {
        preference.priorityPercent =
            settings.value("priority_percent", 50).toInt();
    }
    else
    {
        // Builds before percentage sliders stored a value from 1 to 10.
        // Preserve its relative weight when migrating to the 1–100 scale.
        const int legacyPriority =
            std::clamp(settings.value("priority", 5).toInt(), 1, 10);
        preference.priorityPercent = legacyPriority * 10;
    }
    preference.priorityPercent =
        std::clamp(preference.priorityPercent, 1, 100);
    return preference;
}

void saveUplinkPreference(
    const QString& id,
    const UplinkPreference& preference)
{
    QSettings settings(uplinkPreferencesPath(), QSettings::IniFormat);
    settings.beginGroup("uplinks");
    settings.beginGroup(uplinkPreferenceKey(id));
    settings.setValue("mode", uplinkModeValue(preference.mode));
    settings.setValue("priority_percent", preference.priorityPercent);
    settings.remove("priority");
    settings.sync();
}

UplinkPreference uplinkPreference(const QString& id)
{
    const auto existing = uplinkPreferences.constFind(id);
    if (existing != uplinkPreferences.constEnd())
    {
        return existing.value();
    }

    const UplinkPreference preference = loadUplinkPreference(id);
    uplinkPreferences.insert(id, preference);
    return preference;
}

std::vector<DetectedUplink> detectUplinks()
{
    std::vector<DetectedUplink> detected;
    QSet<QString> seenAddresses;

    const auto adapters = mikhlink::network::getNetworkAdapters();
    for (const auto& adapter : adapters)
    {
        if (!adapter.isUp || !adapter.isHardware || adapter.type == "Loopback")
        {
            continue;
        }

        const QString adapterId = QString::fromUtf8(
            (adapter.id.empty() ? adapter.name : adapter.id).c_str());
        for (const auto& address : adapter.addresses)
        {
            if (address.find('.') == std::string::npos ||
                address.rfind("127.", 0) == 0 ||
                address.rfind("169.254.", 0) == 0)
            {
                continue;
            }

            const QString ip = QString::fromUtf8(address.c_str());
            if (seenAddresses.contains(ip))
            {
                continue;
            }
            seenAddresses.insert(ip);
            detected.push_back({
                adapterId,
                QString::fromUtf8(adapter.name.c_str()),
                QString::fromUtf8(adapter.type.c_str()),
                ip,
                adapter.hasGateway});
        }
    }

    return detected;
}

void setTelemetryCell(int row, int column, const QString& value)
{
    if (telemetryTable == nullptr)
    {
        return;
    }

    QTableWidgetItem* item = telemetryTable->item(row, column);
    if (item == nullptr)
    {
        item = new QTableWidgetItem;
        telemetryTable->setItem(row, column, item);
    }
    item->setText(value);
}

QString telemetryKeyForRow(int row)
{
    if (telemetryTable == nullptr || row < 0 ||
        row >= telemetryTable->rowCount())
    {
        return {};
    }
    const QTableWidgetItem* item = telemetryTable->item(row, IpColumn);
    return item != nullptr
        ? item->data(Qt::UserRole).toString()
        : QString();
}

UplinkMode uplinkModeForKey(const QString& key)
{
    const QString id = uplinkIds.value(key);
    return id.isEmpty()
        ? UplinkMode::Bonding
        : uplinkPreference(id).mode;
}

void resetTelemetryTable()
{
    lastTelemetryIp.clear();
    structuredTelemetryActive = false;
    telemetryJsonErrorReported = false;
    if (telemetrySummary != nullptr)
    {
        telemetrySummary->setText("—");
    }

    if (telemetryTable == nullptr)
    {
        return;
    }

    for (int row = 0; row < telemetryTable->rowCount(); ++row)
    {
        const QString key = telemetryKeyForRow(row);
        setTelemetryCell(
            row,
            StateColumn,
            uplinkModeForKey(key) == UplinkMode::Disabled
                ? localized("Disabled", "Отключён")
                : localized("Waiting", "Ожидание"));
        setTelemetryCell(row, BitrateColumn, "0.00");
        setTelemetryCell(row, ShareColumn, "0.0%");
        setTelemetryCell(row, RttColumn, "—");
        setTelemetryCell(row, JitterColumn, "—");
    }
}

void markTelemetryStopped()
{
    setTelemetryStatus("Stopped", "Остановлено");

    if (telemetryTable == nullptr)
    {
        return;
    }

    for (int row = 0; row < telemetryTable->rowCount(); ++row)
    {
        const QString key = telemetryKeyForRow(row);
        setTelemetryCell(
            row,
            StateColumn,
            uplinkModeForKey(key) == UplinkMode::Disabled
                ? localized("Disabled", "Отключён")
                : localized("Stopped", "Остановлено"));
        setTelemetryCell(row, BitrateColumn, "0.00");
        setTelemetryCell(row, ShareColumn, "0.0%");
    }
}

int ensureTelemetryRow(const QString& key)
{
    const auto existing = uplinkRows.constFind(key);
    if (existing != uplinkRows.constEnd())
    {
        return existing.value();
    }

    if (telemetryTable == nullptr)
    {
        return -1;
    }

    const int row = telemetryTable->rowCount();
    telemetryTable->insertRow(row);
    uplinkRows.insert(key, row);
    setTelemetryCell(
        row,
        ConnectionColumn,
        uplinkNames.value(key, localized("Network", "Сеть")));
    setTelemetryCell(row, TypeColumn, uplinkTypes.value(key, "—"));
    setTelemetryCell(row, IpColumn, uplinkAddresses.value(key, key));
    telemetryTable->item(row, IpColumn)->setData(Qt::UserRole, key);
    setTelemetryCell(row, BatteryColumn, "—");
    setTelemetryCell(row, ThermalColumn, "—");

    const QString id = uplinkIds.value(key, key);
    const UplinkPreference preference = uplinkPreference(id);
    auto* mode = new QComboBox(telemetryTable);
    mode->addItem(
        localized("Bonding", "Бондинг"),
        static_cast<int>(UplinkMode::Bonding));
    mode->addItem(
        localized("Backup", "Резерв"),
        static_cast<int>(UplinkMode::Backup));
    mode->addItem(
        localized("Disabled", "Отключён"),
        static_cast<int>(UplinkMode::Disabled));
    mode->setCurrentIndex(mode->findData(static_cast<int>(preference.mode)));
    mode->setToolTip(localized(
        "Bonding carries traffic with the selected priority. Backup stays connected with a minimal share and takes over if bonding links fail.",
        "Бондинг передаёт трафик с выбранным приоритетом. Резерв остаётся подключённым с минимальной долей и принимает поток при отказе основных каналов."));

    auto* priorityContainer = new QWidget(telemetryTable);
    auto* priorityLayout = new QHBoxLayout(priorityContainer);
    priorityLayout->setContentsMargins(4, 0, 4, 0);
    priorityLayout->setSpacing(6);

    auto* priority = new QSlider(Qt::Horizontal, priorityContainer);
    priority->setRange(1, 100);
    priority->setSingleStep(1);
    priority->setPageStep(10);
    priority->setValue(preference.priorityPercent);
    priority->setMinimumWidth(120);
    priority->setToolTip(localized(
        "Relative traffic priority. Actual usage also depends on link quality and capacity.",
        "Относительный приоритет трафика. Фактическая доля также зависит от качества и пропускной способности канала."));

    auto* priorityLabel = new QLabel(
        QString::number(preference.priorityPercent) + "%",
        priorityContainer);
    priorityLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    priorityLabel->setMinimumWidth(42);
    priorityLayout->addWidget(priority, 1);
    priorityLayout->addWidget(priorityLabel);

    auto* prioritySaveTimer = new QTimer(priorityContainer);
    prioritySaveTimer->setSingleShot(true);
    prioritySaveTimer->setInterval(150);

    telemetryTable->setCellWidget(row, ModeColumn, mode);
    telemetryTable->setCellWidget(row, PriorityColumn, priorityContainer);
    uplinkModeControls.insert(key, mode);
    uplinkPriorityControls.insert(key, priority);
    uplinkPriorityLabels.insert(key, priorityLabel);

    QObject::connect(
        mode,
        &QComboBox::currentIndexChanged,
        [key, id, mode, priority](int) {
            if (rebuildingTelemetryTable)
            {
                return;
            }

            UplinkPreference preference = uplinkPreference(id);
            preference.mode = static_cast<UplinkMode>(
                mode->currentData().toInt());
            uplinkPreferences.insert(id, preference);
            saveUplinkPreference(id, preference);
            priority->setEnabled(preference.mode != UplinkMode::Disabled);

            const bool liveUpdate = writeActiveUplinkWeights();

            const int row = uplinkRows.value(key, -1);
            if (row >= 0)
            {
                setTelemetryCell(
                    row,
                    StateColumn,
                    preference.mode == UplinkMode::Disabled
                        ? localized("Disabled", "Отключён")
                        : liveUpdate
                              ? localized("Active", "Активен")
                              : localized("Ready", "Готов"));
            }
        });
    QObject::connect(
        priority,
        &QSlider::valueChanged,
        [priorityLabel, prioritySaveTimer](int value) {
            priorityLabel->setText(QString::number(value) + "%");
            if (rebuildingTelemetryTable)
            {
                return;
            }
            prioritySaveTimer->start();
        });
    QObject::connect(
        prioritySaveTimer,
        &QTimer::timeout,
        [id, priority] {
            UplinkPreference preference = uplinkPreference(id);
            preference.priorityPercent = priority->value();
            uplinkPreferences.insert(id, preference);
            saveUplinkPreference(id, preference);
            writeActiveUplinkWeights();
        });

    mode->setEnabled(true);
    priority->setEnabled(preference.mode != UplinkMode::Disabled);
    setTelemetryCell(
        row,
        StateColumn,
        preference.mode == UplinkMode::Disabled
            ? localized("Disabled", "Отключён")
            : localized("Ready", "Готов"));
    setTelemetryCell(row, BitrateColumn, "0.00");
    setTelemetryCell(row, ShareColumn, "0.0%");
    setTelemetryCell(row, RttColumn, "—");
    setTelemetryCell(row, JitterColumn, "—");
    return row;
}

void setUplinkControlsEnabled(bool allowRefresh)
{
    for (auto it = uplinkModeControls.begin();
         it != uplinkModeControls.end();
         ++it)
    {
        it.value()->setEnabled(true);
    }
    for (auto it = uplinkPriorityControls.begin();
         it != uplinkPriorityControls.end();
         ++it)
    {
        it.value()->setEnabled(
            uplinkModeForKey(it.key()) != UplinkMode::Disabled);
    }
    if (telemetryRefreshButton != nullptr)
    {
        telemetryRefreshButton->setEnabled(allowRefresh);
    }
}

void populateTelemetryTable(const std::vector<DetectedUplink>& detected)
{
    if (telemetryTable == nullptr)
    {
        return;
    }

    rebuildingTelemetryTable = true;
    telemetryTable->setRowCount(0);
    uplinkRows.clear();
    uplinkNames.clear();
    uplinkTypes.clear();
    uplinkAddresses.clear();
    uplinkIds.clear();
    uplinkModeControls.clear();
    uplinkPriorityControls.clear();
    uplinkPriorityLabels.clear();

    for (const auto& uplink : detected)
    {
        uplinkNames.insert(uplink.ip, uplink.name);
        uplinkTypes.insert(uplink.ip, uplink.type);
        uplinkAddresses.insert(uplink.ip, uplink.ip);
        uplinkIds.insert(uplink.ip, uplink.id);
        ensureTelemetryRow(uplink.ip);
    }

    for (const auto& relay : moblinkRelays)
    {
        const QString key = "moblink:" + relay.id;
        uplinkNames.insert(key, relay.name);
        uplinkTypes.insert(key, "Moblink");
        uplinkAddresses.insert(
            key,
            relay.tunnelReady
                ? relay.peerAddress.toString() + ":" +
                      QString::number(relay.tunnelPort)
                : relay.peerAddress.toString());
        uplinkIds.insert(key, key);
        const int row = ensureTelemetryRow(key);
        setTelemetryCell(
            row,
            BatteryColumn,
            relay.batteryPercentage >= 0
                ? QString::number(relay.batteryPercentage) + "%"
                : "—");
        setTelemetryCell(
            row,
            ThermalColumn,
            relay.thermalState == "white"
                ? localized("Normal", "Норма")
                : relay.thermalState == "yellow"
                      ? localized("Warm", "Нагрев")
                      : relay.thermalState == "red"
                            ? localized("Hot", "Перегрев")
                            : "—");
        setTelemetryCell(
            row,
            StateColumn,
            uplinkModeForKey(key) == UplinkMode::Disabled
                ? localized("Disabled", "Отключён")
                : relay.tunnelReady
                      ? localized("Tunnel ready", "Туннель готов")
                      : localized("Authenticated", "Авторизован"));
    }
    rebuildingTelemetryTable = false;
    setUplinkControlsEnabled(!obs_frontend_streaming_active());
}

void refreshTelemetryTable()
{
    try
    {
        const auto detected = detectUplinks();
        populateTelemetryTable(detected);
        if (detected.empty() && moblinkRelays.empty())
        {
            setTelemetryStatus("No active uplinks", "Нет активных каналов");
        }
        else if (!obs_frontend_streaming_active())
        {
            setTelemetryStatus("Ready", "Готово");
        }
    }
    catch (const std::exception& error)
    {
        setTelemetryStatus(
            "Adapter detection failed",
            "Ошибка поиска адаптеров");
        blog(LOG_ERROR,
             "[Mikhlink] Failed to refresh uplinks: %s",
             error.what());
    }
}

void synchronizeMoblinkRelays(
    const std::vector<mikhlink::moblink::RelaySnapshot>& relays)
{
    bool structureChanged = relays.size() != moblinkRelays.size();
    if (!structureChanged)
    {
        for (const auto& relay : relays)
        {
            const auto existing = std::find_if(
                moblinkRelays.cbegin(),
                moblinkRelays.cend(),
                [&relay](const mikhlink::moblink::RelaySnapshot& candidate) {
                    return candidate.id == relay.id;
                });
            if (existing == moblinkRelays.cend() ||
                existing->name != relay.name ||
                existing->peerAddress != relay.peerAddress ||
                existing->localAddress != relay.localAddress ||
                existing->tunnelPort != relay.tunnelPort ||
                existing->tunnelReady != relay.tunnelReady)
            {
                structureChanged = true;
                break;
            }
        }
    }

    moblinkRelays = relays;
    writeMoblinkRelaysFile();

    if (srtlaSender != nullptr &&
        srtlaSender->state() != QProcess::NotRunning)
    {
        activeSrtlaUplinks.erase(
            std::remove_if(
                activeSrtlaUplinks.begin(),
                activeSrtlaUplinks.end(),
                [](const QString& key) {
                    return key.startsWith("moblink:");
                }),
            activeSrtlaUplinks.end());
        for (const auto& relay : moblinkRelays)
        {
            if (relay.tunnelReady)
            {
                activeSrtlaUplinks.push_back(moblinkRelayKey(relay));
            }
        }
        activeSrtlaUplinks.removeDuplicates();
        writeActiveUplinkWeights();
    }

    if (telemetryMoblinkStatus != nullptr)
    {
        const int ready = static_cast<int>(std::count_if(
            moblinkRelays.cbegin(),
            moblinkRelays.cend(),
            [](const mikhlink::moblink::RelaySnapshot& relay) {
                return relay.tunnelReady;
            }));
        telemetryMoblinkStatus->setText(
            QString::fromUtf8(localized(
                "Moblink phones: ", "Телефоны Moblink: ")) +
            QString::number(static_cast<int>(moblinkRelays.size())) +
            QString::fromUtf8(localized(
                ", tunnels ready: ", ", туннелей готово: ")) +
            QString::number(ready));
    }

    if (structureChanged)
    {
        try
        {
            populateTelemetryTable(detectUplinks());
        }
        catch (const std::exception& error)
        {
            blog(LOG_ERROR,
                 "[Mikhlink] Failed to rebuild telemetry for Moblink: %s",
                 error.what());
        }
        return;
    }

    for (const auto& relay : moblinkRelays)
    {
        const QString key = moblinkRelayKey(relay);
        const int row = uplinkRows.value(key, -1);
        if (row < 0)
        {
            continue;
        }
        setTelemetryCell(
            row,
            BatteryColumn,
            relay.batteryPercentage >= 0
                ? QString::number(relay.batteryPercentage) + "%"
                : "—");
        setTelemetryCell(row, ThermalColumn, moblinkThermalText(relay.thermalState));
        if (srtlaSender == nullptr ||
            srtlaSender->state() == QProcess::NotRunning)
        {
            setTelemetryCell(
                row,
                StateColumn,
                uplinkModeForKey(key) == UplinkMode::Disabled
                    ? localized("Disabled", "Отключён")
                    : relay.tunnelReady
                          ? localized("Tunnel ready", "Туннель готов")
                          : localized("Authenticated", "Авторизован"));
        }
    }
}

void recalculateTelemetryShares()
{
    if (telemetryTable == nullptr)
    {
        return;
    }

    double total = 0.0;
    for (int row = 0; row < telemetryTable->rowCount(); ++row)
    {
        const QTableWidgetItem* item =
            telemetryTable->item(row, BitrateColumn);
        total += item != nullptr ? item->text().toDouble() : 0.0;
    }

    for (int row = 0; row < telemetryTable->rowCount(); ++row)
    {
        const QTableWidgetItem* item =
            telemetryTable->item(row, BitrateColumn);
        const double bitrate = item != nullptr ? item->text().toDouble() : 0.0;
        const double share = total > 0.0 ? bitrate * 100.0 / total : 0.0;
        setTelemetryCell(
            row,
            ShareColumn,
            QString::number(share, 'f', 1) + "%");
    }
}

bool processTelemetrySnapshotLine(const QString& line)
{
    const QString prefix = QString::fromLatin1(StatsJsonPrefix);
    const int prefixPosition = line.indexOf(prefix);
    if (prefixPosition < 0)
    {
        return false;
    }

    const QByteArray payload = line.mid(
        prefixPosition + prefix.size()).toUtf8();
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(
        payload, &parseError);
    if (parseError.error != QJsonParseError::NoError ||
        !document.isObject())
    {
        if (!telemetryJsonErrorReported)
        {
            blog(LOG_WARNING,
                 "[Mikhlink] Invalid structured SRTLA telemetry: %s.",
                 parseError.errorString().toUtf8().constData());
            telemetryJsonErrorReported = true;
        }
        return true;
    }

    telemetryJsonErrorReported = false;
    structuredTelemetryActive = true;
    lastTelemetryIp.clear();

    const QJsonObject snapshot = document.object();
    const QJsonArray links = snapshot.value("links").toArray();
    QSet<QString> reportedKeys;
    double totalMbps = 0.0;

    for (const QJsonValue& value : links)
    {
        if (!value.isObject())
        {
            continue;
        }

        const QJsonObject link = value.toObject();
        const QString ip = link.value("ip").toString();
        const QString key = link.value("id").toString(ip);
        if (key.isEmpty())
        {
            continue;
        }

        const QString kind = link.value("kind").toString("network");
        const QString name = link.value("name").toString();
        if (!name.isEmpty())
        {
            uplinkNames.insert(key, name);
        }
        if (!kind.isEmpty())
        {
            uplinkTypes.insert(
                key,
                kind == "moblink" ? "Moblink" : uplinkTypes.value(key, "Network"));
        }
        if (!uplinkAddresses.contains(key))
        {
            uplinkAddresses.insert(key, ip);
        }
        if (!uplinkIds.contains(key))
        {
            uplinkIds.insert(key, key);
        }

        reportedKeys.insert(key);
        const int row = ensureTelemetryRow(key);
        if (row < 0)
        {
            continue;
        }

        const bool disabled =
            uplinkModeForKey(key) == UplinkMode::Disabled;
        const bool connected = link.value("connected").toBool();
        const bool timedOut = link.value("timed_out").toBool();
        const double bitrateMbps = disabled
            ? 0.0
            : link.value("bitrate_bytes_per_sec").toDouble() *
                  8.0 / 1'000'000.0;
        totalMbps += bitrateMbps;

        setTelemetryCell(
            row,
            StateColumn,
            disabled
                ? localized("Disabled", "Отключён")
                : timedOut
                      ? localized("Recovering", "Восстановление")
                      : connected
                            ? localized("Active", "Активен")
                            : localized("Connecting", "Подключение"));
        setTelemetryCell(
            row,
            BitrateColumn,
            QString::number(bitrateMbps, 'f', 2));

        const double rttMs = link.value("rtt_ms").toDouble();
        const double jitterMs = link.value("rtt_jitter_ms").toDouble();
        setTelemetryCell(
            row,
            RttColumn,
            rttMs > 0.0
                ? QString::number(rttMs, 'f', 1) + " ms"
                : "—");
        setTelemetryCell(
            row,
            JitterColumn,
            jitterMs > 0.0
                ? QString::number(jitterMs, 'f', 1) + " ms"
                : "—");
    }

    for (const QString& key : activeSrtlaUplinks)
    {
        if (reportedKeys.contains(key))
        {
            continue;
        }

        const int row = ensureTelemetryRow(key);
        if (row < 0)
        {
            continue;
        }
        setTelemetryCell(
            row,
            StateColumn,
            uplinkModeForKey(key) == UplinkMode::Disabled
                ? localized("Disabled", "Отключён")
                : localized("Connecting", "Подключение"));
        setTelemetryCell(row, BitrateColumn, "0.00");
        setTelemetryCell(row, RttColumn, "—");
        setTelemetryCell(row, JitterColumn, "—");
    }

    recalculateTelemetryShares();

    if (telemetrySummary != nullptr)
    {
        telemetrySummary->setText(
            QString::fromUtf8(localized("Total: ", "Всего: ")) +
            QString::number(totalMbps, 'f', 2) +
            QString::fromUtf8(localized(" Mbps", " Мбит/с")));
    }
    if (telemetryStatus != nullptr)
    {
        telemetryStatus->setText(
            QString::fromUtf8(localized(
                "Active channels: ", "Активных каналов: ")) +
            QString::number(snapshot.value("active_links").toInt()) +
            "/" +
            QString::number(snapshot.value("total_links").toInt()));
    }

    return true;
}

void processTelemetryLine(const QString& line)
{
    if (telemetryTable == nullptr)
    {
        return;
    }

    if (structuredTelemetryActive)
    {
        return;
    }

    static const QRegularExpression connectionExpression(
        R"(\[\d+\]\s+(ACTIVE|TIMED OUT|RECOVERING).* via ([0-9.]+).*Bitrate: ([0-9.]+) Mbps)");
    static const QRegularExpression rttExpression(
        R"(RTT: kalman=([0-9.]+)ms.*jitter=([0-9.]+)ms.*stable=(true|false))");
    static const QRegularExpression totalExpression(
        R"(Total bitrate: ([0-9.]+) Mbps)");
    static const QRegularExpression activeExpression(
        R"(Active connections: (\d+))");
    static const QRegularExpression establishedExpression(
        R"(connection established \(active=(\d+)\))");
    static const QRegularExpression recoveringExpression(
        R"(via ([0-9.]+).*(timed out|Reconnect attempt|marked for recovery))",
        QRegularExpression::CaseInsensitiveOption);

    QRegularExpressionMatch match = connectionExpression.match(line);
    if (match.hasMatch())
    {
        const QString state = match.captured(1);
        const QString ip = match.captured(2);
        const int row = ensureTelemetryRow(ip);
        if (row >= 0)
        {
            const bool disabled =
                uplinkModeForKey(ip) == UplinkMode::Disabled;
            setTelemetryCell(
                row,
                StateColumn,
                disabled
                    ? localized("Disabled", "Отключён")
                    : state == "ACTIVE"
                          ? localized("Active", "Активен")
                          : localized("Recovering", "Восстановление"));
            setTelemetryCell(
                row,
                BitrateColumn,
                disabled ? "0.00" : match.captured(3));
            lastTelemetryIp = ip;
            recalculateTelemetryShares();
        }
        return;
    }

    match = rttExpression.match(line);
    if (match.hasMatch() && !lastTelemetryIp.isEmpty())
    {
        const int row = ensureTelemetryRow(lastTelemetryIp);
        if (row >= 0)
        {
            setTelemetryCell(row, RttColumn, match.captured(1) + " ms");
            setTelemetryCell(row, JitterColumn, match.captured(2) + " ms");
        }
        return;
    }

    match = recoveringExpression.match(line);
    if (match.hasMatch())
    {
        const int row = ensureTelemetryRow(match.captured(1));
        if (row >= 0)
        {
            const bool disabled =
                uplinkModeForKey(match.captured(1)) == UplinkMode::Disabled;
            setTelemetryCell(
                row,
                StateColumn,
                disabled
                    ? localized("Disabled", "Отключён")
                    : localized("Recovering", "Восстановление"));
            setTelemetryCell(row, BitrateColumn, "0.00");
            recalculateTelemetryShares();
        }
        setTelemetryStatus(
            "Reconnecting channels…",
            "Переподключение каналов…");
        return;
    }

    match = totalExpression.match(line);
    if (match.hasMatch() && telemetrySummary != nullptr)
    {
        telemetrySummary->setText(
            QString::fromUtf8(localized("Total: ", "Всего: ")) +
            match.captured(1) +
            QString::fromUtf8(localized(" Mbps", " Мбит/с")));
        return;
    }

    match = establishedExpression.match(line);
    if (match.hasMatch() && telemetryStatus != nullptr)
    {
        telemetryStatus->setText(
            QString::fromUtf8(localized("Connected channels: ", "Подключено каналов: ")) +
            match.captured(1));
        return;
    }

    match = activeExpression.match(line);
    if (match.hasMatch() && telemetryStatus != nullptr)
    {
        telemetryStatus->setText(
            QString::fromUtf8(localized("Active channels: ", "Активных каналов: ")) +
            match.captured(1));
    }
}

void createTelemetryDock()
{
    if (telemetryWidget != nullptr)
    {
        return;
    }

    telemetryWidget = new QWidget;
    telemetryStatus = new QLabel(
        localized("Stopped", "Остановлено"), telemetryWidget);
    telemetrySummary = new QLabel("—", telemetryWidget);
    telemetryMoblinkStatus = new QLabel(
        localized("Moblink: disabled", "Moblink: выключен"),
        telemetryWidget);
    telemetryRefreshButton = new QPushButton(
        localized("Refresh", "Обновить"), telemetryWidget);
    telemetryTable = new QTableWidget(
        0, TelemetryColumnCount, telemetryWidget);
    telemetryTable->setHorizontalHeaderLabels(QStringList{
        localized("Connection", "Соединение"),
        localized("Type", "Тип"),
        localized("IP / endpoint", "IP / endpoint"),
        localized("Battery", "Батарея"),
        localized("Heat", "Нагрев"),
        localized("Mode", "Режим"),
        localized("Priority", "Приоритет"),
        localized("State", "Состояние"),
        localized("Mbps", "Мбит/с"),
        "%",
        "RTT",
        localized("Jitter", "Джиттер")});
    telemetryTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    telemetryTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    telemetryTable->setAlternatingRowColors(true);
    telemetryTable->verticalHeader()->setVisible(false);
    telemetryTable->horizontalHeader()->setSectionResizeMode(
        QHeaderView::ResizeToContents);
    telemetryTable->horizontalHeader()->setSectionResizeMode(
        ConnectionColumn, QHeaderView::Stretch);
    telemetryTable->horizontalHeader()->setSectionResizeMode(
        PriorityColumn, QHeaderView::Fixed);
    telemetryTable->setColumnWidth(PriorityColumn, 210);

    QObject::connect(
        telemetryRefreshButton,
        &QPushButton::clicked,
        [] { refreshTelemetryTable(); });

    auto* statusLayout = new QHBoxLayout;
    statusLayout->addWidget(telemetryStatus);
    statusLayout->addStretch();
    statusLayout->addWidget(telemetrySummary);
    statusLayout->addWidget(telemetryRefreshButton);

    auto* layout = new QVBoxLayout(telemetryWidget);
    layout->addLayout(statusLayout);
    layout->addWidget(telemetryMoblinkStatus);
    layout->addWidget(telemetryTable);

    if (!obs_frontend_add_dock_by_id(TelemetryDockId, "Mikhlink", telemetryWidget))
    {
        blog(LOG_ERROR, "[Mikhlink] Failed to register telemetry dock.");
        delete telemetryWidget;
        telemetryWidget = nullptr;
        telemetryStatus = nullptr;
        telemetrySummary = nullptr;
        telemetryMoblinkStatus = nullptr;
        telemetryTable = nullptr;
        telemetryRefreshButton = nullptr;
        return;
    }

    refreshTelemetryTable();
}

void removeTelemetryDock()
{
    if (telemetryWidget == nullptr)
    {
        return;
    }

    obs_frontend_remove_dock(TelemetryDockId);
    telemetryWidget = nullptr;
    telemetryStatus = nullptr;
    telemetrySummary = nullptr;
    telemetryMoblinkStatus = nullptr;
    telemetryTable = nullptr;
    telemetryRefreshButton = nullptr;
    uplinkRows.clear();
    uplinkNames.clear();
    uplinkTypes.clear();
    uplinkAddresses.clear();
    uplinkIds.clear();
    uplinkModeControls.clear();
    uplinkPriorityControls.clear();
    uplinkPriorityLabels.clear();
    lastTelemetryIp.clear();
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

void configureMoblinkServerFromCurrentService()
{
    if (moblinkServer == nullptr)
    {
        return;
    }

    mikhlink::moblink::ServerConfig config;
    obs_service_t* current = obs_frontend_get_streaming_service();
    if (current != nullptr &&
        std::strcmp(obs_service_get_type(current), ServiceId) == 0)
    {
        obs_data_t* settings = obs_service_get_settings(current);
        config.enabled = obs_data_get_bool(settings, MoblinkEnabledSetting);
        config.name = QString::fromUtf8(
            obs_data_get_string(settings, MoblinkNameSetting));
        config.password = QString::fromUtf8(
            obs_data_get_string(settings, MoblinkPasswordSetting));
        const int port = static_cast<int>(
            obs_data_get_int(settings, MoblinkPortSetting));
        config.port = static_cast<std::uint16_t>(
            std::clamp(port, 1, 65535));

        const bool useSrtla = obs_data_get_bool(settings, UseSrtlaSetting);
        const ParsedIngest parsed = parseIngest(
            QString::fromUtf8(
                obs_data_get_string(settings, SrtIngestUrlSetting)),
            QString::fromUtf8(
                obs_data_get_string(settings, IngestKeySetting)));
        const int destinationPort = static_cast<int>(
            obs_data_get_int(settings, PortSetting));
        if (useSrtla && parsed.valid &&
            destinationPort >= 1 && destinationPort <= 65535)
        {
            config.destinationHost = QUrl(parsed.effectiveUrl).host();
            config.destinationPort = static_cast<std::uint16_t>(destinationPort);
        }
        obs_data_release(settings);
    }

    const bool listening = moblinkServer->configure(config);
    if (telemetryMoblinkStatus != nullptr)
    {
        if (!config.enabled)
        {
            telemetryMoblinkStatus->setText(
                localized("Moblink: disabled", "Moblink: выключен"));
        }
        else if (!listening)
        {
            telemetryMoblinkStatus->setText(
                QString::fromUtf8(localized(
                    "Moblink error: ", "Ошибка Moblink: ")) +
                moblinkServer->lastError());
        }
        else
        {
            telemetryMoblinkStatus->setText(
                QString::fromUtf8(localized(
                    "Moblink: listening on port ",
                    "Moblink: ожидание телефонов на порту ")) +
                QString::number(config.port));
        }
    }
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
    service->moblinkEnabled = obs_data_get_bool(settings, MoblinkEnabledSetting);
    service->moblinkName = obs_data_get_string(settings, MoblinkNameSetting);
    service->moblinkPassword = obs_data_get_string(settings, MoblinkPasswordSetting);
    service->moblinkPort =
        static_cast<int>(obs_data_get_int(settings, MoblinkPortSetting));
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
    QTimer::singleShot(0, [] { configureMoblinkServerFromCurrentService(); });
}

void serviceDefaults(obs_data_t* settings)
{
    obs_data_set_default_string(settings, "service", "Mikhlink");
    obs_data_set_default_string(settings, NameSetting, "BELABOX");
    obs_data_set_default_string(settings, IngestKeySetting, "");
    obs_data_set_default_string(settings, SrtIngestUrlSetting, "");
    obs_data_set_default_int(settings, PortSetting, 5000);
    obs_data_set_default_bool(settings, UseSrtlaSetting, false);
    obs_data_set_default_bool(settings, MoblinkEnabledSetting, false);
    obs_data_set_default_string(settings, MoblinkNameSetting, "Mikhlink OBS");
    obs_data_set_default_string(settings, MoblinkPasswordSetting, "1234");
    obs_data_set_default_int(settings, MoblinkPortSetting, DefaultMoblinkPort);
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
        localized("Use SRTLA bonding", "Использовать SRTLA bonding"));

    obs_properties_add_bool(
        properties,
        MoblinkEnabledSetting,
        localized("Accept Moblink phones", "Подключать телефоны Moblink"));
    obs_properties_add_text(
        properties,
        MoblinkNameSetting,
        localized("Moblink streamer name", "Имя сервера Moblink"),
        OBS_TEXT_DEFAULT);
    obs_properties_add_text(
        properties,
        MoblinkPasswordSetting,
        localized("Moblink password", "Пароль Moblink"),
        OBS_TEXT_PASSWORD);
    obs_properties_add_int(
        properties,
        MoblinkPortSetting,
        localized("Moblink WebSocket port", "WebSocket-порт Moblink"),
        1,
        65535,
        1);

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

double uplinkSchedulingWeight(const QString& ip, bool hasBondingUplink)
{
    const QString id = uplinkIds.value(ip, ip);
    const UplinkPreference preference = uplinkPreference(id);
    if (preference.mode == UplinkMode::Disabled)
    {
        return 0.0;
    }

    // Preserve the old 1–10 weighting ratios after migrating the UI to
    // 1–100%. A backup stays 100 times lighter while a bonding link exists,
    // then automatically returns to its normal weight if all bonding links
    // are disabled.
    return preference.mode == UplinkMode::Backup && hasBondingUplink
        ? static_cast<double>(preference.priorityPercent) / 5000.0
        : static_cast<double>(preference.priorityPercent) / 50.0;
}

bool writeUplinkWeightsFile(
    const QString& path,
    const QStringList& uplinks,
    bool logUpdate)
{
    if (path.isEmpty() || uplinks.isEmpty())
    {
        return false;
    }

    const bool hasBondingUplink = std::any_of(
        uplinks.cbegin(),
        uplinks.cend(),
        [](const QString& ip) {
            const QString id = uplinkIds.value(ip, ip);
            return uplinkPreference(id).mode == UplinkMode::Bonding;
        });

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        blog(LOG_ERROR, "[Mikhlink] Failed to write SRTLA uplink weights.");
        return false;
    }

    QTextStream stream(&file);
    for (const QString& ip : uplinks)
    {
        const QString id = uplinkIds.value(ip, ip);
        const UplinkPreference preference = uplinkPreference(id);
        const double weight = uplinkSchedulingWeight(ip, hasBondingUplink);
        stream << ip << ' ' << QString::number(weight, 'f', 4) << '\n';

        if (logUpdate)
        {
            const QByteArray mode =
                uplinkModeValue(preference.mode).toUtf8();
            blog(LOG_INFO,
                 "[Mikhlink] SRTLA uplink priority updated: %s, mode=%s, priority=%d%%, weight=%.4f.",
                 ip.toUtf8().constData(),
                 mode.constData(),
                 preference.priorityPercent,
                 weight);
        }
    }
    stream.flush();

    if (!file.commit())
    {
        blog(LOG_ERROR,
             "[Mikhlink] Failed to atomically replace SRTLA uplink weights.");
        return false;
    }
    return true;
}

bool writeActiveUplinkWeights()
{
    if (srtlaSender == nullptr ||
        srtlaSender->state() == QProcess::NotRunning ||
        activeSrtlaWeightsPath.isEmpty() ||
        activeSrtlaUplinks.isEmpty())
    {
        return false;
    }

    return writeUplinkWeightsFile(
        activeSrtlaWeightsPath,
        activeSrtlaUplinks,
        true);
}

void stopSrtlaSender()
{
    if (srtlaSender == nullptr)
    {
        activeSrtlaUplinks.clear();
        activeSrtlaWeightsPath.clear();
        markTelemetryStopped();
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
    activeSrtlaUplinks.clear();
    activeSrtlaWeightsPath.clear();
    srtlaOutputBuffer.clear();
    markTelemetryStopped();
    blog(LOG_INFO, "[Mikhlink] SRTLA sender stopped.");
}

bool startSrtlaSender()
{
    configureMoblinkServerFromCurrentService();
    writeMoblinkRelaysFile();

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
        resetTelemetryTable();
        setTelemetryStatus(
            "Direct SRT (bonding disabled)",
            "Прямой SRT (бондинг выключен)");
        return true;
    }

    if (srtlaSender != nullptr &&
        srtlaSender->state() != QProcess::NotRunning)
    {
        obs_data_release(settings);
        blog(LOG_INFO, "[Mikhlink] SRTLA sender is already running.");
        return true;
    }

    resetTelemetryTable();
    setTelemetryStatus(
        "Preparing SRTLA channels…",
        "Подготовка каналов SRTLA…");

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
        setTelemetryStatus("Invalid SRTLA settings", "Ошибка настроек SRTLA");
        blog(LOG_ERROR, "[Mikhlink] Invalid SRTLA destination settings.");
        return false;
    }

    const QUrl remoteDestination(parsed.effectiveUrl);
    if (remoteDestination.host().endsWith(
            ".srt.belabox.net", Qt::CaseInsensitive) &&
        remotePort != 5000)
    {
        setTelemetryStatus("Invalid BELABOX port", "Неверный порт BELABOX");
        blog(LOG_ERROR,
             "[Mikhlink] BELABOX SRTLA requires remote port 5000; configured port is %d.",
             remotePort);
        return false;
    }

    stopSrtlaSender();

    const QString executable = srtlaExecutablePath();
    if (executable.isEmpty() || !QFileInfo::exists(executable))
    {
        setTelemetryStatus(
            "srtla_send.exe was not found",
            "Не найден srtla_send.exe");
        blog(LOG_ERROR,
             "[Mikhlink] srtla_send.exe was not found next to mikhlink.dll.");
        return false;
    }

    std::vector<DetectedUplink> detected;
    try
    {
        detected = detectUplinks();
        populateTelemetryTable(detected);
    }
    catch (const std::exception& error)
    {
        setTelemetryStatus(
            "Adapter detection failed",
            "Ошибка поиска адаптеров");
        blog(LOG_ERROR,
             "[Mikhlink] Failed to prepare SRTLA uplinks: %s",
             error.what());
        return false;
    }

    const bool hasGateway = std::any_of(
        detected.begin(),
        detected.end(),
        [](const DetectedUplink& uplink) {
            return uplink.hasGateway;
        });

    struct SelectedUplink
    {
        DetectedUplink detected;
        UplinkPreference preference;
    };

    std::vector<SelectedUplink> selected;
    for (const auto& uplink : detected)
    {
        const UplinkPreference preference = uplinkPreference(uplink.id);
        const int row = uplinkRows.value(uplink.ip, -1);
        if (hasGateway && !uplink.hasGateway)
        {
            if (row >= 0)
            {
                setTelemetryCell(
                    row,
                    StateColumn,
                    localized("No default gateway", "Нет шлюза"));
            }
            continue;
        }

        selected.push_back({uplink, preference});
    }

    const bool hasReadyMoblink = std::any_of(
        moblinkRelays.cbegin(),
        moblinkRelays.cend(),
        [](const mikhlink::moblink::RelaySnapshot& relay) {
            return relay.tunnelReady;
        });

    if (selected.empty() && !hasReadyMoblink)
    {
        setTelemetryStatus("No active uplinks", "Нет активных каналов");
        blog(LOG_ERROR,
             "[Mikhlink] No active physical or Moblink uplinks found.");
        return false;
    }

    const bool hasEnabledPhysicalUplink = std::any_of(
        selected.begin(),
        selected.end(),
        [](const SelectedUplink& uplink) {
            return uplink.preference.mode != UplinkMode::Disabled;
        });
    const bool hasEnabledMoblinkUplink = std::any_of(
        moblinkRelays.cbegin(),
        moblinkRelays.cend(),
        [](const mikhlink::moblink::RelaySnapshot& relay) {
            return relay.tunnelReady &&
                uplinkPreference(moblinkRelayKey(relay)).mode !=
                    UplinkMode::Disabled;
        });
    const bool hasEnabledUplink =
        hasEnabledPhysicalUplink || hasEnabledMoblinkUplink;
    if (!hasEnabledUplink)
    {
        setTelemetryStatus("All uplinks are disabled", "Все каналы отключены");
        blog(LOG_ERROR, "[Mikhlink] All selected SRTLA uplinks are disabled.");
        return false;
    }

    if (!selected.empty() && !hasGateway)
    {
        blog(LOG_WARNING,
             "[Mikhlink] Windows reported no default gateways; using all active non-loopback IPv4 addresses.");
    }

    QStringList uplinks;
    for (const auto& uplink : selected)
    {
        uplinks.push_back(uplink.detected.ip);

        const int row = ensureTelemetryRow(uplink.detected.ip);
        if (row >= 0)
        {
            setTelemetryCell(
                row,
                StateColumn,
                uplink.preference.mode == UplinkMode::Disabled
                    ? localized("Disabled", "Отключён")
                    : uplink.preference.mode == UplinkMode::Backup
                          ? localized("Backup ready", "Резерв готов")
                          : localized("Bonding ready", "Бондинг готов"));
        }
    }

    const QString uplinksPath =
        QDir(QDir::tempPath()).filePath("mikhlink-srtla-uplinks.txt");
    QSaveFile uplinksFile(uplinksPath);
    if (!uplinksFile.open(
            QIODevice::WriteOnly | QIODevice::Text))
    {
        setTelemetryStatus(
            "Cannot write the uplink list",
            "Не удалось записать список каналов");
        blog(LOG_ERROR, "[Mikhlink] Failed to write the SRTLA uplink list.");
        return false;
    }

    QTextStream stream(&uplinksFile);
    for (const QString& uplink : uplinks)
    {
        stream << uplink << '\n';
    }
    stream.flush();
    if (!uplinksFile.commit())
    {
        setTelemetryStatus(
            "Cannot save the uplink list",
            "Не удалось сохранить список каналов");
        blog(LOG_ERROR,
             "[Mikhlink] Failed to atomically replace the SRTLA uplink list.");
        return false;
    }

    const QString weightsPath =
        QDir(QDir::tempPath()).filePath("mikhlink-srtla-weights.txt");
    activeSrtlaUplinks = uplinks;
    for (const auto& relay : moblinkRelays)
    {
        if (relay.tunnelReady)
        {
            activeSrtlaUplinks.push_back(moblinkRelayKey(relay));
        }
    }
    activeSrtlaWeightsPath = weightsPath;
    if (!writeUplinkWeightsFile(
            weightsPath, activeSrtlaUplinks, true))
    {
        setTelemetryStatus(
            "Cannot write uplink weights",
            "Не удалось записать веса каналов");
        activeSrtlaUplinks.clear();
        activeSrtlaWeightsPath.clear();
        return false;
    }

    setUplinkControlsEnabled(false);

    srtlaSender = new QProcess;
    srtlaOutputBuffer.clear();
    structuredTelemetryActive = false;
    telemetryJsonErrorReported = false;
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

            srtlaOutputBuffer += QString::fromUtf8(
                srtlaSender->readAllStandardOutput());
            srtlaOutputBuffer.remove(
                QRegularExpression("\\x1B\\[[0-9;]*m"));

            int newline = -1;
            while ((newline = srtlaOutputBuffer.indexOf('\n')) >= 0)
            {
                const QString line =
                    srtlaOutputBuffer.left(newline).trimmed();
                srtlaOutputBuffer.remove(0, newline + 1);
                if (line.isEmpty())
                {
                    continue;
                }

                if (!processTelemetrySnapshotLine(line))
                {
                    blog(LOG_INFO,
                         "[Mikhlink/SRTLA] %s",
                         line.toUtf8().constData());
                    processTelemetryLine(line);
                }
            }
        });

    const QUrl destination(parsed.effectiveUrl);
    const QStringList arguments = {
        "--stats-json-lines",
        "--weights-file",
        weightsPath,
        "--moblink-relays-file",
        moblinkRelaysPath(),
        QString::number(LocalSrtlaPort),
        destination.host(),
        QString::number(remotePort),
        uplinksPath};

    srtlaSender->start(executable, arguments);
    if (!srtlaSender->waitForStarted(3000))
    {
        setTelemetryStatus(
            "Failed to start SRTLA sender",
            "Не удалось запустить SRTLA sender");
        blog(LOG_ERROR,
             "[Mikhlink] Failed to start srtla_send.exe: %s",
             srtlaSender->errorString().toUtf8().constData());
        stopSrtlaSender();
        return false;
    }

    setTelemetryStatus(
        "Connecting SRTLA channels…",
        "Подключение каналов SRTLA…");
    blog(LOG_INFO,
         "[Mikhlink] SRTLA sender started: local port %d, remote host %s, remote port %d, physical uplinks %d, Moblink relays %d.",
         LocalSrtlaPort,
         destination.host().toUtf8().constData(),
         remotePort,
         static_cast<int>(selected.size()),
         static_cast<int>(std::count_if(
             moblinkRelays.cbegin(),
             moblinkRelays.cend(),
             [](const mikhlink::moblink::RelaySnapshot& relay) {
                 return relay.tunnelReady;
             })));
    return true;
}

void frontendEvent(obs_frontend_event event, void*)
{
    if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING)
    {
        configureMoblinkServerFromCurrentService();
    }
    else if (event == OBS_FRONTEND_EVENT_STREAMING_STARTING)
    {
        setUplinkControlsEnabled(false);
        startSrtlaSender();
    }
    else if (event == OBS_FRONTEND_EVENT_STREAMING_STOPPED ||
             event == OBS_FRONTEND_EVENT_EXIT)
    {
        stopSrtlaSender();
        setUplinkControlsEnabled(true);
        if (event == OBS_FRONTEND_EVENT_EXIT)
        {
            removeTelemetryDock();
        }
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
    dialog.setMinimumWidth(720);

    auto* name = new QLineEdit(&dialog);
    name->setText("BELABOX");
    auto* ingestKey = new QLineEdit(&dialog);
    ingestKey->setEchoMode(QLineEdit::Password);
    auto* srtIngestUrl = new QLineEdit(&dialog);
    srtIngestUrl->setMinimumWidth(500);
    auto* port = new QSpinBox(&dialog);
    port->setRange(1, 65535);
    port->setValue(5000);
    auto* useSrtla = new QCheckBox(&dialog);
    useSrtla->setText(localized(
        "Use SRTLA bonding",
        "Использовать SRTLA bonding"));
    auto* moblinkEnabled = new QCheckBox(&dialog);
    moblinkEnabled->setText(localized(
        "Accept Moblink phones",
        "Подключать телефоны Moblink"));
    auto* moblinkName = new QLineEdit(&dialog);
    moblinkName->setText("Mikhlink OBS");
    auto* moblinkPassword = new QLineEdit(&dialog);
    moblinkPassword->setEchoMode(QLineEdit::Password);
    moblinkPassword->setText("1234");
    auto* moblinkPort = new QSpinBox(&dialog);
    moblinkPort->setRange(1, 65535);
    moblinkPort->setValue(DefaultMoblinkPort);

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
        moblinkEnabled->setChecked(
            obs_data_get_bool(currentSettings, MoblinkEnabledSetting));
        moblinkName->setText(QString::fromUtf8(
            obs_data_get_string(currentSettings, MoblinkNameSetting)));
        if (moblinkName->text().trimmed().isEmpty())
        {
            moblinkName->setText("Mikhlink OBS");
        }
        moblinkPassword->setText(QString::fromUtf8(
            obs_data_get_string(currentSettings, MoblinkPasswordSetting)));
        const int savedMoblinkPort = static_cast<int>(
            obs_data_get_int(currentSettings, MoblinkPortSetting));
        moblinkPort->setValue(
            savedMoblinkPort >= 1 && savedMoblinkPort <= 65535
                ? savedMoblinkPort
                : DefaultMoblinkPort);

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

    auto* moblinkGroup = new QGroupBox("Moblink", &dialog);
    auto* moblinkLayout = new QFormLayout(moblinkGroup);
    auto* moblinkHelp = new QLabel(
        localized(
            "On the phone enable Manual mode and use ws://PC_LAN_IP:port. The phone forwards this SRTLA channel through its selected mobile network.",
            "На телефоне включите Manual и укажите ws://LAN_IP_ПК:порт. Телефон передаст этот канал SRTLA через выбранную мобильную сеть."),
        moblinkGroup);
    moblinkHelp->setWordWrap(true);
    moblinkLayout->addRow(moblinkEnabled);
    moblinkLayout->addRow(
        localized("Streamer name", "Имя сервера"), moblinkName);
    moblinkLayout->addRow(
        localized("Password", "Пароль"), moblinkPassword);
    moblinkLayout->addRow(
        localized("WebSocket port", "WebSocket-порт"), moblinkPort);
    moblinkLayout->addRow(moblinkHelp);
    layout->addRow(moblinkGroup);
    layout->addRow(buttons);
    dialog.resize(760, dialog.sizeHint().height());

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

    if (moblinkEnabled->isChecked() &&
        (moblinkName->text().trimmed().isEmpty() ||
         moblinkPassword->text().isEmpty()))
    {
        QMessageBox::warning(
            parent,
            "Mikhlink",
            localized(
                "Moblink streamer name and password are required when Moblink is enabled.",
                "При включённом Moblink задайте имя сервера и пароль."));
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
    obs_data_set_bool(
        settings,
        MoblinkEnabledSetting,
        moblinkEnabled->isChecked());
    const QByteArray moblinkNameUtf8 =
        moblinkName->text().trimmed().toUtf8();
    const QByteArray moblinkPasswordUtf8 =
        moblinkPassword->text().toUtf8();
    obs_data_set_string(
        settings,
        MoblinkNameSetting,
        moblinkNameUtf8.constData());
    obs_data_set_string(
        settings,
        MoblinkPasswordSetting,
        moblinkPasswordUtf8.constData());
    obs_data_set_int(
        settings,
        MoblinkPortSetting,
        moblinkPort->value());

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
    configureMoblinkServerFromCurrentService();

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
              "SRTLA bonding is enabled. OBS will send SRT locally and Mikhlink will forward it through all enabled channels.",
              "Включён SRTLA-бондинг. OBS отправит SRT локально, а Mikhlink перенаправит его через все включённые каналы.")
        : localized(
              "Direct SRT mode is enabled.",
              "Включён режим прямого SRT.");
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

    moblinkServer = std::make_unique<mikhlink::moblink::Server>();
    moblinkServer->onLog = [](const QString& message) {
        blog(LOG_INFO,
             "[Mikhlink/Moblink] %s",
             message.toUtf8().constData());
    };
    moblinkServer->onRelaysChanged = [](
        const std::vector<mikhlink::moblink::RelaySnapshot>& relays) {
        synchronizeMoblinkRelays(relays);
    };
    writeMoblinkRelaysFile();

    registerOutput();
    registerService();
    createTelemetryDock();
    obs_frontend_add_event_callback(frontendEvent, nullptr);
    obs_frontend_add_tools_menu_item(
        "Mikhlink", openMikhlinkSettings, nullptr);
    QTimer::singleShot(0, [] { configureMoblinkServerFromCurrentService(); });

    blog(LOG_INFO,
         "[Mikhlink] Output, streaming service, and settings menu registered.");
    return true;
}

void obs_module_unload(void)
{
    obs_frontend_remove_event_callback(frontendEvent, nullptr);
    stopSrtlaSender();
    if (moblinkServer != nullptr)
    {
        moblinkServer->stop();
        moblinkServer->onRelaysChanged = nullptr;
        moblinkServer->onLog = nullptr;
        moblinkServer.reset();
    }
    moblinkRelays.clear();
    QFile::remove(moblinkRelaysPath());
    removeTelemetryDock();
}
