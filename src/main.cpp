#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <HTTPClient.h>
#include <M5Unified.h>
#include <Sonos.h>
#include <WiFi.h>

#include "config.h"
#include "device_settings.h"
#include "wifi_credentials.h"

namespace {
constexpr uint32_t kBackground = 0x101820;
constexpr uint32_t kPanel = 0x1D2A35;
constexpr uint32_t kPlay = 0x20A060;
constexpr uint32_t kPause = 0xD08020;
constexpr uint32_t kGroup = 0x2878B8;
constexpr uint32_t kText = 0xFFFFFF;
constexpr uint32_t kMutedText = 0xB8C4CC;
constexpr uint32_t kError = 0xE05050;

constexpr int kMediaButtonY = 120;
constexpr int kMediaButtonHeight = 54;
constexpr int kGroupButtonY = 182;
constexpr int kGroupButtonHeight = 49;
constexpr int kButtonGap = 8;
constexpr int kButtonWidth = (320 - (kButtonGap * 3)) / 2;
constexpr uint8_t kDisplayBrightness = 140;
constexpr uint32_t kDisplayIdleTimeoutMs = 15000;
constexpr uint32_t kFaderSampleIntervalMs = 25;
constexpr uint32_t kFaderWarmupMs = 1500;
constexpr uint32_t kVolumeSendIntervalMs = 250;
constexpr uint32_t kFaderLedHoldMs = 700;
constexpr int kFaderActivationDelta = 100;
constexpr int kFaderLedMotionDelta = 24;
constexpr int kVolumeChangeThreshold = 2;
constexpr uint8_t kFaderLedBrightness = 160;
constexpr int kFaderLedLevels = FADER_LED_COUNT / 2;
static_assert(FADER_LED_COUNT == 14, "Fader position map expects 14 LEDs");

struct LedRgb {
  uint8_t red;
  uint8_t green;
  uint8_t blue;
};

// Seven saturated steps stay distinguishable through the fader's diffuser:
// cool colors mean low volume, warm colors mean high volume.
constexpr LedRgb kFaderScaleColors[kFaderLedLevels] = {
    {0, 2, 24},   // deep blue: minimum
    {0, 9, 32},   // blue/cyan
    {0, 25, 29},  // cyan
    {0, 34, 11},  // green: middle
    {32, 29, 0},  // yellow
    {50, 16, 0},  // amber
    {62, 2, 0},   // red: maximum
};

Sonos sonos;
String selectedIp;
String selectedRoom;
bool ready = false;
bool touchArmed = true;
bool displayAwake = true;
bool faderCalibrated = false;
bool faderControlsSonos = false;
int filteredFaderRaw = -1;
int faderBaselineRaw = -1;
int lastSentVolume = -1;
int lastAttemptedVolume = -1;
uint32_t lastFaderSampleAt = 0;
uint32_t faderWarmupStartedAt = 0;
uint32_t lastVolumeSentAt = 0;
uint32_t lastFaderLedMotionAt = 0;
uint32_t lastDisplayActivityAt = 0;
int lastFaderLedLevel = -1;
int lastFaderLedMotionRaw = -1;
bool faderLedsActive = false;
Adafruit_NeoPixel faderPixels(FADER_LED_COUNT, FADER_RGB_PIN,
                              NEO_GRB + NEO_KHZ800);

void drawStatus(const String& line1, const String& line2 = "",
                uint32_t color = kMutedText) {
  M5.Display.fillRect(0, 62, 320, 55, kBackground);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(color, kBackground);
  M5.Display.drawString(line1, 160, 77);
  M5.Display.setTextColor(kMutedText, kBackground);
  M5.Display.drawString(line2, 160, 99);
}

void drawMediaButton(int x, uint32_t color, const char* label,
                     const char* symbol) {
  M5.Display.fillRoundRect(x, kMediaButtonY, kButtonWidth,
                           kMediaButtonHeight, 10, color);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextColor(kText, color);
  M5.Display.setTextSize(2);
  M5.Display.drawString(String(symbol) + "  " + label, x + (kButtonWidth / 2),
                        kMediaButtonY + (kMediaButtonHeight / 2));
}

void drawGroupButton() {
  M5.Display.fillRoundRect(kButtonGap, kGroupButtonY, 320 - (kButtonGap * 2),
                           kGroupButtonHeight, 10, kGroup);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextColor(kText, kGroup);
  M5.Display.setTextSize(2);
  M5.Display.drawString("GROUP ALL", 160,
                        kGroupButtonY + (kGroupButtonHeight / 2));
}

void drawInterface() {
  M5.Display.fillScreen(kBackground);
  M5.Display.fillRect(0, 0, 320, 54, kPanel);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextColor(kText, kPanel);
  M5.Display.setTextSize(2);
  M5.Display.drawString("CoreS3 Sonos", 160, 27);

  drawMediaButton(kButtonGap, kPlay, "PLAY", ">");
  drawMediaButton((kButtonGap * 2) + kButtonWidth, kPause, "PAUSE", "||");
  drawGroupButton();
}

void noteDisplayActivity() {
  lastDisplayActivityAt = millis();
  if (displayAwake) return;

  M5.Display.wakeup();
  displayAwake = true;
  Serial.println("Display awake");
}

void handleDisplayIdleTimeout() {
  if (!displayAwake ||
      millis() - lastDisplayActivityAt < kDisplayIdleTimeoutMs) {
    return;
  }

  M5.Display.sleep();
  displayAwake = false;
  touchArmed = false;
  Serial.println("Display asleep after idle timeout");
}

void fail(const String& message) {
  ready = false;
  Serial.println(message);
  drawStatus("Not ready", message, kError);
}

bool connectWifi() {
  if (String(WIFI_SSID) == "YOUR_WIFI_NAME" ||
      String(WIFI_PASSWORD) == "YOUR_WIFI_PASSWORD") {
    fail("Edit wifi_credentials.h");
    return false;
  }

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  drawStatus("Connecting to Wi-Fi...", WIFI_SSID);

  const uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < 25000) {
    delay(200);
  }

