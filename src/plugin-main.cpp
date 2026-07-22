#include <obs-frontend-api.h>
#include <obs-module.h>

#include "network/NetworkAdapter.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <new>

OBS_DECLARE_MODULE()

namespace
{

constexpr const char* OutputId = "mikhlink_test_output";

struct MikhlinkOutput
{
    obs_output_t* output = nullptr;
    std::atomic<std::uint64_t> videoFrames{0};
    std::atomic<std::uint64_t> audioBlocks{0};
    std::chrono::steady_clock::time_point startedAt;
};

obs_output_t* testOutput = nullptr;

const char* outputName(void*)
{
    return "Mikhlink Test Output";
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

    if (!obs_output_can_begin_data_capture(state->output, 0))
    {
        blog(LOG_ERROR, "[Mikhlink] OBS refused to begin test capture.");
        return false;
    }

    state->videoFrames.store(0, std::memory_order_relaxed);
    state->audioBlocks.store(0, std::memory_order_relaxed);
    state->startedAt = std::chrono::steady_clock::now();

    if (!obs_output_begin_data_capture(state->output, 0))
    {
        blog(LOG_ERROR, "[Mikhlink] Failed to begin test capture.");
        return false;
    }

    blog(LOG_INFO, "[Mikhlink] Test capture started.");
    return true;
}

void stopOutput(void* data, std::uint64_t)
{
    auto* state = static_cast<MikhlinkOutput*>(data);
    obs_output_end_data_capture(state->output);

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - state->startedAt);

    blog(LOG_INFO,
         "[Mikhlink] Test capture stopped after %lld ms. Video frames: %llu. Audio blocks: %llu.",
         static_cast<long long>(elapsed.count()),
         static_cast<unsigned long long>(
             state->videoFrames.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(
             state->audioBlocks.load(std::memory_order_relaxed)));
}

void receiveVideo(void* data, video_data*)
{
    auto* state = static_cast<MikhlinkOutput*>(data);
    state->videoFrames.fetch_add(1, std::memory_order_relaxed);
}

void receiveAudio(void* data, audio_data*)
{
    auto* state = static_cast<MikhlinkOutput*>(data);
    state->audioBlocks.fetch_add(1, std::memory_order_relaxed);
}

void toggleTestCapture(void*)
{
    if (testOutput == nullptr)
    {
        testOutput = obs_output_create(
            OutputId, "Mikhlink Test Output", nullptr, nullptr);

        if (testOutput == nullptr)
        {
            blog(LOG_ERROR, "[Mikhlink] Failed to create test output.");
            return;
        }
    }

    if (obs_output_active(testOutput))
    {
        obs_output_stop(testOutput);
        return;
    }

    if (!obs_output_start(testOutput))
    {
        blog(LOG_ERROR, "[Mikhlink] Failed to start test output.");
    }
}

void registerOutput()
{
    obs_output_info info = {};
    info.id = OutputId;
    info.flags = OBS_OUTPUT_AV;
    info.get_name = outputName;
    info.create = createOutput;
    info.destroy = destroyOutput;
    info.start = startOutput;
    info.stop = stopOutput;
    info.raw_video = receiveVideo;
    info.raw_audio = receiveAudio;

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
    obs_frontend_add_tools_menu_item(
        "Mikhlink: Start/Stop Test Capture", toggleTestCapture, nullptr);

    blog(LOG_INFO, "[Mikhlink] Test output registered.");
    return true;
}

void obs_module_unload(void)
{
    if (testOutput != nullptr)
    {
        if (obs_output_active(testOutput))
        {
            obs_output_stop(testOutput);
        }

        obs_output_release(testOutput);
        testOutput = nullptr;
    }
}
