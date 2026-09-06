#include <freertos/task.h>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace {
struct TaskDeleted {};
}

struct FakeTask {
	std::mutex mutex;
	std::condition_variable cv;
	uint32_t notifications = 0;
	bool deleted = false;
};

thread_local TaskHandle_t gCurrentTask = nullptr;

BaseType_t xTaskCreate(TaskFunction_t entry, const char *, uint32_t, void *arg, UBaseType_t, TaskHandle_t *out) {
	auto *task = new FakeTask();
	*out = task;
	std::thread([task, entry, arg] {
		gCurrentTask = task;
		try {
			entry(arg);
		} catch (const TaskDeleted &) {
		}
		gCurrentTask = nullptr;
	}).detach();
	return pdPASS;
}

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t entry, const char *name, uint32_t stack, void *arg, UBaseType_t priority, TaskHandle_t *out, BaseType_t) {
	return xTaskCreate(entry, name, stack, arg, priority, out);
}

void xTaskNotifyGive(TaskHandle_t task) {
	if (!task) return;
	{
		std::lock_guard<std::mutex> lock(task->mutex);
		task->notifications++;
	}
	task->cv.notify_all();
}

uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t timeout) {
	TaskHandle_t task = gCurrentTask;
	if (!task) return 0;
	std::unique_lock<std::mutex> lock(task->mutex);
	if (timeout == portMAX_DELAY) {
		task->cv.wait(lock, [&] { return task->notifications > 0 || task->deleted; });
	} else if (!task->cv.wait_for(
	               lock,
	               std::chrono::milliseconds(timeout),
	               [&] { return task->notifications > 0 || task->deleted; })) {
		return 0;
	}
	if (task->deleted) throw TaskDeleted{};
	uint32_t value = task->notifications;
	if (clear) task->notifications = 0;
	else task->notifications--;
	return value;
}

void vTaskDelay(TickType_t ticks) {
	if (ticks == portMAX_DELAY) ticks = 1;
	std::this_thread::sleep_for(std::chrono::milliseconds(ticks));
}

void vTaskSuspend(TaskHandle_t handle) {
	TaskHandle_t task = handle != nullptr ? handle : gCurrentTask;
	if (!task) return;
	std::unique_lock<std::mutex> lock(task->mutex);
	task->cv.wait(lock, [&] { return task->deleted; });
	throw TaskDeleted{};
}

TaskHandle_t xTaskGetCurrentTaskHandle() { return gCurrentTask; }

void vTaskDelete(TaskHandle_t handle) {
	TaskHandle_t task = handle != nullptr ? handle : gCurrentTask;
	if (!task) return;
	{
		std::lock_guard<std::mutex> lock(task->mutex);
		task->deleted = true;
	}
	task->cv.notify_all();
}

UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t) { return 123; }
