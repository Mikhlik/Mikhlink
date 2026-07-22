#include <obs-module.h>

#include "network/NetworkAdapter.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <new>
#include <string>

OBS_DECLARE_MODULE()

namespace
{

constexpr const char* OutputId = "mikhlink_output";
constexpr const char* ServiceId = "mikhlink_service";
constexpr const char* AddressSetting = "address";
constexpr const char* PortSetting = "port";

const char* SupportedVideoCodecs[] = {"h264", "hevc", nullptr};
const char* SupportedAudioCodecs[] = {"aac", "opus", nullptr};

struct MikhlinkService
{
    std::string address;
    int port = 5000;
    std::string url;
};

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
    service->address = obs_data_get_string(settings, AddressSetting);
    service->port = static_cast<int>(obs_data_get_int(settings, PortSetting));

    service->url.clear();
    if (!service->address.empty() && service->port > 0)
    {
        service->url =
            "srtla://" + service->address + ":" + std::to_string(service->port);
    }
}

const char* serviceName(void*)
{
    return "Mikhlink (SRTLA/BELABOX)";
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
    obs_data_set_default_string(settings, AddressSetting, "");
    obs_data_set_default_int(settings, PortSetting, 5000);
}

obs_properties_t* serviceProperties(void*)
{
    obs_properties_t* properties = obs_properties_create();

    obs_properties_add_text(
        properties,
        AddressSetting,
        "BELABOX / SRTLA address",
        OBS_TEXT_DEFAULT);

    obs_properties_add_int(
        properties,
        PortSetting,
        "BELABOX / SRTLA port",
        1,
        65535,
        1);

    return properties;
}

const char* serviceUrl(void* data)
{
    return static_cast<MikhlinkService*>(data)->url.c_str();
}

const char* serviceKey(void*)
{
    return "";
}

const char* serviceOutputType(void*)
{
    return OutputId;
}

const char* serviceProtocol(void*)
{
    return "SRTLA";
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

    return nullptr;
}

bool serviceCanConnect(void* data)
{
    const auto* service = static_cast<MikhlinkService*>(data);
    return !service->address.empty() && service->port > 0;
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
             "[Mikhlink] BELABOX/SRTLA address and port are not configured.");
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
         "[Mikhlink] Encoded capture started for %s using video=%s, audio=%s.",
         obs_service_get_connect_info(
             service, OBS_SERVICE_CONNECT_INFO_SERVER_URL),
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

    blog(LOG_INFO, "[Mikhlink] Output and streaming service registered.");
    return true;
}
