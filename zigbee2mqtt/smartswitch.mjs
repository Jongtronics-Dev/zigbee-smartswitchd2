import { command_toggle, temperature, humidity, occupancy } from 'zigbee-herdsman-converters/converters/fromZigbee';
import { light_onoff_brightness, light_color } from 'zigbee-herdsman-converters/converters/toZigbee';
import { presets as e, access as ea } from 'zigbee-herdsman-converters/lib/exposes';
import { bind, temperature as reportTemperature, humidity as reportHumidity, occupancy as reportOccupancy } from 'zigbee-herdsman-converters/lib/reporting';
import { deviceAddCustomCluster } from 'zigbee-herdsman-converters/lib/modernExtend';

// Endpoint layout — MUST match the EP_* defines in ZigbeeManager.h
//   1 light      : on/off + level + colour (LED)
//   2 switch     : on/off client (button -> action)
//   3 sensor     : temperature + humidity
//   4 occupancy  : occupancy sensing
//   5 audio      : analog output (genAnalogOutput) -> MP3 file index
//   6 ota        : analog output (genAnalogOutput) -> OTA trigger
//   7 radar      : custom cluster ssd2Radar (0xFC10) -> LD2410 radar config + live data

// LD2410 radar cluster — MUST match ZigbeeRadar.h (attribute ids) and Radar.cpp (live layout).
// Data types: 0x10 boolean, 0x20 uint8, 0x21 uint16, 0x41 octet string, 0x42 char string.
const RADAR_EP = 7;
const RADAR_GATES = 9;
const RADAR_ATTRIBUTES = {
  movingSensitivity: { ID: 0x0000, type: 0x41, write: true },
  staticSensitivity: { ID: 0x0001, type: 0x41, write: true },
  maxMovingGate: { ID: 0x0002, type: 0x20, write: true },
  maxStaticGate: { ID: 0x0003, type: 0x20, write: true },
  noOneDuration: { ID: 0x0004, type: 0x21, write: true },
  resolution: { ID: 0x0005, type: 0x20, write: true },
  bluetooth: { ID: 0x0006, type: 0x10, write: true },
  live: { ID: 0x0007, type: 0x10, write: true },
  liveData: { ID: 0x0008, type: 0x41 },
  firmware: { ID: 0x0009, type: 0x42 },
  command: { ID: 0x000a, type: 0x20, write: true },
};
const RADAR_CLUSTER = {
  name: 'ssd2Radar',
  ID: 0xfc10,
  attributes: Object.fromEntries(Object.entries(RADAR_ATTRIBUTES).map(([name, a]) => [name, { name, ...a }])),
  commands: {},
  commandsResponse: {},
};
const RADAR_RESOLUTIONS = ['0.75m', '0.2m'];
const RADAR_COMMANDS = { restart: 1, factory_reset: 2, refresh: 3 };
// Config attributes, read on demand and after configure.
const RADAR_READ = ['movingSensitivity', 'staticSensitivity', 'maxMovingGate', 'maxStaticGate', 'noOneDuration',
  'resolution', 'bluetooth', 'live', 'firmware'];

const clampInt = (value, min, max) => Math.min(max, Math.max(min, Math.round(Number(value))));

function radarEndpoint(meta) {
  const endpoint = meta.device.getEndpoint(RADAR_EP);
  if (!endpoint) throw new Error('SmartSwitchD2 has no radar endpoint 7 yet: update the firmware and re-interview the device');
  return endpoint;
}

// Live data: see Radar::reportLive() in the firmware.
function decodeRadarLive(buf) {
  const b = Buffer.from(buf);
  if (b.length < 29) return null;
  return {
    target: ['none', 'moving', 'static', 'both'][b[0]] ?? 'none',
    moving_distance: b.readUInt16LE(1),
    moving_energy: b[3],
    static_distance: b.readUInt16LE(4),
    static_energy: b[6],
    detection_distance: b.readUInt16LE(7),
    moving_gates: [...b.subarray(9, 18)],
    static_gates: [...b.subarray(18, 27)],
    light: b[27],
    out: b[28],
  };
}