  if (WiFi.status() != WL_CONNECTED) {
    fail("Wi-Fi timeout; check credentials");
    return false;
  }

  Serial.printf("Wi-Fi connected: %s\n", WiFi.localIP().toString().c_str());
  drawStatus("Wi-Fi connected", WiFi.localIP().toString());
  return true;
}

bool findSonosCoordinator() {
  SonosConfig config;
  config.discoveryTimeoutMs = 7000;
  config.soapTimeoutMs = 5000;
  config.maxRetries = 2;
  config.enableLogging = true;
  sonos.setConfig(config);
  sonos.setLogCallback([](const String& message) {
    Serial.println("[Sonos] " + message);
  });

  auto result = sonos.begin();
  if (result != SonosResult::SUCCESS) {
    fail("Sonos init: " + sonos.getErrorString(result));
    return false;
  }

  drawStatus("Finding Sonos players...", "This takes about 7 seconds");
  result = sonos.discoverDevices();
  if (result != SonosResult::SUCCESS) {
    fail("Discovery: " + sonos.getErrorString(result));
    return false;
  }

  const auto devices = sonos.getDiscoveredDevices();
  Serial.printf("Discovered %u Sonos player(s)\n",
                static_cast<unsigned>(devices.size()));
  for (const auto& device : devices) {
    Serial.printf("  %s - %s\n", device.name.c_str(), device.ip.c_str());
  }

  if (devices.empty()) {
    fail("No Sonos found on this LAN");
    return false;
  }

  if (strlen(SONOS_COORDINATOR_ROOM) > 0) {
    SonosDevice* requested = sonos.getDeviceByName(SONOS_COORDINATOR_ROOM);
    if (requested == nullptr) {
      fail("Room not found; check settings");
      return false;
    }
    selectedIp = requested->ip;
    selectedRoom = requested->name;
  } else {
    selectedIp = devices.front().ip;
    selectedRoom = devices.front().name;
  }

  ready = true;
  Serial.printf("Selected coordinator: %s (%s)\n", selectedRoom.c_str(),
                selectedIp.c_str());
  drawStatus("Ready: " + selectedRoom, selectedIp, kPlay);
  return true;
}

void runCommand(bool play) {
  if (!ready) {
    drawStatus("Not ready", "Restart after fixing config", kError);
    return;
  }

  drawStatus(play ? "Sending PLAY..." : "Sending PAUSE...", selectedRoom);
  const SonosResult result = play ? sonos.play(selectedIp) : sonos.pause(selectedIp);
  if (result == SonosResult::SUCCESS) {
    drawStatus(play ? "Playing" : "Paused", selectedRoom,
               play ? kPlay : kPause);
    Serial.printf("%s succeeded for %s\n", play ? "Play" : "Pause",
                  selectedRoom.c_str());
  } else {
    const String error = sonos.getErrorString(result);
    drawStatus(play ? "Play failed" : "Pause failed", error, kError);
    Serial.println(error);
  }
}

