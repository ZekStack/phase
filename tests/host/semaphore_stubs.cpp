#include <Arduino.h>
#include <freertos/semphr.h>
#include <chrono>
#include <condition_variable>
#include <mutex>

using Clock = std::chrono::steady_clock;
static const auto gStart = Clock::now();
uint32_t millis() {
	return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - gStart).count());
}

struct FakeSemaphore {
	enum class Kind { Recursive, Binary } kind;
	std::recursive_timed_mutex recursive;
	std::mutex mutex;
	std::condition_variable cv;
	bool available = false;
	explicit FakeSemaphore(Kind value) : kind(value) {}
};

SemaphoreHandle_t xSemaphoreCreateRecursiveMutex() { return new FakeSemaphore(FakeSemaphore::Kind::Recursive); }
SemaphoreHandle_t xSemaphoreCreateBinary() { return new FakeSemaphore(FakeSemaphore::Kind::Binary); }
BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t handle, TickType_t timeout) {
	if (!handle) return pdFALSE;
	if (timeout == portMAX_DELAY) { handle->recursive.lock(); return pdTRUE; }
	return handle->recursive.try_lock_for(std::chrono::milliseconds(timeout)) ? pdTRUE : pdFALSE;
}
BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t handle) {
	if (!handle) return pdFALSE;
	handle->recursive.unlock();
	return pdTRUE;
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t handle, TickType_t timeout) {
	if (!handle) return pdFALSE;
	std::unique_lock<std::mutex> lock(handle->mutex);
	if (timeout == 0) {
		if (!handle->available) return pdFALSE;
	} else if (timeout == portMAX_DELAY) {
		handle->cv.wait(lock, [&] { return handle->available; });
	} else if (!handle->cv.wait_for(lock, std::chrono::milliseconds(timeout), [&] { return handle->available; })) {
		return pdFALSE;
	}
	handle->available = false;
	return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t handle) {
	if (!handle) return pdFALSE;
	{ std::lock_guard<std::mutex> lock(handle->mutex); handle->available = true; }
	handle->cv.notify_all();
	return pdTRUE;
}
void vSemaphoreDelete(SemaphoreHandle_t handle) { delete handle; }
