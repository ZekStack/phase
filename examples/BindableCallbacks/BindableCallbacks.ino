#include <Arduino.h>
#include <Phase.h>

class PrivateModule {
  public:
	void registerWith(Phase &phase) {
		phase.add("private-module", [this]() {
			return init();
		}, [this]() {
			return deinit();
		}).start([this]() {
			return start();
		}, [this]() {
			return stop();
		});
	}

  private:
	PhaseResult init() {
		Serial.println("private init");
		return PhaseResult::success();
	}

	PhaseResult start() {
		Serial.println("private start");
		return PhaseResult::success();
	}

	PhaseResult stop() {
		Serial.println("private stop");
		return PhaseResult::success();
	}

	PhaseResult deinit() {
		Serial.println("private deinit");
		return PhaseResult::success();
	}
};

Phase phase;
PrivateModule module;

void setup() {
	Serial.begin(115200);

	PhaseResult result = phase.init();
	if (!result) {
		Serial.println(result.message);
		return;
	}

	module.registerWith(phase);

	phase.onReady([]() {
		Serial.println("private module ready");
	});

	phase.start();
}

void loop() {
	delay(1000);
}
