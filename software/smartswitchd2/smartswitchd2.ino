#include "Debug.h"
#include "Control.h"
#include "Version.h"

Control control;

void setup()
{
  // Serial debug is gated by DEBUG_SERIAL in Debug.h. In a production build
  // (DEBUG_SERIAL 0) this and all DBG_* prints compile to nothing, so there is
  // no serial output and no USB-CDC TX blocking.
  DBG_BEGIN();

  DBG_PRINTF("\n\nSmartSwitch-D2\n");
  DBG_PRINTF("Software version: %s\n\n", SOFTWARE_VERSION);

  // Note: OTA runs in a SEPARATE updater image (ota_1). When an update is requested
  // the main app switches the boot partition to the updater and reboots (see
  // OtaManager::requestOta), so there is no OTA branch here.
  control.initialize();
}

void loop()
{
  control.loop();
}
