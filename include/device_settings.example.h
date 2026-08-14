#pragma once

// Use the room that should become coordinator when you press GROUP ALL.
// The spelling must exactly match the room name in the Sonos app.
// Leave empty to select the first player discovered on the network.
constexpr char SONOS_COORDINATOR_ROOM[] = "Living Room";

// Full fader travel maps to 0..SONOS_MAX_VOLUME. This safety cap prevents the
// physical control from unexpectedly sending the group to Sonos's full 100%.
constexpr int SONOS_MAX_VOLUME = 40;
