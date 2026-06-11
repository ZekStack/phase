#include <Arduino.h>
#include <Phase.h>

Phase phase;
uint32_t shutdownAtMs = 0;
bool shutdownRequested = false;

void setup() {
	Serial.begin(115200);

	PhaseResult result = phase.init();
	if (!result) {
		Serial.println(result.message);
		return;
	}

	phase.add("network", []() {
		Serial.println("network init");
		return true;
	}, []() {
		Serial.println("network deinit");
		return true;
	}).start([]() {
		Serial.println("network start");
		return true;
	}, []() {
		Serial.println("network stop");
		return true;
	});

	phase.onReady([]() {
		Serial.println("ready, shutdown in three seconds");
		shutdownAtMs = millis() + 3000;
	});

	phase.start();
}

void loop() {
	if (!shutdownRequested && shutdownAtMs > 0 && millis() >= shutdownAtMs) {
		shutdownRequested = true;
		phase.stop();
	}
	delay(20);
}
