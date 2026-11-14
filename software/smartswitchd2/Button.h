#pragma once

#include "Arduino.h"
#include "Button2.h"

class Button {
	typedef std::function<void()> CallbackFunction;

public:
	Button(uint8_t _pin);

	void initialize(CallbackFunction pressed_handler, CallbackFunction hold_handler);

	void loop();

private:
	uint8_t pin;

	CallbackFunction hold_cb = NULL;
	CallbackFunction pressed_cb = NULL;

	Button2* button2;

	void hold(Button2& btn);
	void pressed(Button2& btn);
};