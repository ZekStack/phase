#include <Arduino.h>
#include <Phase.h>

Phase phase;
uint32_t resumeAtMs = 0;
bool resumed = false;

void setup() {
	Serial.begin(115200);

	PhaseResult result = phase.init();
	if (!result) {
		Serial.println(result.message);
		return;
	}

	phase.add("storage", []() {
		Serial.println("storage init after resume");
	});

	phase.onChange([](PhaseChange change) {
		Serial.println(change.message);
	});

	phase.onReady([]() {
		Serial.println("ready after pause");
	});

	phase.pause("startup-delay");
	phase.start();

	resumeAtMs = millis() + 2000;
	Serial.println("boot is paused for two seconds");
}

void loop() {
	if (!resumed && millis() >= resumeAtMs) {
		resumed = true;
		phase.resume();
	}
	delay(20);
}
