#include "rf_audio.h"
#include <atomic>
#include <cstdint>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

namespace rf_audio {
namespace {
struct Block {
    uint32_t generation;
    size_t frames;
    float iq[BLOCK_FRAMES * 2];
};

StaticQueue_t queue_control;
alignas(16) uint8_t queue_storage[QUEUE_BLOCKS * sizeof(Block)];
QueueHandle_t queue = nullptr;
std::atomic<bool> ready{false};
std::atomic<pal::AudioInputCallback> callback{nullptr};
std::atomic<bool> callback_busy{false};
std::atomic<uint32_t> generation{0};

// Only the RF producer uses these; avoid spending half its stack on packets.
Block outgoing{};
Block discarded{};

void consume(void *)
{
    Block block{};
    for (;;) {
        if (xQueueReceive(queue, &block, portMAX_DELAY) != pdTRUE) continue;
        // Set busy BEFORE loading the callback: close() first clears the
        // pointer, then waits for this section, protecting the caller's lifetime.
        callback_busy.store(true);
        const auto target = callback.load();
        if (target && block.generation == generation.load())
            target(block.iq, static_cast<int>(block.frames));
        callback_busy.store(false);
    }
}
} // namespace

bool initialize()
{
    if (ready.load()) return true;
    if (!queue)
        queue = xQueueCreateStatic(QUEUE_BLOCKS, sizeof(Block), queue_storage, &queue_control);
    if (!queue || xTaskCreatePinnedToCore(consume, "dx_audio", 8192, nullptr, 3,
                                        nullptr, 0) != pdPASS) return false;
    ready.store(true);
    return true;
}

bool open(pal::AudioInputCallback target)
{
    if (!ready.load() || !target) return false;
    close();
    callback.store(target);
    return true;
}

void close()
{
    callback.store(nullptr);
    invalidate();
    while (callback_busy.load()) vTaskDelay(1);
}

void invalidate()
{
    generation.fetch_add(1);
    if (ready.load()) (void)xQueueReset(queue);
}

void submit(const float *iq, size_t frames)
{
    // Snapshot before the callback check so a concurrent close/reopen cannot
    // relabel an already-started submission as belonging to the new stream.
    const uint32_t submitted_generation = generation.load();
    if (!ready.load() || !callback.load() || !iq || frames == 0 || frames > BLOCK_FRAMES)
        return;
    outgoing.generation = submitted_generation;
    outgoing.frames = frames;
    std::memcpy(outgoing.iq, iq, frames * 2 * sizeof(float));
    if (xQueueSend(queue, &outgoing, 0) == pdTRUE) return;
    // Retain freshness if speaker/DSP falls behind. Never let an audio queue
    // stall the RF control worker or grow unbounded latency while tuning.
    (void)xQueueReceive(queue, &discarded, 0);
    (void)xQueueSend(queue, &outgoing, 0);
}
} // namespace rf_audio