const fzLocal = {
  SmartSwitchD2_radar: {
    cluster: 'ssd2Radar',
    type: ['attributeReport', 'readResponse'],
    convert: (model, msg, publish, options, meta) => {
      const d = msg.data;
      const result = {};
      if (d.movingSensitivity !== undefined) result.radar_moving_sensitivity = [...Buffer.from(d.movingSensitivity)];
      if (d.staticSensitivity !== undefined) result.radar_static_sensitivity = [...Buffer.from(d.staticSensitivity)];
      if (d.maxMovingGate !== undefined) result.radar_max_moving_gate = d.maxMovingGate;
      if (d.maxStaticGate !== undefined) result.radar_max_static_gate = d.maxStaticGate;
      if (d.noOneDuration !== undefined) result.radar_no_one_duration = d.noOneDuration;
      if (d.resolution !== undefined) result.radar_distance_resolution = RADAR_RESOLUTIONS[d.resolution] ?? RADAR_RESOLUTIONS[0];
      if (d.bluetooth !== undefined) result.radar_bluetooth = !!d.bluetooth;
      if (d.live !== undefined) result.radar_live = !!d.live;
      if (d.liveData !== undefined) result.radar_live_data = decodeRadarLive(d.liveData);
      if (d.firmware !== undefined) {
        // "none" = no radar fitted (detected at boot), "" = fitted but not answering or not measuring.
        const firmware = String(d.firmware).trim();
        result.radar_present = firmware !== 'none';
        result.radar_firmware = firmware === 'none' ? '' : firmware;
      }
      return result;
    },
  },
};

// Custom converter: write the MP3 file index to the analog-output endpoint.
const tzLocal = {
  SmartSwitchD2_play_audio: {
    key: ['file'],
    convertSet: async (entity, key, value, meta) => {
      // entity resolves to the 'audio' endpoint via withEndpoint('audio')
      await entity.write('genAnalogOutput', { presentValue: value });
      return { state: { file: value } };
    },
  },
  // OTA trigger: write 1 (firmware), 2 (sounds) or 3 (both) to the 'ota' endpoint's
  // analog output. The device reboots into its WiFi updater and pulls from ha_server.
  SmartSwitchD2_ota: {
    key: ['update'],
    convertSet: async (entity, key, value, meta) => {
      // entity resolves to the 'ota' endpoint via withEndpoint('ota')
      await entity.write('genAnalogOutput', { presentValue: value });
      return { state: { update: value } };
    },
  },
  // LD2410 radar: always addressed to endpoint 7, whatever the key's postfix resolves to.
  // The device reports the values it actually applied, which then update the state.
  SmartSwitchD2_radar: {
    key: ['radar_moving_sensitivity', 'radar_static_sensitivity', 'radar_max_moving_gate', 'radar_max_static_gate',
      'radar_no_one_duration', 'radar_distance_resolution', 'radar_bluetooth', 'radar_live', 'radar_command'],
    convertSet: async (entity, key, value, meta) => {
      const endpoint = radarEndpoint(meta);
      const gates = (v) => {
        if (!Array.isArray(v) || v.length !== RADAR_GATES) throw new Error(`${key} must be an array of ${RADAR_GATES} values`);
        return Buffer.from(v.map((x) => clampInt(x, 0, 100)));
      };
      let payload;
      switch (key) {
        case 'radar_moving_sensitivity': payload = { movingSensitivity: gates(value) }; break;
        case 'radar_static_sensitivity': payload = { staticSensitivity: gates(value) }; break;
        case 'radar_max_moving_gate': payload = { maxMovingGate: clampInt(value, 2, 8) }; break;
        case 'radar_max_static_gate': payload = { maxStaticGate: clampInt(value, 2, 8) }; break;
        case 'radar_no_one_duration': payload = { noOneDuration: clampInt(value, 0, 65535) }; break;
        case 'radar_distance_resolution': {
          const index = RADAR_RESOLUTIONS.indexOf(value);
          if (index < 0) throw new Error(`radar_distance_resolution must be one of ${RADAR_RESOLUTIONS.join(', ')}`);
          payload = { resolution: index };
          break;
        }
        case 'radar_bluetooth': payload = { bluetooth: value ? 1 : 0 }; break;
        case 'radar_live': payload = { live: value ? 1 : 0 }; break;
        case 'radar_command': {
          if (!(value in RADAR_COMMANDS)) throw new Error(`radar_command must be one of ${Object.keys(RADAR_COMMANDS).join(', ')}`);
          payload = { command: RADAR_COMMANDS[value] };
          break;
        }
      }
      await endpoint.write('ssd2Radar', payload);
    },
    convertGet: async (entity, key, meta) => {
      const endpoint = radarEndpoint(meta);
      await endpoint.read('ssd2Radar', RADAR_READ.slice(0, 6));
      await endpoint.read('ssd2Radar', RADAR_READ.slice(6));
    },
  },
};

