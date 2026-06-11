#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

class PhaseMutex {
  public:
	PhaseMutex() {
		_handle = xSemaphoreCreateRecursiveMutex();
	}

	~PhaseMutex() {
		if (_handle != nullptr) {
			vSemaphoreDelete(_handle);
		}
	}

	PhaseMutex(const PhaseMutex &) = delete;
	PhaseMutex &operator=(const PhaseMutex &) = delete;

	bool lock(TickType_t timeout = portMAX_DELAY) {
		return _handle != nullptr && xSemaphoreTakeRecursive(_handle, timeout) == pdTRUE;
	}

	void unlock() {
		if (_handle != nullptr) {
			xSemaphoreGiveRecursive(_handle);
		}
	}

  private:
	SemaphoreHandle_t _handle = nullptr;
};

class PhaseLock {
  public:
	explicit PhaseLock(PhaseMutex &mutex) : _mutex(mutex), _locked(mutex.lock()) {
	}

	~PhaseLock() {
		if (_locked) {
			_mutex.unlock();
		}
	}

	PhaseLock(const PhaseLock &) = delete;
	PhaseLock &operator=(const PhaseLock &) = delete;

	explicit operator bool() const {
		return _locked;
	}

  private:
	PhaseMutex &_mutex;
	bool _locked = false;
};
