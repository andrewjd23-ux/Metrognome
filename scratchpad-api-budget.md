# Metrognome Scratchpad: API Budget and Refresh Logic

Working notes for the Stormglass API version of Metrognome.

## Goal

Metrognome should not get stuck, burn through its daily Stormglass quota, or lose track of usage if rebooted. It should keep showing cached/stale data when the API is unavailable or the daily limit has been reached.

## Core idea

Use a daily API budget manager:

- Store `callsToday` and `apiDate` in ESP32 flash using `Preferences`.
- At boot, connect to WiFi and get real time using NTP.
- Compare the stored date with today's UTC date.
- If the stored date is different from today, reset `callsToday` to 0.
- Scheduled refreshes and manual refreshes must both go through the same quota check.
- Manual encoder presses must request a refresh, not blindly call the API.

## Suggested limits

Initial free-tier-friendly defaults:

- `DAILY_API_LIMIT = 10`
- `RESERVED_CALLS = 1`
- Scheduled refresh interval: every 3 hours
- Manual refresh allowed only while `callsToday < DAILY_API_LIMIT - RESERVED_CALLS`
- Scheduled refresh allowed while `callsToday < DAILY_API_LIMIT`

This preserves one call for scheduled operation or emergency/manual use.

## Rollover time

Use UTC midnight for the API counter rollover unless Stormglass documents a different quota window.

The display can show local time later, but API quota accounting should use UTC to avoid daylight-saving weirdness.

## Refresh flow

```text
encoder press or timer event
  -> request refresh
  -> check WiFi
  -> check NTP/time available
  -> load API counter from Preferences
  -> reset counter if stored date != today UTC
  -> quota check
  -> if allowed, call Stormglass
  -> if API call succeeds, increment counter and cache parsed data
  -> if API call fails, keep old cached data and show API ERR / STALE
```

## UI behaviour

Small onboard OLED should show compact system state:

```text
WiFi OK
API 3/10
PAGE SEA
SYNC 14:32
```

If limit reached:

```text
API LIMIT
DATA STALE
RESET 00:00
```

If WiFi fails:

```text
WiFi FAIL
DATA STALE
API IDLE
```

Big OLED should continue showing the most recent cached weather/marine values, with a `STALE` marker if the data is old or the latest refresh failed.

## Pseudocode

```cpp
#include <Preferences.h>
#include <time.h>

Preferences prefs;

const int DAILY_API_LIMIT = 10;
const int RESERVED_CALLS = 1;

int callsToday = 0;
String storedDate = "";

String todayUtcDate() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return "";
  }

  char buffer[11];
  strftime(buffer, sizeof(buffer), "%Y-%m-%d", &timeinfo);
  return String(buffer);
}

void loadApiCounter() {
  prefs.begin("metrognome", false);

  String today = todayUtcDate();
  storedDate = prefs.getString("api_date", "");
  callsToday = prefs.getInt("api_calls", 0);

  if (today != "" && storedDate != today) {
    callsToday = 0;
    storedDate = today;
    prefs.putString("api_date", storedDate);
    prefs.putInt("api_calls", callsToday);
  }
}

bool canCallApi(bool manualRefresh) {
  loadApiCounter();

  if (manualRefresh) {
    return callsToday < (DAILY_API_LIMIT - RESERVED_CALLS);
  }

  return callsToday < DAILY_API_LIMIT;
}

void recordApiCall() {
  callsToday++;
  prefs.putInt("api_calls", callsToday);
}
```

## Build order reminder

1. Add WiFi connection and status display.
2. Add NTP time sync.
3. Add API budget manager and fake refresh counter.
4. Add HTTPS Stormglass call.
5. Add JSON parsing into a `WeatherData`/`MetrognomeData` struct.
6. Add cache/stale/error states.
7. Add real Weather / Sea / Tide / Moon pages.

## Notes

Do not store real WiFi credentials or Stormglass keys in committed files. Use a local `secrets.h`, and add a safe example file such as `secrets.example.h` later.
