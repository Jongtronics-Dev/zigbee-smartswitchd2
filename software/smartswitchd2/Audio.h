#pragma once

#include "I2sOutput.h"
#include "SPIFFS.h"
#include <Arduino.h>

#define BUFFER_SIZE 4096
#define NUM_OF_FILES 29

// Play index of the "update being installed" sound (see MP3_FILES below). Played once,
// in full, right before the device reboots into the WiFi updater for an OTA.
#define AUDIO_UPDATE_INSTALLING 26

// The ESP32-C6 is single-core, so the audio task shares the CPU with the Zigbee
// stack task (priority 5). At the normal playback priority (below Zigbee) the sound
// start gets preempted by a button-press Zigbee TX, so the lamp can react before the
// local click is heard. We briefly boost the audio task ABOVE Zigbee to get the first
// samples out instantly, then drop back so Zigbee isn't starved during playback.
#define AUDIO_TASK_PRIO_IDLE  2   // normal playback priority (below Zigbee = 5)
#define AUDIO_TASK_PRIO_START 8   // brief boost above Zigbee to start the sound now

class Audio {
private:
  FILE* open_file(const char* filename);
  void play_task();

  I2sOutput* i2sOutput;
  SPIFFS* spiffs;

  TaskHandle_t play_task_handle = nullptr;

  volatile bool playing, stopping;
  uint8_t next_file, next_volume;

  short* pcm;
  uint8_t* input_buf;

  // MP3 file table, addressed directly by the play index. Index 0 = STOP (never
  // opened as a file). Sounds are packed 1..27 in alphabetical order, matching the
  // SOUNDS list in the front-end (DevicePanelSmartSwitch.jsx) — keep the two in sync,
  // and in sync with the files in data/ (spiffs.bin).
  const char* MP3_FILES[NUM_OF_FILES] = {
    "STOPPING",
    "/fs/airhorn1.mp3",
    "/fs/airhorn2.mp3",
    "/fs/baby_laughing.mp3",
    "/fs/bye_have_a_great_time.mp3",
    "/fs/clap_and_cheering.mp3",
    "/fs/click.mp3",
    "/fs/click_voice.mp3",
    "/fs/fart1.mp3",
    "/fs/fart2.mp3",
    "/fs/fart3.mp3",
    "/fs/hello_there.mp3",
    "/fs/kids_cheering.mp3",
    "/fs/la_ciguena.mp3",
    "/fs/mario_failed.mp3",
    "/fs/mario_jump.mp3",
    "/fs/mario_star_song.mp3",
    "/fs/mario_yahoo.mp3",
    "/fs/notification1.mp3",
    "/fs/notification2.mp3",
    "/fs/notification3.mp3",
    "/fs/oh_no_no_laugh.mp3",
    "/fs/rick_roll.mp3",
    "/fs/sad_horn.mp3",
    "/fs/slaap_kindje_slaap.mp3",
    "/fs/slow_clap.mp3",
    "/fs/update_being_installed.mp3",
    "/fs/wakker_worden.mp3",
    "/fs/welcome_home.mp3"
  };
public:
  Audio();
  void initialize();

  void play(uint8_t file_index, uint8_t volume);
  void stop();

  // True while a sound is queued or actively playing. Used to hold off the OTA reboot
  // until the "update being installed" sound has finished (see Control::loop).
  bool isBusy() const { return playing; }
};
