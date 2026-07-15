#include <freertos/task.h>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

struct FakeTask {
	std::mutex mutex;
	std::condition_variable cv;
	uint32_t notifications = 0;
};
thread_local TaskHandle_t gCurrentTask = nullptr;
BaseType_t xTaskCreate(TaskFunction_t entry, const char *, uint32_t, void *arg, UBaseType_t, TaskHandle_t *out) {
	auto *task = new FakeTask();
	*out = task;
	std::thread([task, entry, arg] { gCurrentTask = task; entry(arg); gCurrentTask = nullptr; }).detach();
	return pdPASS;
}
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t entry, const char *name, uint32_t stack, void *arg, UBaseType_t priority, TaskHandle_t *out, BaseType_t) {
	return xTaskCreate(entry, name, stack, arg, priority, out);
}
void xTaskNotifyGive(TaskHandle_t task) {
	if (!task) return;
	{ std::lock_guard<std::mutex> lock(task->mutex); task->notifications++; }
	task->cv.notify_all();
}
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t timeout) {
	TaskHandle_t task = gCurrentTask;
	if (!task) return 0;
	std::unique_lock<std::mutex> lock(task->mutex);
	if (timeout == portMAX_DELAY) task->cv.wait(lock, [&] { return task->notifications > 0; });
	else if (!task->cv.wait_for(lock, std::chrono::milliseconds(timeout), [&] { return task->notifications > 0; })) return 0;
	uint32_t value = task->notifications;
	if (clear) task->notifications = 0; else task->notifications--;
	return value;
}
void vTaskDelay(TickType_t ticks) { std::this_thread::sleep_for(std::chrono::milliseconds(ticks)); }
TaskHandle_t xTaskGetCurrentTaskHandle() { return gCurrentTask; }
void vTaskDelete(TaskHandle_t) {}
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t) { return 123; }