/** @type{import('zigbee-herdsman-converters/lib/types').DefinitionWithExtend | import('zigbee-herdsman-converters/lib/types').DefinitionWithExtend[]} */
export default {
  zigbeeModel: ['SmartSwitchD2'],
  model: 'SmartSwitchD2',
  vendor: 'Jongtronics',
  description: 'SmartSwitch Digital v2',
  icon: 'data:image/jpeg;base64,/9j//gAQTGF2YzYwLjE1LjEwMAD/2wBDAAgKCgsKCw0NDQ0NDRAPEBAQEBAQEBAQEBASEhIVFRUSEhIQEBISFBQVFRcXFxUVFRUXFxkZGR4eHBwjIyQrKzP/xACHAAEAAgIDAQAAAAAAAAAAAAAAAgQDCAEHBQYBAQAAAAAAAAAAAAAAAAAAAAAQAAIBAQMFCQ0FBgcBAQAAAAABAgMSBBFxE1EGBUGBkTHB0RSxIdKhgpJiYVRTUjMHQzIVskLCcuIks4PT4WN0okQ18JNkFhEBAAAAAAAAAAAAAAAAAAAAAP/AABEIAMgAyAMBEgACEgADEgD/2gAMAwEAAhEDEQA/AO/gAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAPj9b77etn7GrV7rPN1FOlG0km1Gc0nhjxPDd4wA+hq7QuVCebqXmjCS44ynFNZVj2Gq/21fd105Pdxhx98ANqFtG4vivV3/8AWHOas/bl8W5RfgPugA2qV8uj4rxQf8yHdGrC27efVUPFl3QAbVK8UHxVab8OPOasfb1bdoUH43OAG1aqU3xTi/CXOarLWCp6PS8aQAbWYp7q4TVhaxS9HjvVJdyAG1Bq4tZJeolvVn3IAbRmsUdZ2vl11krsANnTWha1yXpayV3zgBsua3LW6a+Zfl/N/bADZE10WuM18+/rw8fzgBsWa9LXOfpV97z5QA2FNf1rpP0y9b9NPkADYA6FWus/Tq2/RXcAB30dFLXap6dLfor+mAHep0gtd6vp0N+iv6YAd3nS0Nc7xUajTvdCUpdkY5pYt7i4kAHdJgu85VKFKc8LUqcJSw4sXFN4b4AZwAAAAAAAAAAAAAfF66RtavX7yVSlwVoF3WmGc2DtFf8Azyl4uEuQANVSTADGSYAQK9StCDwtLhADOUekw0oALuJSz8NKAC6VM/HSAFwqZ6OkAPVutBXmrYcrCwxbwx7McOU8+F4dOSlCbi1up9oAX71Q6NWlTtWksGnhhimUJXh1JOU5OTe6wAylfOIAM5htoAMxhtoAMpitoAMxjUkAGQAAOQA9DZkW7/dsPWJ8GLLmw4W9p3ZeU/usANtaKwpU1ohFd5GRdgAcgAAAAAAAAAAAAA8bbcM5snaEdN1vH8ORdvsc5dLxD2qNWPDBoANPTncWRABjZzIAMKjGH4KbyxT75Nrk6gAxOMH8ql4uHUyYAY7FH1NP/V3RMAMWaoepj40ucygBgzF39Thkm+5MwAYOj3b2J70/2TOAFbo130VV4S5kWQAq9Fo+1WXA/wAxZACp0Wn6yr4q7otgBU6LH10/F/uW8AApdG0V+GMuYuABSnRlSi5KtGWHbhg8e+kZqvu5/pYASpScop6Ujij9EciACyjlAB9TqxC3te7LzvkXKX9TYW9tUfNh9+IAbOgAAAAAAAAAAAAAAAIyVqLWlNcJIANN6kbMpR0NrgeBc2hDN3y9Q9mvWXBUkAHmM5YAQfN1EnuZF1ABjwOQAiSACBIAIkgAicgBEkAESQAROQA4JABAkAFWv7qeTlF59zLe6wA5pfSsiJU+JABYRygA7D1Gha2vF6EuvHkL3w/p47SnLRFfdmAGwQAAAAAAAAAAAAAAAAAA1R29CxtfaEdF6r9+bZc1ojY27tBf41rxoxfKAHyrRJgBjw4siJvcyIAMeBIAIYEgAhgSACJyAESQARJABAkAEcCQARJABHAkAFK9+5eWPWL57pfriAE6ZzAALKCADt34eQ/ea8tC/L/cu/DqPbeZZeqAAdzgAAAAAAAAAAAAAAAAADWrXOFjWC+eUqMuGjDmLevcbO3Zv2qFB96UeQAPgGcMAOdxZDncWTlYAROQAjgSACGBIAIYEgAhgSACJIAIEgAicgBwcgBwcgB598+iH60c3z5S8sAMkDmAAWUEAHefw8hhQvEtMn1x5i58PoYbPqS0y/NLmADtAAAAAAAAAAAAAAAAAAAdBfEKFna1CXtXWHeqVEXPiLD99uM9NCovFqJ8oAdSsMAMi+lb/WI/St8AOQAHBwAAABwAA4AADkAInIARJABE5ADg5ADzb59VH9TF795QyyADLAlAALCOUAGxGoccNlY6XyyfKW9So2djUvPh91PlAD7oAAAAAAAAAAAAAAAAAAdOfEaP/HT/AMxH+Gy38RY/utxn7NaovGp48gAdH4EWwAmsMN9mByADNiVGwAtlDEAL5QtPS+EALpSty0sALpUzktPUAFwqZ2XmAC2VlWehABZMGd83fADOYc4vOAGYx24/9QAefeu2tRXmkWJ07VWM8eyMWsPOwAlFGQAJgANndUo2djXfIvuxLercbGybsvJ/sAH0gAAAAAAAAAAAAAAAAAA+Q1p2N9s3GNJVHTnSqZ2GEVK07LjZeMo4Lt48T6ucbSADV2vqrtqnJ2bs5rTGUOq0bLSop7gAao1di7WpfVcrxvQb6sTaZ3aOgANRp3W90/ru9eOWnNchtXWuEanY+IANSW5LjjJbzNoKmw6EvwxeVIANWs4jZCrqvdanHSpvwVzABrfnEd9VdTbnL5MN5YdQAdC20dzVNR7u+Km1kkwA6atnadTUWP4XUW/jyAB1dbPv6mpNVfTVnvpMAPgbZ9hPU6+x4qi34vkAD5FSPoZ6r7ThxWJcK6wA8FSPZ/8Aze2F/t3L9MovlADyVIvz2PtWl9VzvG9BvqxACpiSldL3T+u71o5aclyAByu0y3ahWr1YQhTnKTlHBKLx4wA2v2JGzsy6ryPzMtXGm6F2o0vYhFb+Hb3wA9AAAAAAAAAAAAAAAAAAAAAGNoyABhwMuAAYLJnwACrYLOAAVLBbwAClmi7gAFHNF3AAKGZL+AAedmFoR6NkAPLd2i/wo9SyAHj9Cp+yj2LIAeR0On7KPXsgB5XQ6eg9bAAPL6HDQergAHnwuVBNSzcMVxOyseo9EAC7AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAf/2Q==',

  // Registers the ssd2Radar cluster with zigbee-herdsman so its attributes are typed.
  extend: [deviceAddCustomCluster('ssd2Radar', RADAR_CLUSTER)],

  // Needed because features now live on separate endpoints.
  meta: { multiEndpoint: true },

  // Map friendly names -> endpoint numbers.
  endpoint: (device) => {
    return { light: 1, switch: 2, sensor: 3, occupancy: 4, audio: 5, ota: 6, radar: RADAR_EP };
  },

  // What the device sends to us
  fromZigbee: [command_toggle, temperature, humidity, occupancy, fzLocal.SmartSwitchD2_radar],

  // What we can send to the device
  toZigbee: [light_onoff_brightness, light_color, tzLocal.SmartSwitchD2_play_audio, tzLocal.SmartSwitchD2_ota, tzLocal.SmartSwitchD2_radar],

  // What features are exposed to Home Assistant/Frontend
  exposes: [
    e.light_brightness_colorxy().withEndpoint('light'),
    e.temperature().withEndpoint('sensor'),
    e.humidity().withEndpoint('sensor'),
    e.occupancy().withEndpoint('occupancy'),
    // For multiEndpoint devices z2m postfixes the action with the endpoint name,
    // so the button on endpoint 'switch' emits 'toggle_switch'. Declare the
    // actual value here so the exposed action matches what the device sends.
    e.action(['toggle_switch']),
    e.numeric('file', ea.SET).withValueMin(0).withValueMax(99).withDescription('MP3 file').withEndpoint('audio'),
    e.numeric('update', ea.SET).withValueMin(0).withValueMax(3)
      .withDescription('OTA update: 1=firmware, 2=sounds, 3=both').withEndpoint('ota'),
    // LD2410 radar (endpoint 7). Gate n covers n x 0.75 m (or 0.2 m) from the switch.
    e.list('radar_moving_sensitivity', ea.ALL, e.numeric('threshold', ea.STATE_SET).withValueMin(0).withValueMax(100))
      .withDescription('Radar moving-target threshold per gate 0-8 (lower = more sensitive)'),
    e.list('radar_static_sensitivity', ea.ALL, e.numeric('threshold', ea.STATE_SET).withValueMin(0).withValueMax(100))
      .withDescription('Radar static-target threshold per gate 0-8 (lower = more sensitive)'),
    e.numeric('radar_max_moving_gate', ea.ALL).withValueMin(2).withValueMax(8).withDescription('Furthest gate for moving targets'),
    e.numeric('radar_max_static_gate', ea.ALL).withValueMin(2).withValueMax(8).withDescription('Furthest gate for static targets'),
    e.numeric('radar_no_one_duration', ea.ALL).withValueMin(0).withValueMax(65535).withUnit('s')
      .withDescription('Time the radar keeps reporting presence after the last target'),
    e.enum('radar_distance_resolution', ea.ALL, RADAR_RESOLUTIONS).withDescription('Gate size (the radar restarts)'),
    e.binary('radar_bluetooth', ea.ALL, true, false).withDescription('Radar Bluetooth for HLKRadarTool (turns off after 15 min)'),
    e.binary('radar_live', ea.ALL, true, false).withDescription('Stream live gate energies (turns off after 5 min)'),
    e.enum('radar_command', ea.SET, Object.keys(RADAR_COMMANDS)).withDescription('Radar action'),
    e.text('radar_firmware', ea.STATE).withDescription('Radar firmware version'),
    e.binary('radar_present', ea.STATE, true, false).withDescription('An LD2410 radar is fitted (detected at boot)'),
  ],

  // Configuration to ensure the device reports status changes
  configure: async (device, coordinatorEndpoint, definition) => {
    const switchEp = device.getEndpoint(2);
    const sensorEp = device.getEndpoint(3);
    const occupancyEp = device.getEndpoint(4);

    // Bind so the button's on/off command reaches the coordinator as an action.
    await bind(switchEp, coordinatorEndpoint, ['genOnOff']);

    // Bind + report the measurement clusters on their own endpoints.
    await bind(sensorEp, coordinatorEndpoint, ['msTemperatureMeasurement', 'msRelativeHumidity']);
    await bind(occupancyEp, coordinatorEndpoint, ['msOccupancySensing']);

    // Reporting config applied on interview (fresh pair). Overrides z2m's generic defaults
    // (temp: max 1h / change 1C, humidity: max 1h) with our values so a freshly paired switch
    // already reports sensibly. Keep these in sync with LocationSensors (ha_server), which is
    // the authoritative source and re-pushes on restart / reconnect / location change:
    //   temperature: min 10s, max 30min, change 0.3C  (measuredValue 0.01 units -> 30)
    //   humidity:    min 10s, max 30min, change 0.5%  (measuredValue 0.01 units -> 50)
    //   occupancy:   max 30min
    await reportTemperature(sensorEp, { min: 10, max: 1800, change: 30 });
    await reportHumidity(sensorEp, { min: 10, max: 1800, change: 50 });
    await reportOccupancy(occupancyEp, { max: 1800 });

    // The radar reports to the coordinator by itself; just read the current state once.
    // Split in two reads to stay within one ZCL frame.
    const radarEp = device.getEndpoint(RADAR_EP);
    if (radarEp) {
      await radarEp.read('ssd2Radar', RADAR_READ.slice(0, 6));
      await radarEp.read('ssd2Radar', RADAR_READ.slice(6));
    }
  },
};