String extractXmlValue(const String& xml, const String& tag) {
  const String startTag = "<" + tag + ">";
  const String endTag = "</" + tag + ">";
  const int start = xml.indexOf(startTag);
  if (start < 0) return "";
  const int valueStart = start + startTag.length();
  const int end = xml.indexOf(endTag, valueStart);
  return end < 0 ? "" : xml.substring(valueStart, end);
}

String getPlayerUuid(const String& ip) {
  HTTPClient http;
  http.setTimeout(5000);
  http.setReuse(false);
  if (!http.begin("http://" + ip + ":1400/xml/device_description.xml")) {
    return "";
  }

  const int status = http.GET();
  if (status != HTTP_CODE_OK) {
    Serial.printf("UUID lookup failed for %s: HTTP %d\n", ip.c_str(), status);
    http.end();
    return "";
  }

  String uuid = extractXmlValue(http.getString(), "UDN");
  http.end();
  if (uuid.startsWith("uuid:")) uuid.remove(0, 5);
  return uuid;
}

bool joinPlayerToCoordinator(const String& playerIp,
                             const String& coordinatorUuid) {
  const String body =
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
      "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
      "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
      "<s:Body>"
      "<u:SetAVTransportURI "
      "xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
      "<InstanceID>0</InstanceID><CurrentURI>x-rincon:" +
      coordinatorUuid +
      "</CurrentURI><CurrentURIMetaData></CurrentURIMetaData>"
      "</u:SetAVTransportURI></s:Body></s:Envelope>";

  HTTPClient http;
  http.setTimeout(5000);
  http.setReuse(false);
  const String url =
      "http://" + playerIp + ":1400/MediaRenderer/AVTransport/Control";
  if (!http.begin(url)) return false;
  http.addHeader("Content-Type", "text/xml; charset=utf-8");
  http.addHeader(
      "SOAPAction",
      "\"urn:schemas-upnp-org:service:AVTransport:1#SetAVTransportURI\"");
  const int status = http.POST(body);
  http.end();
  Serial.printf("Group request to %s: HTTP %d\n", playerIp.c_str(), status);
  return status == HTTP_CODE_OK;
}

int faderRawToPercent(int raw) {
  raw = constrain(raw, FADER_RAW_MIN, FADER_RAW_MAX);
  int percent =
      ((raw - FADER_RAW_MIN) * 100) / (FADER_RAW_MAX - FADER_RAW_MIN);
  if (FADER_REVERSED) percent = 100 - percent;
  return constrain(percent, 0, 100);
}

bool setSonosGroupVolume(int volume) {
  volume = constrain(volume, 0, SONOS_MAX_VOLUME);
  const String body =
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
      "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
      "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
      "<s:Body>"
      "<u:SetGroupVolume "
      "xmlns:u=\"urn:schemas-upnp-org:service:GroupRenderingControl:1\">"
      "<InstanceID>0</InstanceID><DesiredVolume>" +
      String(volume) +
      "</DesiredVolume></u:SetGroupVolume>"
      "</s:Body></s:Envelope>";

  HTTPClient http;
  http.setTimeout(5000);
  http.setReuse(false);
  const String url = "http://" + selectedIp +
                     ":1400/MediaRenderer/GroupRenderingControl/Control";
  if (!http.begin(url)) return false;
  http.addHeader("Content-Type", "text/xml; charset=utf-8");
  http.addHeader(
      "SOAPAction",
      "\"urn:schemas-upnp-org:service:GroupRenderingControl:1#"
      "SetGroupVolume\"");
  const int status = http.POST(body);
  http.end();
  Serial.printf("SONOS group volume=%d HTTP=%d\n", volume, status);
  return status == HTTP_CODE_OK;
}

void clearFaderLeds() {
  faderPixels.clear();
  faderPixels.show();
}

int pixelForFaderLevel(int level, bool otherSide) {
  const int physicalLevel =
      FADER_LEDS_REVERSED ? kFaderLedLevels - 1 - level : level;
  return otherSide ? FADER_LED_COUNT - 1 - physicalLevel : physicalLevel;
}

void setFaderLevelPair(int level, uint32_t color) {
  faderPixels.setPixelColor(pixelForFaderLevel(level, false), color);
  faderPixels.setPixelColor(pixelForFaderLevel(level, true), color);
}

