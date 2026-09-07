#pragma once

#include <Strata.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace Strata::FreeRTOS {

class RecursiveMutex {
  public:
	RecursiveMutex() noexcept = default;
	~RecursiveMutex() noexcept { reset(); }
	RecursiveMutex(const RecursiveMutex &) = delete;
	RecursiveMutex &operator=(const RecursiveMutex &) = delete;
	RecursiveMutex(RecursiveMutex &&other) noexcept : _handle(std::exchange(other._handle, nullptr)) {}
	RecursiveMutex &operator=(RecursiveMutex &&other) noexcept {
		if (this != &other) {
			reset();
			_handle = std::exchange(other._handle, nullptr);
		}
		return *this;
	}

	static RecursiveMutex create() noexcept {
		RecursiveMutex mutex;
		mutex._handle = xSemaphoreCreateRecursiveMutex();
		return mutex;
	}

	void reset() noexcept {
		if (_handle != nullptr) {
			vSemaphoreDelete(_handle);
			_handle = nullptr;
		}
	}

	bool lock(TickType_t ticks = portMAX_DELAY) noexcept {
		return _handle != nullptr && xSemaphoreTakeRecursive(_handle, ticks) == pdTRUE;
	}

	void unlock() noexcept {
		if (_handle != nullptr) (void)xSemaphoreGiveRecursive(_handle);
	}

	explicit operator bool() const noexcept { return _handle != nullptr; }

  private:
	SemaphoreHandle_t _handle = nullptr;
};

} // namespace Strata::FreeRTOS
