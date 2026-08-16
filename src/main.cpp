#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <HTTPClient.h>
#include <M5Unified.h>
#include <Sonos.h>
#include <WiFi.h>
#include <vector>

#include "config.h"
#include "device_settings.h"
#include "wifi_credentials.h"

namespace {
constexpr uint32_t kBackground = 0x101820;
constexpr uint32_t kPanel = 0x1D2A35;
constexpr uint32_t kPlay = 0x20A060;
constexpr uint32_t kPause = 0xD08020;
constexpr uint32_t kGroup = 0x2878B8;
constexpr uint32_t kFavorite = 0x76509B;
constexpr uint32_t kText = 0xFFFFFF;
constexpr uint32_t kMutedText = 0xB8C4CC;
constexpr uint32_t kError = 0xE05050;

constexpr int kMediaButtonY = 120;
constexpr int kMediaButtonHeight = 54;
constexpr int kGroupButtonY = 182;
constexpr int kGroupButtonHeight = 49;
constexpr int kButtonGap = 8;
constexpr int kButtonWidth = (320 - (kButtonGap * 3)) / 2;
constexpr int kMainHeaderHeight = 34;
constexpr int kFavoriteHeaderHeight = 32;
constexpr int kFavoriteMargin = 6;
constexpr int kFavoriteTileY = 34;
constexpr int kFavoriteTileGap = 4;
constexpr int kFavoriteTileWidth =
    (320 - (kFavoriteMargin * 2) - kFavoriteTileGap) / 2;
constexpr int kFavoriteTileHeight = 72;
constexpr int kFavoritesPerPage = 4;
constexpr int kFavoriteNavY = 186;
constexpr int kFavoriteNavHeight = 52;
constexpr size_t kMaxFavorites = 48;
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

struct FavoriteItem {
  String title;
  String uri;
  String metadata;
};

struct SonosZone {
  String name;
  String ip;
  String uuid;
  bool isCoordinator;
};

enum class ScreenMode { Main, Favorites };

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
std::vector<FavoriteItem> favorites;
ScreenMode screenMode = ScreenMode::Main;
int favoritePage = 0;
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

String getPlayerUuid(const String& ip);

void drawStatus(const String& line1, const String& line2 = "",
                uint32_t color = kMutedText) {
  if (screenMode != ScreenMode::Main) return;
  M5.Display.fillRect(0, 62, 320, 55, kBackground);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(color, kBackground);
  M5.Display.drawString(line1, 160, 77);
  M5.Display.setTextColor(kMutedText, kBackground);
  M5.Display.drawString(line2, 160, 99);
}

void drawMediaButton() {
  M5.Display.fillRoundRect(kButtonGap, kMediaButtonY,
                           320 - (kButtonGap * 2), kMediaButtonHeight, 10,
                           kPlay);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextColor(kText, kPlay);
  M5.Display.setTextSize(2);
  M5.Display.drawString(">  PLAY / PAUSE", 160,
                        kMediaButtonY + (kMediaButtonHeight / 2));
}

void drawBottomButton(int x, uint32_t color, const char* label) {
  M5.Display.fillRoundRect(x, kGroupButtonY, kButtonWidth,
                           kGroupButtonHeight, 10, color);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextColor(kText, color);
  M5.Display.setTextSize(2);
  M5.Display.drawString(label, x + (kButtonWidth / 2),
                        kGroupButtonY + (kGroupButtonHeight / 2));
}

void drawInterface() {
  screenMode = ScreenMode::Main;
  M5.Display.fillScreen(kBackground);
  M5.Display.fillRect(0, 0, 320, kMainHeaderHeight, kPanel);
  M5.Display.setTextDatum(middle_left);
  M5.Display.setTextColor(kText, kPanel);
  M5.Display.setTextSize(1);
  M5.Display.drawString("SONOS REMOTE", 10, kMainHeaderHeight / 2);

  drawMediaButton();
  drawBottomButton(kButtonGap, kGroup, "GROUP ALL");
  drawBottomButton((kButtonGap * 2) + kButtonWidth, kFavorite, "FAVORITES");
}

String favoriteLabel(const String& title) {
  constexpr int kMaxLabelLength = 22;
  if (title.length() <= kMaxLabelLength) return title;
  return title.substring(0, kMaxLabelLength - 3) + "...";
}

int favoritePageCount() {
  if (favorites.empty()) return 1;
  return (favorites.size() + kFavoritesPerPage - 1) / kFavoritesPerPage;
}

void drawFavoriteHeader(const String& title = "FAVORITES") {
  M5.Display.fillRect(0, 0, 320, kFavoriteHeaderHeight, kPanel);
  M5.Display.fillRoundRect(4, 3, 68, 26, 6, kGroup);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextColor(kText, kGroup);
  M5.Display.setTextSize(1);
  M5.Display.drawString("< BACK", 38, 16);
  M5.Display.setTextColor(kText, kPanel);
  M5.Display.drawString(title, 156, 16);
}

void drawFavoritesLoading() {
  M5.Display.fillScreen(kBackground);
  drawFavoriteHeader();
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextColor(kMutedText, kBackground);
  M5.Display.setTextSize(1);
  M5.Display.drawString("Loading Sonos Favorites...", 160, 125);
}

void drawFavoriteTileLabel(const String& title, int centerX, int centerY,
                           uint32_t background) {
  const String label = favoriteLabel(title);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextColor(kText, background);
  M5.Display.setTextSize(2);

  constexpr int kCharactersPerLine = 11;
  if (label.length() <= kCharactersPerLine) {
    M5.Display.drawString(label, centerX, centerY);
    return;
  }

  int split = label.lastIndexOf(' ', kCharactersPerLine);
  if (split < 7) split = kCharactersPerLine;
  String top = label.substring(0, split);
  String bottom = label.substring(split);
  top.trim();
  bottom.trim();
  if (bottom.length() > kCharactersPerLine) {
    bottom = bottom.substring(0, kCharactersPerLine - 3) + "...";
  }
  M5.Display.drawString(top, centerX, centerY - 10);
  M5.Display.drawString(bottom, centerX, centerY + 10);
}

void drawFavoritesScreen() {
  screenMode = ScreenMode::Favorites;
  favoritePage = constrain(favoritePage, 0, favoritePageCount() - 1);
  M5.Display.fillScreen(kBackground);
  const int pages = favoritePageCount();
  drawFavoriteHeader(String("FAVORITES ") + String(favoritePage + 1) + "/" +
                     String(pages));

  const int first = favoritePage * kFavoritesPerPage;
  for (int tile = 0; tile < kFavoritesPerPage; ++tile) {
    const int index = first + tile;
    if (index >= static_cast<int>(favorites.size())) break;

    const int column = tile % 2;
    const int row = tile / 2;
    const int x = kFavoriteMargin +
                  column * (kFavoriteTileWidth + kFavoriteTileGap);
    const int y =
        kFavoriteTileY + row * (kFavoriteTileHeight + kFavoriteTileGap);
    const uint32_t tileColor = tile % 2 == 0 ? 0x2A4050 : 0x243744;
    M5.Display.fillRoundRect(x, y, kFavoriteTileWidth, kFavoriteTileHeight, 9,
                             tileColor);
    drawFavoriteTileLabel(favorites[index].title,
                          x + (kFavoriteTileWidth / 2),
                          y + (kFavoriteTileHeight / 2), tileColor);
  }

  const bool hasPrevious = favoritePage > 0;
  const bool hasNext = favoritePage + 1 < pages;
  M5.Display.fillRoundRect(kFavoriteMargin, kFavoriteNavY, kFavoriteTileWidth,
                           kFavoriteNavHeight, 8,
                           hasPrevious ? kGroup : kPanel);
  M5.Display.fillRoundRect(kFavoriteMargin + kFavoriteTileWidth +
                               kFavoriteTileGap,
                           kFavoriteNavY, kFavoriteTileWidth,
                           kFavoriteNavHeight, 8,
                           hasNext ? kGroup : kPanel);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(hasPrevious ? kText : kMutedText,
                          hasPrevious ? kGroup : kPanel);
  M5.Display.drawString("< PREVIOUS", 82,
                        kFavoriteNavY + (kFavoriteNavHeight / 2));
  M5.Display.setTextColor(hasNext ? kText : kMutedText,
                          hasNext ? kGroup : kPanel);
  M5.Display.drawString("NEXT >", 238,
                        kFavoriteNavY + (kFavoriteNavHeight / 2));
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

String extractXmlElement(const String& xml, const String& tag) {
  const String opening = "<" + tag;
  const String closing = "</" + tag + ">";
  const int openingStart = xml.indexOf(opening);
  if (openingStart < 0) return "";
  const int valueStart = xml.indexOf('>', openingStart);
  if (valueStart < 0) return "";
  const int valueEnd = xml.indexOf(closing, valueStart + 1);
  if (valueEnd < 0) return "";
  return xml.substring(valueStart + 1, valueEnd);
}

String decodeXmlOnce(String value) {
  value.replace("&lt;", "<");
  value.replace("&gt;", ">");
  value.replace("&quot;", "\"");
  value.replace("&apos;", "'");
  value.replace("&amp;", "&");
  return value;
}

String escapeXml(String value) {
  value.replace("&", "&amp;");
  value.replace("<", "&lt;");
  value.replace(">", "&gt;");
  value.replace("\"", "&quot;");
  value.replace("'", "&apos;");
  return value;
}

bool loadFavorites() {
  favorites.clear();
  const String body =
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
      "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
      "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
      "<s:Body>"
      "<u:Browse "
      "xmlns:u=\"urn:schemas-upnp-org:service:ContentDirectory:1\">"
      "<ObjectID>FV:2</ObjectID>"
      "<BrowseFlag>BrowseDirectChildren</BrowseFlag>"
      "<Filter>*</Filter><StartingIndex>0</StartingIndex>"
      "<RequestedCount>100</RequestedCount><SortCriteria></SortCriteria>"
      "</u:Browse></s:Body></s:Envelope>";

  HTTPClient http;
  http.setTimeout(10000);
  http.setReuse(false);
  const String url =
      "http://" + selectedIp + ":1400/MediaServer/ContentDirectory/Control";
  if (!http.begin(url)) return false;
  http.addHeader("Content-Type", "text/xml; charset=utf-8");
  http.addHeader(
      "SOAPAction",
      "\"urn:schemas-upnp-org:service:ContentDirectory:1#Browse\"");
  const int status = http.POST(body);
  if (status != HTTP_CODE_OK) {
    Serial.printf("Favorites browse failed: HTTP %d\n", status);
    http.end();
    return false;
  }

  String response = http.getString();
  http.end();
  String didl = decodeXmlOnce(extractXmlValue(response, "Result"));
  response.clear();
  if (didl.isEmpty()) {
    Serial.println("Favorites browse returned no DIDL result");
    return false;
  }

  favorites.reserve(32);
  int cursor = 0;
  int skipped = 0;
  while (favorites.size() < kMaxFavorites) {
    const int itemStart = didl.indexOf("<item ", cursor);
    if (itemStart < 0) break;
    const int itemEnd = didl.indexOf("</item>", itemStart);
    if (itemEnd < 0) break;
    const String item = didl.substring(itemStart, itemEnd + 7);
    cursor = itemEnd + 7;

    const String title = decodeXmlOnce(extractXmlElement(item, "dc:title"));
    const String uri = decodeXmlOnce(extractXmlElement(item, "res"));
    const String metadata =
        decodeXmlOnce(extractXmlElement(item, "r:resMD"));
    if (title.isEmpty() || uri.isEmpty()) {
      ++skipped;
      continue;
    }

    favorites.push_back({title, uri, metadata});
    Serial.printf("Favorite %u: %s\n",
                  static_cast<unsigned>(favorites.size()), title.c_str());
  }

  Serial.printf("Loaded %u playable favorite(s); skipped %d shortcut(s)\n",
                static_cast<unsigned>(favorites.size()), skipped);
  return true;
}

int postAvTransportTo(const String& playerIp, const String& action,
                      const String& arguments, String* responseOut = nullptr) {
  String body;
  body.reserve(arguments.length() + action.length() * 2 + 300);
  body =
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
      "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
      "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
      "<s:Body><u:";
  body += action;
  body += " xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">";
  body += arguments;
  body += "</u:";
  body += action;
  body += "></s:Body></s:Envelope>";

  HTTPClient http;
  http.setTimeout(10000);
  http.setReuse(false);
  const String url =
      "http://" + playerIp + ":1400/MediaRenderer/AVTransport/Control";
  if (!http.begin(url)) return false;
  http.addHeader("Content-Type", "text/xml; charset=utf-8");
  http.addHeader("SOAPAction", "\"urn:schemas-upnp-org:service:AVTransport:1#" +
                                   action + "\"");
  const int status = http.POST(body);
  const String response = http.getString();
  http.end();
  if (responseOut != nullptr) *responseOut = response;

  if (status != HTTP_CODE_OK) {
    const String errorCode = extractXmlValue(response, "errorCode");
    Serial.printf("AVTransport %s to %s failed: HTTP %d, Sonos error %s\n",
                  action.c_str(), playerIp.c_str(), status,
                  errorCode.isEmpty() ? "unknown" : errorCode.c_str());
  }
  return status;
}

int postAvTransport(const String& action, const String& arguments,
                    String* responseOut = nullptr) {
  return postAvTransportTo(selectedIp, action, arguments, responseOut);
}

void togglePlayback() {
  if (!ready) {
    drawStatus("Not ready", "Restart after fixing config", kError);
    return;
  }

  drawStatus("Checking playback...", selectedRoom);
  String response;
  if (postAvTransport("GetTransportInfo", "<InstanceID>0</InstanceID>",
                      &response) != HTTP_CODE_OK) {
    drawStatus("Play / Pause failed", "Could not read Sonos state", kError);
    return;
  }

  const String state = extractXmlValue(response, "CurrentTransportState");
  const bool shouldPause = state == "PLAYING" || state == "TRANSITIONING";
  Serial.printf("Playback toggle: state=%s, action=%s\n", state.c_str(),
                shouldPause ? "pause" : "play");
  runCommand(!shouldPause);
}

bool setTransportUri(const String& uri, const String& metadata) {
  String arguments;
  arguments.reserve(uri.length() + metadata.length() + 160);
  arguments = "<InstanceID>0</InstanceID><CurrentURI>";
  arguments += escapeXml(uri);
  arguments += "</CurrentURI><CurrentURIMetaData>";
  arguments += escapeXml(metadata);
  arguments += "</CurrentURIMetaData>";
  return postAvTransport("SetAVTransportURI", arguments) == HTTP_CODE_OK;
}

bool addFavoriteToQueue(const FavoriteItem& favorite, int& firstTrack) {
  String arguments;
  arguments.reserve(favorite.uri.length() + favorite.metadata.length() + 260);
  arguments = "<InstanceID>0</InstanceID><EnqueuedURI>";
  arguments += escapeXml(favorite.uri);
  arguments += "</EnqueuedURI><EnqueuedURIMetaData>";
  arguments += escapeXml(favorite.metadata);
  arguments +=
      "</EnqueuedURIMetaData>"
      "<DesiredFirstTrackNumberEnqueued>0</DesiredFirstTrackNumberEnqueued>"
      "<EnqueueAsNext>0</EnqueueAsNext>";

  String response;
  if (postAvTransport("AddURIToQueue", arguments, &response) != HTTP_CODE_OK) {
    return false;
  }

  firstTrack = extractXmlValue(response, "FirstTrackNumberEnqueued").toInt();
  Serial.printf("Favorite added to Sonos queue at track %d\n", firstTrack);
  return firstTrack > 0;
}

bool clearSonosQueue() {
  return postAvTransport("RemoveAllTracksFromQueue",
                         "<InstanceID>0</InstanceID>") == HTTP_CODE_OK;
}

bool selectQueueTrack(int trackNumber) {
  const String playerUuid = getPlayerUuid(selectedIp);
  if (playerUuid.isEmpty() ||
      !setTransportUri("x-rincon-queue:" + playerUuid + "#0", "")) {
    return false;
  }

  const String arguments =
      "<InstanceID>0</InstanceID><Unit>TRACK_NR</Unit><Target>" +
      String(trackNumber) + "</Target>";
  return postAvTransport("Seek", arguments) == HTTP_CODE_OK;
}

bool prepareFavorite(const FavoriteItem& favorite) {
  const bool needsQueue = favorite.uri.startsWith("x-rincon-cpcontainer:") ||
                          favorite.uri.startsWith("x-rincon-playlist:");
  if (!needsQueue) {
    Serial.println("Favorite playback mode: direct stream");
    return setTransportUri(favorite.uri, favorite.metadata);
  }

  Serial.println("Favorite playback mode: Sonos queue");
  int firstTrack = 0;
  return clearSonosQueue() && addFavoriteToQueue(favorite, firstTrack) &&
         selectQueueTrack(firstTrack);
}

void playFavorite(int index) {
  if (index < 0 || index >= static_cast<int>(favorites.size())) return;
  const FavoriteItem& favorite = favorites[index];
  const String title = favorite.title;

  drawInterface();
  drawStatus("Starting favorite...", title, kFavorite);
  if (!prepareFavorite(favorite)) {
    drawStatus("Favorite failed", "Could not prepare source", kError);
    return;
  }

  const SonosResult result = sonos.play(selectedIp);
  if (result == SonosResult::SUCCESS) {
    drawStatus("Playing favorite", title, kPlay);
    Serial.printf("Favorite started: %s\n", title.c_str());
  } else {
    drawStatus("Favorite failed", sonos.getErrorString(result), kError);
  }
}

void openFavorites() {
  if (!ready) {
    drawStatus("Not ready", "Restart after fixing config", kError);
    return;
  }

  screenMode = ScreenMode::Favorites;
  drawFavoritesLoading();
  if (!loadFavorites()) {
    drawInterface();
    drawStatus("Favorites failed", "Check network and retry", kError);
    return;
  }

  favoritePage = 0;
  drawFavoritesScreen();
  noteDisplayActivity();
}

void handleFavoriteTouch(int x, int y) {
  if (y >= 3 && y < 29 && x >= 4 && x < 72) {
    drawInterface();
    drawStatus("Ready: " + selectedRoom, selectedIp, kPlay);
    return;
  }

  if (y >= kFavoriteTileY && y < kFavoriteNavY) {
    for (int tile = 0; tile < kFavoritesPerPage; ++tile) {
      const int column = tile % 2;
      const int row = tile / 2;
      const int tileX = kFavoriteMargin +
                        column * (kFavoriteTileWidth + kFavoriteTileGap);
      const int tileY =
          kFavoriteTileY + row * (kFavoriteTileHeight + kFavoriteTileGap);
      if (x >= tileX && x < tileX + kFavoriteTileWidth && y >= tileY &&
          y < tileY + kFavoriteTileHeight) {
        playFavorite(favoritePage * kFavoritesPerPage + tile);
        return;
      }
    }
    return;
  }

  if (y >= kFavoriteNavY && y < kFavoriteNavY + kFavoriteNavHeight) {
    if (x < 160 && favoritePage > 0) {
      --favoritePage;
      drawFavoritesScreen();
    } else if (x >= 160 && favoritePage + 1 < favoritePageCount()) {
      ++favoritePage;
      drawFavoritesScreen();
    }
  }
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

String extractXmlAttribute(const String& openingTag, const String& name) {
  const String marker = name + "=\"";
  const int start = openingTag.indexOf(marker);
  if (start < 0) return "";
  const int valueStart = start + marker.length();
  const int end = openingTag.indexOf('"', valueStart);
  return end < 0 ? "" : decodeXmlOnce(openingTag.substring(valueStart, end));
}

String ipFromSonosLocation(String location) {
  if (location.startsWith("http://")) location.remove(0, 7);
  int end = location.indexOf(':');
  if (end < 0) end = location.indexOf('/');
  return end < 0 ? location : location.substring(0, end);
}

bool loadSonosTopology(std::vector<SonosZone>& zones) {
  zones.clear();
  const String body =
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
      "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
      "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
      "<s:Body><u:GetZoneGroupState "
      "xmlns:u=\"urn:schemas-upnp-org:service:ZoneGroupTopology:1\">"
      "</u:GetZoneGroupState></s:Body></s:Envelope>";

  HTTPClient http;
  http.setTimeout(10000);
  http.setReuse(false);
  const String url =
      "http://" + selectedIp + ":1400/ZoneGroupTopology/Control";
  if (!http.begin(url)) return false;
  http.addHeader("Content-Type", "text/xml; charset=utf-8");
  http.addHeader(
      "SOAPAction",
      "\"urn:schemas-upnp-org:service:ZoneGroupTopology:1#GetZoneGroupState\"");
  const int status = http.POST(body);
  if (status != HTTP_CODE_OK) {
    Serial.printf("Sonos topology failed: HTTP %d\n", status);
    http.end();
    return false;
  }

  String response = http.getString();
  http.end();
  String topology =
      decodeXmlOnce(extractXmlValue(response, "ZoneGroupState"));
  response.clear();
  if (topology.isEmpty()) return false;

  int groupCursor = 0;
  while (true) {
    const int groupStart = topology.indexOf("<ZoneGroup ", groupCursor);
    if (groupStart < 0) break;
    const int groupOpenEnd = topology.indexOf('>', groupStart);
    const int groupEnd = topology.indexOf("</ZoneGroup>", groupOpenEnd);
    if (groupOpenEnd < 0 || groupEnd < 0) break;

    const String groupOpening =
        topology.substring(groupStart, groupOpenEnd + 1);
    const String coordinatorUuid =
        extractXmlAttribute(groupOpening, "Coordinator");
    int memberCursor = groupOpenEnd + 1;
    while (true) {
      const int memberStart =
          topology.indexOf("<ZoneGroupMember ", memberCursor);
      if (memberStart < 0 || memberStart >= groupEnd) break;
      const int memberOpenEnd = topology.indexOf('>', memberStart);
      if (memberOpenEnd < 0 || memberOpenEnd >= groupEnd) break;
      memberCursor = memberOpenEnd + 1;

      const String memberOpening =
          topology.substring(memberStart, memberOpenEnd + 1);
      if (extractXmlAttribute(memberOpening, "Invisible") == "1") continue;

      SonosZone zone;
      zone.uuid = extractXmlAttribute(memberOpening, "UUID");
      zone.name = extractXmlAttribute(memberOpening, "ZoneName");
      zone.ip = ipFromSonosLocation(
          extractXmlAttribute(memberOpening, "Location"));
      zone.isCoordinator = zone.uuid == coordinatorUuid;
      if (!zone.ip.isEmpty() && !zone.uuid.isEmpty()) zones.push_back(zone);
    }
    groupCursor = groupEnd + 12;
  }

  Serial.printf("Sonos topology contains %u visible zone(s)\n",
                static_cast<unsigned>(zones.size()));
  return !zones.empty();
}

bool findPlayingCoordinator(const std::vector<SonosZone>& zones,
                            String& coordinatorIp, String& coordinatorRoom) {
  int activeCoordinators = 0;

  for (const auto& zone : zones) {
    if (!zone.isCoordinator) continue;

    String transportResponse;
    if (postAvTransportTo(zone.ip, "GetTransportInfo",
                          "<InstanceID>0</InstanceID>",
                          &transportResponse) != HTTP_CODE_OK) {
      continue;
    }

    const String state =
        extractXmlValue(transportResponse, "CurrentTransportState");
    if (state != "PLAYING" && state != "TRANSITIONING") continue;

    Serial.printf("Active Sonos coordinator: %s (%s)\n", zone.name.c_str(),
                  state.c_str());

    if (activeCoordinators == 0) {
      coordinatorIp = zone.ip;
      coordinatorRoom = zone.name;
    }
    ++activeCoordinators;
  }

  if (activeCoordinators > 1) {
    Serial.printf("%d active Sonos groups found; using %s\n",
                  activeCoordinators, coordinatorRoom.c_str());
  }
  return activeCoordinators > 0;
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

  String coordinatorIp;
  String coordinatorRoom;
  std::vector<SonosZone> zones;
  drawStatus("Finding playing room...", "Checking every Sonos player");
  if (!loadSonosTopology(zones)) {
    drawStatus("Group failed", "Could not read Sonos topology", kError);
    return;
  }
  if (!findPlayingCoordinator(zones, coordinatorIp, coordinatorRoom)) {
    drawStatus("Nothing is playing", "Start music, then try again", kError);
    Serial.println("Group All stopped: no active Sonos coordinator found");
    return;
  }

  selectedIp = coordinatorIp;
  selectedRoom = coordinatorRoom;
  drawStatus("Grouping to " + selectedRoom + "...", selectedIp);
  const String coordinatorUuid = getPlayerUuid(selectedIp);
  if (coordinatorUuid.isEmpty()) {
    drawStatus("Group failed", "Coordinator UUID unavailable", kError);
    return;
  }

  int joined = 0;
  int failed = 0;
  for (const auto& zone : zones) {
    if (zone.ip == selectedIp) continue;
    if (joinPlayerToCoordinator(zone.ip, coordinatorUuid)) {
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
    if (screenMode == ScreenMode::Favorites && touchArmed) {
      touchArmed = false;
      handleFavoriteTouch(touch.x, touch.y);
    } else if (touchArmed && touch.y >= kMediaButtonY &&
        touch.y < kMediaButtonY + kMediaButtonHeight) {
      touchArmed = false;
      togglePlayback();
    } else if (touchArmed && touch.y >= kGroupButtonY &&
               touch.y < kGroupButtonY + kGroupButtonHeight) {
      touchArmed = false;
      if (touch.x < 160) {
        groupAll();
      } else {
        openFavorites();
      }
    }
  }

  handleDisplayIdleTimeout();
  delay(10);
}