uint32_t boostedScaleColor(const LedRgb& scale) {
  const int strongest = max(scale.red, max(scale.green, scale.blue));
  if (strongest == 0) return 0;
  return faderPixels.Color((scale.red * 255) / strongest,
                           (scale.green * 255) / strongest,
                           (scale.blue * 255) / strongest);
}

int faderRawToLedLevel(int raw) {
  raw = constrain(raw, FADER_RAW_MIN, FADER_RAW_MAX);
  if (FADER_REVERSED) {
    raw = FADER_RAW_MAX - (raw - FADER_RAW_MIN);
  }

  // Round to the nearest of the seven real LED positions. With this unit's
  // reversed electrical direction, raw 4050 maps to the bottom pair and raw
  // 80 maps to the top pair.
  return constrain(
      ((raw - FADER_RAW_MIN) * (kFaderLedLevels - 1) +
       ((FADER_RAW_MAX - FADER_RAW_MIN) / 2)) /
          (FADER_RAW_MAX - FADER_RAW_MIN),
      0, kFaderLedLevels - 1);
}

void showFaderScale(int markerLevel) {
  // While moving, all positions show a subdued low-to-high color scale. The
  // current pair keeps its normal hue but is boosted to full intensity.
  faderPixels.clear();
  for (int level = 0; level < kFaderLedLevels; ++level) {
    const LedRgb scale = kFaderScaleColors[level];
    const uint32_t color =
        level == markerLevel ? boostedScaleColor(scale)
                             : faderPixels.Color(scale.red, scale.green,
                                                 scale.blue);
    setFaderLevelPair(level, color);
  }
  faderPixels.show();
}

void noteFaderLedMotion(int raw, int smoothedRaw) {
  if (lastFaderLedMotionRaw < 0) {
    lastFaderLedMotionRaw = smoothedRaw;
    return;
  }

  // The smoothed reading rejects stationary ADC jitter, preventing noise from
  // keeping the lights alive. The unsmoothed reading still places the marker.
  if (abs(smoothedRaw - lastFaderLedMotionRaw) < kFaderLedMotionDelta) return;
  lastFaderLedMotionRaw = smoothedRaw;
  const int level = faderRawToLedLevel(raw);
  if (level != lastFaderLedLevel) {
    Serial.printf("Fader marker raw=%d level=%d pixels=%d,%d\n", raw, level,
                  pixelForFaderLevel(level, false),
                  pixelForFaderLevel(level, true));
  }
  lastFaderLedLevel = level;
  noteDisplayActivity();
  showFaderScale(level);
  faderLedsActive = true;
  lastFaderLedMotionAt = millis();
}

void handleFaderLedTimeout() {
  if (!faderLedsActive ||
      millis() - lastFaderLedMotionAt < kFaderLedHoldMs) {
    return;
  }

  clearFaderLeds();
  faderLedsActive = false;
  lastFaderLedLevel = -1;
  Serial.println("Fader LEDs off after idle timeout");
}

void handleFader() {
  if (millis() - lastFaderSampleAt < kFaderSampleIntervalMs) return;
  lastFaderSampleAt = millis();

  const int raw = analogRead(FADER_ANALOG_PIN);
  if (filteredFaderRaw < 0) {
    filteredFaderRaw = raw;
    faderBaselineRaw = raw;
    faderWarmupStartedAt = millis();
  } else {
    // Light smoothing makes the displayed percentage easier to read while
    // still following hand movement quickly.
    filteredFaderRaw = ((filteredFaderRaw * 3) + raw) / 4;
  }

  const int percent = faderRawToPercent(filteredFaderRaw);
  const int cappedTarget = (percent * SONOS_MAX_VOLUME + 50) / 100;

  if (!faderCalibrated) {
    // Ignore ADC settling at startup. Volume control cannot activate here.
    faderBaselineRaw = filteredFaderRaw;
    if (millis() - faderWarmupStartedAt >= kFaderWarmupMs) {
      faderCalibrated = true;
      Serial.printf("Fader calibrated at raw=%d; waiting for movement\n",
                    faderBaselineRaw);
      drawStatus("Volume control ready",
                 "Move fader  |  max " + String(SONOS_MAX_VOLUME) + "%",
                 kGroup);
    }
  } else if (!faderControlsSonos &&
             abs(filteredFaderRaw - faderBaselineRaw) >=
                 kFaderActivationDelta) {
    faderControlsSonos = true;
    Serial.println("Fader movement detected; Sonos volume enabled");
  }

  // Ignore ADC startup settling, then follow the unsmoothed reading for
  // immediate LED feedback. This remains independent of Wi-Fi/Sonos.
  if (faderCalibrated) noteFaderLedMotion(raw, filteredFaderRaw);

  const bool exactEndpoint =
      cappedTarget == 0 || cappedTarget == SONOS_MAX_VOLUME;
  const bool meaningfulVolumeChange =
      lastAttemptedVolume < 0 ||
      abs(cappedTarget - lastAttemptedVolume) >= kVolumeChangeThreshold ||
      (exactEndpoint && cappedTarget != lastAttemptedVolume);
  if (ready && faderControlsSonos && meaningfulVolumeChange &&
      millis() - lastVolumeSentAt >= kVolumeSendIntervalMs) {
    lastVolumeSentAt = millis();
    lastAttemptedVolume = cappedTarget;
    if (setSonosGroupVolume(cappedTarget)) {
      lastSentVolume = cappedTarget;
    } else {
      drawStatus("Volume failed", "Move fader to retry", kError);
    }
  }
}

