#include "Button.h"

Button::Button(uint8_t _pin)
{
	pin = _pin;
}

void Button::initialize(CallbackFunction pressed_handler, CallbackFunction hold_handler)
{
	pressed_cb = pressed_handler;
	hold_cb = hold_handler;

	button2 = new Button2;
	button2->begin(pin);

	button2->setPressedHandler(std::bind(&Button::pressed, this, std::placeholders::_1));

	button2->setLongClickDetectedHandler(std::bind(&Button::hold, this, std::placeholders::_1));
	button2->setLongClickTime(5000);

	button2->setDebounceTime(2);
}

void Button::loop()
{
	button2->loop();
}

// --- Private Handlers ---

void Button::hold(Button2& btn)
{
	if (hold_cb) hold_cb();
}

void Button::pressed(Button2& btn)
{
	if (pressed_cb) pressed_cb();
}