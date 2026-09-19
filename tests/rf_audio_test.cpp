// Tests production rf_audio.cpp with bounded host FreeRTOS/queue substitutes.
// No ESP hardware, serial, GUI, or real audio device is accessed.
#include "rf_audio.h"
#include <freertos/queue.h>
#include <freertos/task.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

struct FakeAudioQueue {
    size_t capacity;
    size_t item_size;
    size_t maximum_queued = 0;
    std::mutex mutex;
    std::condition_variable available;
    std::deque<std::vector<uint8_t>> items;
};

namespace {
struct TaskFinished {};
std::unique_ptr<FakeAudioQueue> queue_owner;
std::vector<std::thread> tasks;
std::atomic<bool> end_tasks{false};
bool fail_task_once = true;
unsigned task_attempts = 0;
std::mutex pop_mutex;
std::condition_variable pop_changed;
bool pause_next_pop = false;
bool pop_paused = false;
bool release_pop = false;

struct Delivered { int value; int frames; };
std::mutex observed_mutex;
std::condition_variable observed_changed;
std::vector<Delivered> delivered;
bool hold_next = false;
bool held = false;
bool release_held = false;

void receiveAudio(const float *iq, int frames)
{
    assert(iq && frames > 0 && frames <= static_cast<int>(rf_audio::BLOCK_FRAMES));
    const int value = static_cast<int>(iq[0]);
    for (int i = 0; i < frames * 2; ++i) assert(iq[i] == value);
    std::unique_lock<std::mutex> lock(observed_mutex);
    delivered.push_back({value, frames});
    if (hold_next) {
        hold_next = false;
        held = true;
        observed_changed.notify_all();
        assert(observed_changed.wait_for(lock, 2s, [] { return release_held; }));
        held = false;
    }
    observed_changed.notify_all();
}

void holdOne()
{
    std::lock_guard<std::mutex> lock(observed_mutex);
    assert(!held);
    hold_next = true;
    release_held = false;
}

void awaitHeld()
{
    std::unique_lock<std::mutex> lock(observed_mutex);
    assert(observed_changed.wait_for(lock, 1s, [] { return held; }));
}

void releaseOne()
{
    std::lock_guard<std::mutex> lock(observed_mutex);
    release_held = true;
    observed_changed.notify_all();
}

void awaitCount(size_t count)
{
    std::unique_lock<std::mutex> lock(observed_mutex);
    assert(observed_changed.wait_for(lock, 1s, [=] { return delivered.size() >= count; }));
}

void clearObserved()
{
    std::lock_guard<std::mutex> lock(observed_mutex);
    delivered.clear();
}

void submit(int value, size_t frames = rf_audio::BLOCK_FRAMES)
{
    std::array<float, rf_audio::BLOCK_FRAMES * 2> data;
    data.fill(static_cast<float>(value));
    rf_audio::submit(data.data(), frames);
    // The source belongs to the producer; queued callbacks must not retain it.
    data.fill(-999.0f);
}

void expectValues(std::initializer_list<int> values)
{
    std::lock_guard<std::mutex> lock(observed_mutex);
    assert(delivered.size() == values.size());
    size_t i = 0;
    for (int value : values) assert(delivered[i++].value == value);
}

void holdPoppedBlock()
{
    std::lock_guard<std::mutex> lock(pop_mutex);
    pause_next_pop = true;
    release_pop = false;
}

void awaitPoppedBlock()
{
    std::unique_lock<std::mutex> lock(pop_mutex);
    assert(pop_changed.wait_for(lock, 1s, [] { return pop_paused; }));
}

void releasePoppedBlock()
{
    std::lock_guard<std::mutex> lock(pop_mutex);
    release_pop = true;
    pop_changed.notify_all();
}
} // namespace

QueueHandle_t xQueueCreateStatic(UBaseType_t count, UBaseType_t item_size,
                                uint8_t *storage, StaticQueue_t *control)
{
    assert(count == rf_audio::QUEUE_BLOCKS && storage && control);
    assert(!queue_owner);
    queue_owner = std::make_unique<FakeAudioQueue>();
    queue_owner->capacity = count;
    queue_owner->item_size = item_size;
    return queue_owner.get();
}

BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t wait)
{
    assert(queue && item && wait == 0); // Producer must never block.
    std::lock_guard<std::mutex> lock(queue->mutex);
    if (queue->items.size() == queue->capacity) return pdFALSE;
    const auto *bytes = static_cast<const uint8_t *>(item);
    queue->items.emplace_back(bytes, bytes + queue->item_size);
    queue->maximum_queued = std::max(queue->maximum_queued, queue->items.size());
    queue->available.notify_one();
    return pdTRUE;
}

BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t wait)
{
    assert(queue && item);
    std::unique_lock<std::mutex> lock(queue->mutex);
    if (wait == portMAX_DELAY)
        queue->available.wait(lock, [&] { return end_tasks || !queue->items.empty(); });
    else assert(wait == 0);
    if (end_tasks) throw TaskFinished{};
    if (queue->items.empty()) return pdFALSE;
    std::memcpy(item, queue->items.front().data(), queue->item_size);
    queue->items.pop_front();
    lock.unlock();
    if (wait == portMAX_DELAY) {
        std::unique_lock<std::mutex> pop_lock(pop_mutex);
        if (pause_next_pop) {
            pause_next_pop = false;
            pop_paused = true;
            pop_changed.notify_all();
            assert(pop_changed.wait_for(pop_lock, 2s, [] { return release_pop; }));
            pop_paused = false;
        }
    }
    return pdTRUE;
}

BaseType_t xQueueReset(QueueHandle_t queue)
{
    assert(queue);
    std::lock_guard<std::mutex> lock(queue->mutex);
    queue->items.clear();
    return pdPASS;
}

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t task, const char *name,
                                 uint32_t stack, void *argument,
                                 UBaseType_t priority, TaskHandle_t *handle,
                                 BaseType_t core)
{
    assert(task && std::string(name) == "dx_audio" && stack >= 4096);
    assert(priority == 3 && core == 0); // DSP offloads away from RF/ADC CPU1.
    ++task_attempts;
    if (fail_task_once) { fail_task_once = false; return pdFALSE; }
    if (handle) *handle = reinterpret_cast<TaskHandle_t>(1);
    tasks.emplace_back([=] {
        try { task(argument); }
        catch (const TaskFinished &) {}
    });
    return pdPASS;
}

void vTaskDelay(TickType_t ticks) { std::this_thread::sleep_for(std::chrono::milliseconds(ticks)); }

int main()
{
    // Initialization failure leaves no live callback; retry reuses static queue.
    assert(!rf_audio::open(receiveAudio));
    submit(99);
    assert(!rf_audio::initialize());
    assert(!rf_audio::open(receiveAudio));
    assert(rf_audio::initialize());
    assert(rf_audio::initialize());
    assert(task_attempts == 2 && tasks.size() == 1);
    assert(!rf_audio::open(nullptr));
    assert(rf_audio::open(receiveAudio));

    // Hold DSP to fill the queue: only the newest four blocks survive.
    holdOne(); submit(1); awaitHeld();
    for (int value = 2; value <= 7; ++value) submit(value);
    assert(queue_owner->maximum_queued == rf_audio::QUEUE_BLOCKS);
    releaseOne(); awaitCount(5); rf_audio::close();
    expectValues({1, 4, 5, 6, 7});

    // Invalidation drops old queued blocks but does not wait for in-flight DSP.
    clearObserved(); assert(rf_audio::open(receiveAudio));
    holdOne(); submit(10); awaitHeld();
    submit(11); submit(12); submit(13);
    rf_audio::invalidate();
    submit(14, 11);
    releaseOne(); awaitCount(2); rf_audio::close();
    expectValues({10, 14});
    assert(delivered.back().frames == 11);

    // Queue reset cannot remove an already-popped block. Its generation must
    // reject it before callback invocation, including across close/reopen.
    clearObserved(); assert(rf_audio::open(receiveAudio));
    holdPoppedBlock(); submit(15); awaitPoppedBlock();
    rf_audio::invalidate();
    submit(16);
    releasePoppedBlock(); awaitCount(1); rf_audio::close();
    expectValues({16});

    clearObserved(); assert(rf_audio::open(receiveAudio));
    holdPoppedBlock(); submit(17); awaitPoppedBlock();
    rf_audio::close();
    assert(rf_audio::open(receiveAudio));
    submit(18);
    releasePoppedBlock(); awaitCount(1); rf_audio::close();
    expectValues({18});

    // Close is a lifetime barrier even if a callback is already blocked.
    clearObserved(); assert(rf_audio::open(receiveAudio));
    holdOne(); submit(20); awaitHeld(); submit(21);
    std::atomic<bool> close_returned{false};
    std::thread closer([&] { rf_audio::close(); close_returned = true; });
    std::this_thread::sleep_for(20ms);
    assert(!close_returned);
    releaseOne(); closer.join(); assert(close_returned);
    submit(22);
    assert(rf_audio::open(receiveAudio));
    submit(23); awaitCount(2); rf_audio::close();
    expectValues({20, 23});

    // Malformed/empty input cannot reach a callback or overflow packet storage.
    clearObserved(); assert(rf_audio::open(receiveAudio));
    rf_audio::submit(nullptr, 1);
    submit(30, 0);
    submit(31, rf_audio::BLOCK_FRAMES + 1);
    submit(32, 1); awaitCount(1); rf_audio::close();
    expectValues({32});

    end_tasks = true;
    queue_owner->available.notify_all();
    for (auto &task : tasks) task.join();
    std::puts("RF audio dispatcher PASS: bounded freshest queue, copied blocks, generation reset, close barrier, CPU0 offload");
}