void groupAll() {
  if (!ready) {
    drawStatus("Not ready", "Restart after fixing config", kError);
    return;
  }

  drawStatus("Grouping all rooms...", selectedRoom);
  const String coordinatorUuid = getPlayerUuid(selectedIp);
  if (coordinatorUuid.isEmpty()) {
    drawStatus("Group failed", "Coordinator UUID unavailable", kError);
    return;
  }

  const auto devices = sonos.getDiscoveredDevices();
  int joined = 0;
  int failed = 0;
  for (const auto& device : devices) {
    if (device.ip == selectedIp) continue;
    if (joinPlayerToCoordinator(device.ip, coordinatorUuid)) {
      ++joined;
    } else {
      ++failed;
    }
  }

  if (failed == 0) {
    drawStatus("All rooms grouped", String(joined) + " player(s) joined",
               kGroup);
    Serial.printf("Group All succeeded: %d player(s) joined to %s\n", joined,
                  selectedRoom.c_str());
  } else {
    drawStatus("Group partially failed",
               String(joined) + " joined, " + String(failed) + " failed",
               kError);
    Serial.printf("Group All partial result: %d joined, %d failed\n", joined,
                  failed);
  }
}
}  // namespace

void setup() {
  auto cfg = M5.config();
  cfg.output_power = true;
  cfg.internal_imu = false;
  cfg.internal_rtc = false;
  cfg.internal_mic = false;
  cfg.internal_spk = false;
  M5.begin(cfg);
  M5.Display.setRotation(1);
  M5.Display.setBrightness(kDisplayBrightness);

  Serial.begin(115200);
  delay(500);
  Serial.println("\nCoreS3 local Sonos play/pause controller");

  analogReadResolution(12);
  analogSetPinAttenuation(FADER_ANALOG_PIN, ADC_11db);
  faderPixels.begin();
  faderPixels.setBrightness(kFaderLedBrightness);
  clearFaderLeds();

  drawInterface();
  if (connectWifi()) {
    findSonosCoordinator();
    WiFi.setSleep(true);
    Serial.println("Wi-Fi modem sleep enabled");
  }

  setCpuFrequencyMhz(160);
  Serial.printf("CPU frequency: %u MHz\n", getCpuFrequencyMhz());
  lastDisplayActivityAt = millis();
}

void loop() {
  M5.update();
  const bool displayWasAwake = displayAwake;
  handleFaderLedTimeout();
  handleFader();
  const auto touch = M5.Touch.getDetail();

  if (!touch.isPressed()) {
    touchArmed = true;
  } else if (!displayWasAwake) {
    // The first touch only wakes the display; it never presses a UI button.
    touchArmed = false;
    noteDisplayActivity();
  } else {
    noteDisplayActivity();
    if (touchArmed && touch.y >= kMediaButtonY &&
        touch.y < kMediaButtonY + kMediaButtonHeight) {
      touchArmed = false;
      const bool playPressed = touch.x < 160;
      runCommand(playPressed);
    } else if (touchArmed && touch.y >= kGroupButtonY &&
               touch.y < kGroupButtonY + kGroupButtonHeight) {
      touchArmed = false;
      groupAll();
    }
  }

  handleDisplayIdleTimeout();
  delay(10);
}
