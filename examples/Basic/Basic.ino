#include <Arduino.h>
#include <Phase.h>

Phase phase;

PhaseResult initStorage() {
	Serial.println("storage init");
	return PhaseResult::success();
}

PhaseResult deinitStorage() {
	Serial.println("storage deinit");
	return PhaseResult::success();
}

void setup() {
	Serial.begin(115200);

	PhaseResult result = phase.init();
	if (!result) {
		Serial.println(result.message);
		return;
	}

	phase.add("storage", initStorage, deinitStorage);

	phase.onReady([]() {
		Serial.println("Phase ready");
	});

	phase.onFailed([](PhaseResult failed) {
		Serial.println(failed.message);
	});

	phase.start();
	Serial.println("setup continues while Phase boots");
}

void loop() {
	delay(1000);
}
