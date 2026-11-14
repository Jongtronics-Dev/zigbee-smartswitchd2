#include "Debug.h"
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_STDIO
#include "minimp3.h"
#include "Audio.h"

Audio::Audio() : playing(false), stopping(false), next_file(0)
{
  i2sOutput = new I2sOutput(I2S_NUM_0);
}

void Audio::initialize()
{
  spiffs = new SPIFFS("/fs");
  i2sOutput->initialize();

  auto task = [](void* arg) { static_cast<Audio*>(arg)->play_task(); };
  xTaskCreate(task, "play_audio", 32768, this, AUDIO_TASK_PRIO_IDLE, &play_task_handle);
}

void Audio::play(uint8_t file_index, uint8_t volume)
{
  if (file_index >= NUM_OF_FILES)
  {
    DBG_PRINTF("Invalid file index %d\n", file_index);
    return;
  }
  
  next_file = file_index;
  next_volume = volume;
  // Mark busy immediately (synchronously) so isBusy() is reliable the instant play()
  // returns — the audio task clears it once it finishes and nothing else is queued.
  playing = true;

  // Boost the audio task above the Zigbee stack so the sound starts immediately and
  // isn't preempted by a concurrent Zigbee TX on this single-core chip. play_task()
  // drops back to AUDIO_TASK_PRIO_IDLE as soon as the first samples are playing.
  if (play_task_handle) {
    vTaskPrioritySet(play_task_handle, AUDIO_TASK_PRIO_START);
  }
}

void Audio::stop()
{
  next_file = 0;
  stopping = true;
}

// private

void Audio::play_task()
{
  while (1)
  {
    vTaskDelay(pdMS_TO_TICKS(1));
    // Idle: nothing queued and nothing playing -> clear the busy flag so isBusy() is false.
    if (next_file == 0) { playing = false; continue; }

    FILE *fp = open_file(MP3_FILES[next_file]);
    next_file = 0;

    if (!fp) {
      // Nothing to play; give the CPU back to Zigbee.
      vTaskPrioritySet(NULL, AUDIO_TASK_PRIO_IDLE);
      continue;
    }

    i2sOutput->set_volume(next_volume);
    
    // mp3 decoder state
    mp3dec_t mp3d = {};
    mp3dec_init(&mp3d);
    mp3dec_frame_info_t info = {};
    // keep track of how much data we have buffered, need to read and decoded
    int to_read = BUFFER_SIZE;
    int buffered = 0;
    int decoded = 0;

    pcm = (short*)malloc(sizeof(short) * MINIMP3_MAX_SAMPLES_PER_FRAME);
    if (!pcm)
    {
      DBG_PRINTLN("Failed to allocate pcm memory");
      break;
    }
    
    input_buf = (uint8_t*)malloc(BUFFER_SIZE);
    if (!input_buf)
    {
      DBG_PRINTLN("Failed to allocate input_buf memory");
      break;
    } 
    
    stopping = false;
    bool boosted = true; // task is currently at AUDIO_TASK_PRIO_START (set in play())
    while (!stopping)
    {
      // read in the data that is needed to top up the buffer
      size_t n = fread(input_buf + buffered, 1, to_read, fp);
      buffered += n;

      // feed the watchdog
      vTaskDelay(pdMS_TO_TICKS(1));

      if (buffered == 0)
      {
        break;
      }

      // decode the next frame
      int samples = mp3dec_decode_frame(&mp3d, input_buf, buffered, pcm, &info);
      // we've processed this may bytes from teh buffered data
      buffered -= info.frame_bytes;
      // shift the remaining data to the front of the buffer
      memmove(input_buf, input_buf + info.frame_bytes, buffered);
      // we need to top up the buffer from the file
      to_read = info.frame_bytes;
      if (samples > 0)
      {
        // if we haven't started the output yet we can do it now as we now know the sample rate and number of channels
        if (decoded == 0)
        {
          i2sOutput->start((uint32_t)info.hz);
        }
        // if we've decoded a frame of mono samples convert it to stereo by duplicating the left channel
        // we can do this in place as our samples buffer has enough space
        if (info.channels == 1)
        {
          // Sample i becomes frame i: left at 2i, right at 2i + 1. Back to front, so no sample
          // is overwritten before it is copied. (This used to write the right channel to 2i - 1:
          // every right sample one step early, and pcm[-1] - in front of the buffer - written.)
          for (int i = samples - 1; i >= 0; i--)
          {
            short s = pcm[i];
            pcm[i * 2] = s;
            pcm[i * 2 + 1] = s;
          }
        }
        // write the decoded samples to the I2S output
        i2sOutput->write(pcm, samples);
        // keep track of how many samples we've decoded
        decoded += samples;

        // The sound is now audible; drop back below Zigbee so the rest of the
        // decode/playback doesn't starve the stack. The I2S DMA keeps playing.
        if (boosted)
        {
          vTaskPrioritySet(NULL, AUDIO_TASK_PRIO_IDLE);
          boosted = false;
        }
      }
    }

    // Safety: ensure we never leave the loop still boosted (e.g. a file that
    // produced no samples), which would keep starving Zigbee.
    if (boosted)
    {
      vTaskPrioritySet(NULL, AUDIO_TASK_PRIO_IDLE);
    }

    // free(pcm);
    // free(input_buf);
    fclose(fp);
  }
}

FILE* Audio::open_file(const char* filename)
{
  FILE *fp = fopen(filename, "r");
  if (!fp)
  {
    DBG_PRINTF("Failed to open file %s\n", filename);
    return NULL;
  }

  DBG_PRINTF("Opened file %s\n", filename);
  return fp;
}
