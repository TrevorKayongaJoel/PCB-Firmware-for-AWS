#include "config.h"
#include "portal_html.h"
#include "calibration.h"
#include "serial_ui.h"

// ---------- Object Instances ----------
Adafruit_ADS1115      ads;
Adafruit_INA219      batterySensor(0x40);
Adafruit_INA219      solarSensor(0x41);
RTC_DS3231           rtc;
DHT                  dht(DHTPIN, DHTTYPE);
Preferences          preferences;

// ---------- DS18B20 Objects ----------
OneWire              oneWire(DS18B20_PIN);
DallasTemperature    ds18b20(&oneWire);
bool                 ds18b20Ok = false;

// ---------- BME280 (pressure) ----------
// Not fitted yet. Probed on every rail power-up, so the sensor starts working
// the moment it is plugged in - no reflash required.
Adafruit_BME280      bme;
bool                 bmeOk = false;

// ---------- Global Variables ----------
int sleepIntervalSec = 300;
String stationCode = "AWS-UG-001";
bool relayOn = false;

// ---------- Clock State ----------
bool rtcOk = false;
bool rtcTimeValid = false;
static bool ntpEverSynced = false;          // NTP succeeded at least once this boot
static unsigned long lastNtpSyncMs = 0;     // millis() of last success
static unsigned long lastNtpAttemptMs = 0;  // millis() of last attempt, success or not
DataRecord lastRecord;
bool haveLastRecord = false;
uint32_t nextCycleEpoch = 0;
int queueDepthCached = 0;
unsigned long nextCycleMillis = 0;

// ================== WIND & RAIN PULSE COUNTING ==================
// Pulses have to be counted whether the CPU is awake or in light sleep, and
// light sleep only supports LEVEL-triggered GPIO wake - not edges. So there are
// two ways into the counters:
//
//   awake  - the edge ISRs below
//   asleep - esp_light_sleep_start() returns on a LOW level and the wake
//            handler counts the pulse directly
//
// Both funnel through countWindPulse()/countRainPulse(), which share one
// debounce timestamp per channel. Without that shared timestamp a single reed
// closure is counted twice: once by the level wake, once by the edge ISR as the
// CPU resumes.

volatile uint32_t windPulseCount = 0;
volatile uint32_t rainPulseCount = 0;
static volatile uint32_t lastWindPulseUs = 0;
static volatile uint32_t lastRainPulseUs = 0;

// GPIO wakes where neither line was still low by the time we read it - i.e.
// pulses too short to attribute. Reported each interval so it stays visible.
static volatile uint32_t ambiguousWakes = 0;

// Gust = the highest 3 s average within the interval. Tracked as a running
// "best" bucket. Deliberately integer-only: doing float maths inside an IRAM
// ISR means relying on FPU state being saved, which it is not by default.
static volatile uint32_t gustBucketStartUs = 0;
static volatile uint32_t gustBucketCount   = 0;
static volatile uint32_t gustBestCount     = 0;
static volatile uint32_t gustBestSpanUs    = 1;   // 1, not 0, so the first compare works

// When the current interval started. Seeded in setup() at the moment the
// interrupts are armed, so the first burst divides by the time counting has
// actually been running rather than by an assumed full interval.
static uint32_t      lastSnapshotEpoch  = 0;
static unsigned long lastSnapshotMillis = 0;

// Interval results, refreshed by snapshotPulseCounters().
float windSpeed_MPH = 0.0;
float windSpeed_MS  = 0.0;
float windGust_MS   = 0.0;
float rain_mm       = 0.0;   // mm accumulated in the current interval, zeroed each snapshot
float lastIntervalSec = 0.0;   // what windSpeed_MS and rain_mm actually average over
uint32_t windGustCount  = 0;   // pulses in the winning gust window, 0 if none closed
uint32_t windGustSpanMs = 0;   // width of that window

// ---------- Per-minute buckets ----------
// The interval counters above are never reset here. A bucket is the DIFFERENCE
// between the counter now and at the last close, so the slices sum to the
// interval total by construction rather than by luck - no pulse can land in
// both a slice and the remainder, or in neither.
static uint16_t bucketWind[MAX_BUCKETS];
static uint16_t bucketRain[MAX_BUCKETS];
static uint8_t  bucketCount = 0;
static uint32_t bucketWindBase = 0;   // counter values when the open slice began
static uint32_t bucketRainBase = 0;
static unsigned long bucketStartMillis = 0;

// Result of the last COMPLETED interval, published by snapshotPulseCounters()
// and consumed by readSensors() - the same handover windSpeed_MS uses. Kept
// separate from the live accumulator above so the next interval can start
// counting immediately instead of waiting for the record to be built.
static uint16_t lastBucketWind[MAX_BUCKETS];
static uint16_t lastBucketRain[MAX_BUCKETS];
static uint8_t  lastBucketCount = 0;

static inline uint16_t clampU16(uint32_t v) {
  return (v > 0xFFFFUL) ? (uint16_t)0xFFFF : (uint16_t)v;
}

// windNow/rainNow are the counter values at the instant the slice closes.
static void closeBucket(uint32_t windNow, uint32_t rainNow) {
  uint32_t w = windNow - bucketWindBase;
  uint32_t r = rainNow - bucketRainBase;

  if (bucketCount < MAX_BUCKETS) {
    bucketWind[bucketCount] = clampU16(w);
    bucketRain[bucketCount] = clampU16(r);
    bucketCount++;
  } else {
    // Should be unreachable - MAX_BUCKETS has headroom over SLEEP_MAX_SEC. Fold
    // rather than drop anyway: the sum-equals-total invariant is what the server
    // reconstructs rainfall from, and a silent shortfall there is invisible.
    uint8_t last = MAX_BUCKETS - 1;
    bucketWind[last] = clampU16((uint32_t)bucketWind[last] + w);
    bucketRain[last] = clampU16((uint32_t)bucketRain[last] + r);
  }

  bucketWindBase = windNow;
  bucketRainBase = rainNow;
}

// Close every slice whose BUCKET_SEC has run out. Called from every phase that
// can hold the CPU for an appreciable time - the sleep loop, the portal loop and
// the queue drain - because a phase that runs long without calling this does not
// lose pulses, but does push them into the wrong minute.
//
// The boundary advances by exactly BUCKET_SEC rather than to now(), so slice i
// always covers minute i of the interval and the server can derive its window
// from the record timestamp alone.
void maybeCloseBuckets() {
  while ((long)(millis() - bucketStartMillis) >= (long)BUCKET_SEC * 1000L) {
    noInterrupts();
    uint32_t w = windPulseCount;
    uint32_t r = rainPulseCount;
    interrupts();

    closeBucket(w, r);
    bucketStartMillis += (unsigned long)BUCKET_SEC * 1000UL;
  }
}

void IRAM_ATTR countWindPulse() {
  uint32_t now = micros();
  if ((uint32_t)(now - lastWindPulseUs) < WIND_DEBOUNCE_US) return;
  lastWindPulseUs = now;
  windPulseCount++;

  // Close the gust bucket once it has run its full width and keep it if it beat
  // the best so far. Comparing count/span as a cross-multiply avoids a divide.
  uint32_t span = now - gustBucketStartUs;
  if (span >= GUST_WINDOW_US) {
    if ((uint64_t)gustBucketCount * (uint64_t)gustBestSpanUs >
        (uint64_t)gustBestCount * (uint64_t)span) {
      gustBestCount  = gustBucketCount;
      gustBestSpanUs = span;
    }
    gustBucketStartUs = now;
    gustBucketCount   = 0;
  }
  gustBucketCount++;
}

void IRAM_ATTR countRainPulse() {
  uint32_t now = micros();
  if ((uint32_t)(now - lastRainPulseUs) < DEBOUNCE_US) return;
  lastRainPulseUs = now;
  rainPulseCount++;
}

void IRAM_ATTR windISR() { countWindPulse(); }
void IRAM_ATTR rainISR() { countRainPulse(); }

// ---------- Clock reference ----------
// The DS3231 sits on the gated sensor rail, so it is only readable during a
// burst. Between bursts the last good reading is carried forward on millis(),
// which keeps advancing across light sleep.
static uint32_t      clockRefEpoch  = 0;
static unsigned long clockRefMillis = 0;

// ---------- RTC bus pin swap ----------
// The DS3231 is the one device on the bus whose SDA and SCL pads are crossed
// (RTC_SDA/RTC_SCL), so every RTC access runs with the I2C peripheral
// re-pointed at that reversed pair and put back straight afterwards.
//
// Wire.begin() will not move the pins of a bus that is already up - it warns
// and returns - so the end() is what actually does the work. Keep the swapped
// window short: while it is open the other devices on those two wires see the
// lines crossed. Accesses nest (sensorRailUp() re-probes and then calls
// refreshRtcValidity()), so only the outermost scope touches the bus.
static uint8_t rtcBusDepth = 0;

struct RtcBus {
  RtcBus() {
    if (rtcBusDepth++ == 0) {
      Wire.end();
      Wire.begin(RTC_SDA, RTC_SCL);
      // Re-pointing the GPIO matrix glitches both lines, and the DS3231 can
      // read that as a stray START and then sit half-way through a transaction
      // it will never see the end of. One throwaway probe clocks it back to
      // idle. Without it the first real access after each switch is the one
      // that gets eaten - which is exactly how this looked before the boot
      // scan happened to absorb it.
      Wire.beginTransmission(0x68);   // RTClib keeps DS3231_ADDRESS private
      Wire.endTransmission();
    }
  }
  ~RtcBus() {
    if (--rtcBusDepth == 0) {
      Wire.end();
      Wire.begin(I2C_SDA, I2C_SCL);
    }
  }
};

#if I2C_SCAN_ON_BOOT
// Prints every address that ACKs on whichever pin mapping is live right now.
// Run on both mappings it separates the two reasons the DS3231 can stay silent:
// if the swapped scan still lists the sensors then the pin swap is not reaching
// the hardware, and if it lists nothing at all the swap is working and the
// module itself is the problem - unfitted, unpowered, or not the pin order the
// footprint assumes.
static void scanI2C(const char *label) {
  String hits;
  for (uint8_t addr = 0x08; addr < 0x78; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      char buf[8];
      snprintf(buf, sizeof(buf), " 0x%02X", addr);
      hits += buf;
    }
  }
  logInfo("I2C scan %s:%s", label,
          hits.length() ? hits.c_str() : " nothing responded");
}
#endif

// The DS3231 holds EAT, so this is not a true UTC epoch - but every use here is
// a difference between two of them, which is unaffected. Returns 0 only when
// the time is not known at all.
uint32_t currentEpoch() {
  if (rtcOk && relayOn) {
    RtcBus bus;
    DateTime now = rtc.now();
    if (now.isValid() && now.year() >= RTC_MIN_VALID_YEAR) {
      clockRefEpoch  = now.unixtime();
      clockRefMillis = millis();
      rtcTimeValid   = true;
      return clockRefEpoch;
    }
    // The rail is up and it still did not answer sensibly, so the clock really
    // is unset. Only latch invalid here: a failed read with the rail down means
    // nothing at all, and latching on it strands every later record at 1970.
    rtcTimeValid = false;
  }

  if (clockRefEpoch > 0) {
    return clockRefEpoch + (uint32_t)((millis() - clockRefMillis) / 1000UL);
  }
  return 0;
}

// ---------- Close out the interval ----------
// Called once at the top of each duty cycle, so every pulse is attributed to
// exactly one interval and nothing is double counted.
void snapshotPulseCounters() {
  noInterrupts();
  uint32_t windPulses = windPulseCount;  windPulseCount = 0;
  uint32_t rainTips   = rainPulseCount;  rainPulseCount = 0;
  uint32_t bestCount  = gustBestCount;
  uint32_t bestSpan   = gustBestSpanUs;
  gustBestCount     = 0;
  gustBestSpanUs    = 1;
  gustBucketCount   = 0;
  gustBucketStartUs = micros();
  interrupts();

  // Close the final, short slice using the very numbers the interval totals came
  // from, so the slices sum to those totals exactly. Then re-base to zero: the
  // counters were just reset above.
  closeBucket(windPulses, rainTips);
  memcpy(lastBucketWind, bucketWind, sizeof(lastBucketWind));
  memcpy(lastBucketRain, bucketRain, sizeof(lastBucketRain));
  lastBucketCount = bucketCount;

  bucketCount       = 0;
  bucketWindBase    = 0;
  bucketRainBase    = 0;
  bucketStartMillis = millis();

  // Elapsed time comes from the RTC, not micros(), which wraps every ~71 min -
  // uncomfortably close to the 60 min maximum interval. millis() is the
  // fallback when the clock is unknown; it advances across light sleep too.
  uint32_t nowEpoch = currentEpoch();
  float elapsed;
  if (nowEpoch > 0 && lastSnapshotEpoch > 0 && nowEpoch > lastSnapshotEpoch) {
    elapsed = (float)(nowEpoch - lastSnapshotEpoch);
  } else if (lastSnapshotMillis > 0) {
    elapsed = (float)(millis() - lastSnapshotMillis) / 1000.0f;
  } else {
    elapsed = (float)sleepIntervalSec;
  }
  lastSnapshotEpoch  = nowEpoch;      // 0 is fine - the millis path covers it
  lastSnapshotMillis = millis();
  if (elapsed < 1.0f) elapsed = 1.0f;

  float hz = (float)windPulses / elapsed;
  windSpeed_MPH = hz * WIND_MPH_PER_HZ;
  windSpeed_MS  = hz * WIND_MS_PER_HZ;

  float gustHz = (bestSpan > 0)
                 ? ((float)bestCount * 1000000.0f / (float)bestSpan)
                 : 0.0f;
  windGust_MS = gustHz * WIND_MS_PER_HZ;
  // With few pulses no bucket ever closes, which would report a gust below the
  // interval mean. The gust can never be less than the average.
  if (windGust_MS < windSpeed_MS) windGust_MS = windSpeed_MS;

  // Publish the window the gust came from. A zero count is how the server tells
  // "no window ever closed, so windGust_MS is the mean standing in for a gust"
  // apart from a genuinely measured one - the clamp above erases that
  // difference, and nothing else in the record records it.
  if (bestCount > 0) {
    windGustCount  = bestCount;
    windGustSpanMs = bestSpan / 1000UL;
  } else {
    windGustCount  = 0;
    windGustSpanMs = 0;
  }

  rain_mm = (float)rainTips * MM_PER_TIP;
  lastIntervalSec = elapsed;

  uint32_t missed = ambiguousWakes;
  ambiguousWakes = 0;
  if (missed > 0) {
    logWarn("%lu wake(s) not attributable to a pin - pulses may be undercounted.",
                  (unsigned long)missed);
  }

  logStep("wind %lu pulses / %.0f s -> %.2f m/s avg, %.2f m/s gust | rain %lu tips -> %.1f mm | %u slice(s) of %d s",
                (unsigned long)windPulses, elapsed, windSpeed_MS, windGust_MS,
                (unsigned long)rainTips, rain_mm,
                lastBucketCount, BUCKET_SEC);
}

// ================== LIGHT SLEEP ==================

// Light-sleep wake is level-triggered, so re-arming while the contact is still
// closed wakes instantly and spins at full power. Bounded so a stuck-closed
// reed shows up as a fault instead of a silent flat battery.
static bool waitForReedRelease(uint8_t pin) {
  uint32_t start = millis();
  while (digitalRead(pin) == LOW) {
    if ((millis() - start) > REED_RELEASE_MS) return false;
    delay(1);
  }
  return true;
}

static void reportStuckReed(const char *which) {
  // Rate limited, and Serial-only: the SD rail is powered down during sleep.
  static uint32_t lastReportMs = 0;
  if ((millis() - lastReportMs) < 60000UL) return;
  lastReportMs = millis();
  logWarn("%s reed still closed after %d ms - check for a stuck contact.",
                which, REED_RELEASE_MS);
}

void sleepUntilNextEventOrDeadline() {
  // How long we are allowed to stay down. Deliberately millis()-based rather
  // than reading the RTC: if the DS3231 shares the gated rail it is
  // unreachable right now, and esp_timer accounts for light sleep so millis()
  // keeps advancing. The deadline is re-derived from the RTC at the top of
  // every burst, so drift cannot accumulate across cycles.
  long msLeft = (long)(nextCycleMillis - millis());
  if (msLeft <= 0) return;                          // burst is due

  long remaining = msLeft / 1000;
  if (remaining < 1)                remaining = 1;
  if (remaining > SLEEP_CHUNK_SEC)  remaining = SLEEP_CHUNK_SEC;

  // The radio must be down: light sleep with WiFi up either draws far more or
  // is rejected outright.
  if (WiFi.getMode() != WIFI_OFF) WiFi.mode(WIFI_OFF);

  // Hand the pins from the Arduino edge ISRs to the level-triggered wake, then
  // hand them back afterwards. The two share the GPIO interrupt-type register,
  // and the driver source is not shipped with the core, so rather than assume
  // how gpio_wakeup_enable() treats an already-attached edge interrupt, this
  // makes the ownership explicit in both directions.
  detachInterrupt(digitalPinToInterrupt(WIND_PIN));
  detachInterrupt(digitalPinToInterrupt(RAIN_PIN));

  gpio_wakeup_enable((gpio_num_t)WIND_PIN, GPIO_INTR_LOW_LEVEL);
  gpio_wakeup_enable((gpio_num_t)RAIN_PIN, GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();
  esp_sleep_enable_timer_wakeup((uint64_t)remaining * 1000000ULL);

  esp_err_t err = esp_light_sleep_start();

  if (err == ESP_OK && esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO) {
    // Classic ESP32 has no esp_sleep_get_gpio_wakeup_status() - that API sits
    // behind SOC_GPIO_SUPPORT_DEEPSLEEP_WAKEUP, which only the S2/S3/C3 set.
    // The wake was level-triggered, so whichever line is still low is the one
    // that woke us. A closure shorter than the wake latency can slip through;
    // ambiguousWakes counts those so the loss is measurable rather than silent.
    // Sample both lines first: the contact that woke us may reopen within
    // milliseconds, and anything read later is already too late.
    bool windLow = (digitalRead(WIND_PIN) == LOW);
    bool rainLow = (digitalRead(RAIN_PIN) == LOW);

    // Give the pins back to the edge ISRs BEFORE the release waits below.
    // Those waits can run for REED_RELEASE_MS each, and with the ISRs still
    // detached a tip on the other channel during that window would vanish.
    // Re-arming here cannot double count: the line is already low, so there is
    // no falling edge to fire on, and the shared debounce covers the rest.
    gpio_wakeup_disable((gpio_num_t)WIND_PIN);
    gpio_wakeup_disable((gpio_num_t)RAIN_PIN);
    attachInterrupt(digitalPinToInterrupt(WIND_PIN), windISR, FALLING);
    attachInterrupt(digitalPinToInterrupt(RAIN_PIN), rainISR, FALLING);

    if (windLow) {
      countWindPulse();
      if (!waitForReedRelease(WIND_PIN)) reportStuckReed("Wind");
    }
    if (rainLow) {
      countRainPulse();
      if (!waitForReedRelease(RAIN_PIN)) reportStuckReed("Rain");
    }
    if (!windLow && !rainLow) ambiguousWakes++;
  } else {
    // Timer wake, or sleep was rejected - nothing to attribute, just restore.
    gpio_wakeup_disable((gpio_num_t)WIND_PIN);
    gpio_wakeup_disable((gpio_num_t)RAIN_PIN);
    attachInterrupt(digitalPinToInterrupt(WIND_PIN), windISR, FALLING);
    attachInterrupt(digitalPinToInterrupt(RAIN_PIN), rainISR, FALLING);
  }
}

// ---------- Config Load / Save ----------
void loadConfig() {
  preferences.begin("weather", false);
  // getInt() reports a missing key at log_v, but getString() uses log_e even
  // when a default is supplied - so probe with isKey() to keep a fresh NVS
  // from printing errors that are not errors.
  sleepIntervalSec = preferences.getInt("sleepInt", 300);
  stationCode = preferences.isKey("station")
                ? preferences.getString("station")
                : String("AWS-UG-001");
  preferences.end();
  logOk("Config loaded: interval %d s, station %s", sleepIntervalSec, stationCode.c_str());
}

void saveConfig(int newInterval, const String &newStation) {
  preferences.begin("weather", false);
  if (newInterval > 0) preferences.putInt("sleepInt", newInterval);
  if (newStation.length() > 0) preferences.putString("station", newStation);
  preferences.end();
  if (newInterval > 0) sleepIntervalSec = newInterval;
  if (newStation.length() > 0) stationCode = newStation;
  logOk("Config saved: interval %d s, station %s", sleepIntervalSec, stationCode.c_str());
}

// ---------- Error Logging ----------
// The SD card shares the sensor rail, so an error raised during the config
// portal or a sleep window has nowhere to be written. Hold those in RAM and
// flush them at the next burst rather than losing them.
static String   errorBuffer[ERROR_BUFFER_SLOTS];
static uint8_t  errorBufferCount   = 0;
static uint32_t errorBufferDropped = 0;

static void bufferError(const String &line) {
  if (errorBufferCount < ERROR_BUFFER_SLOTS) errorBuffer[errorBufferCount++] = line;
  else                                       errorBufferDropped++;
}

void flushBufferedErrors() {
  if (errorBufferCount == 0 && errorBufferDropped == 0) return;

  uint8_t flushed = errorBufferCount;
  for (uint8_t i = 0; i < errorBufferCount; i++) {
    appendSD(errorBuffer[i], SD_ERROR_FILE);
    errorBuffer[i] = "";
  }
  if (errorBufferDropped > 0) {
    appendSD(getTimestamp() + " | " + String(errorBufferDropped) +
             " further error(s) dropped - RAM buffer full", SD_ERROR_FILE);
  }
  errorBufferCount   = 0;
  errorBufferDropped = 0;
  logInfo("Flushed %u buffered error(s) to the log.", flushed);
}

// Set while a debug live read is in flight. Those run on demand - potentially
// hundreds of times in one session - so a sensor that is failing would write a
// duplicate error.log line per read and fill the card with noise. The serial
// console still shows every one, which is where you are looking anyway when
// you are standing at the station.
static volatile bool suppressErrorLog = false;

void logError(const String &message) {
  String line = getTimestamp() + " | " + message;
  Serial.println(TAG_FAIL " " + line);
  if (suppressErrorLog) return;
  if (!appendSD(line, SD_ERROR_FILE)) bufferError(line);
}

// ---------- Generic SD Append ----------
bool appendSD(const String &line, const char *filename) {
  File f = SD.open(filename, FILE_APPEND);
  if (!f) return false;
  f.println(line);
  f.close();
  return true;
}

// ================== DIAGNOSTICS ==================
// Why a station stopped is close to impossible to reconstruct once someone has
// power-cycled it - the evidence goes with the RAM. These write it to the card
// as it happens instead: the restart reason at every boot, and the heap every
// time it sets a new low.
//
// The pair is what turns an unexplained stoppage into a measured one. A long
// run of new lows with not one recovery is a leak; a flat trend with the odd
// dip is normal churn. And no restart of any kind in the days beforehand is
// what separates a memory fault from a power fault.

static uint32_t heapLowWater = UINT32_MAX;   // lowest free heap seen this boot

static const char *resetReasonName(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_EXT:       return "external reset pin";
    case ESP_RST_SW:        return "software restart";
    case ESP_RST_PANIC:     return "panic or exception";
    case ESP_RST_INT_WDT:   return "interrupt watchdog";
    case ESP_RST_TASK_WDT:  return "task watchdog";
    case ESP_RST_WDT:       return "other watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep wake";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "unknown";
  }
}

// Once per boot, as soon as both the card and the clock are up, so the line
// lands stamped and on disk before anything else can go wrong.
void logBootDiagnostics() {
  const char *reason = resetReasonName(esp_reset_reason());
  uint32_t freeHeap  = ESP.getFreeHeap();
  uint32_t largest   = ESP.getMaxAllocHeap();

  logInfo("Boot reason: %s (free %u B, largest block %u B)",
          reason, freeHeap, largest);
  appendSD(getTimestamp() + " | BOOT    | reason=" + reason +
           " | free=" + String(freeHeap) + " | largest=" + String(largest),
           SD_DIAG_FILE);

  heapLowWater = freeHeap;   // the trend starts here, not at UINT32_MAX
}

// One line per new low, so a healthy station writes a handful and then goes
// quiet. Needs the card, so it belongs before the rail drops.
void noteHeapLow() {
  uint32_t freeHeap = ESP.getFreeHeap();
  if (freeHeap >= heapLowWater) return;
  heapLowWater = freeHeap;

  appendSD(getTimestamp() + " | HEAP    | free=" + String(freeHeap) +
           " | largest=" + String(ESP.getMaxAllocHeap()) +
           " | queued=" + String(queueDepthCached),
           SD_DIAG_FILE);
}

// The safety net. Does not return if it decides to restart, so where it is
// called from is part of the design - see the call site in loop().
//
// It watches the largest free block rather than the total, because that is what
// an encrypted connection actually needs; see HEAP_MIN_BLOCK_BYTES.
static bool firstCycleSinceBoot = true;

void heapWatchdog() {
  const bool firstCycle = firstCycleSinceBoot;
  firstCycleSinceBoot = false;

  uint32_t largest = ESP.getMaxAllocHeap();
  if (largest >= HEAP_MIN_BLOCK_BYTES) return;

  String what = "largest free block " + String(largest) + " B is under " +
                String((uint32_t)HEAP_MIN_BLOCK_BYTES) + " B";

  // Never restart on the first cycle of a boot. If the block is already this
  // small with the heap as fresh as it will ever be, a restart cannot fix it,
  // and a station wedged in a reboot loop is a worse fault than the one being
  // guarded against. Record it and carry on instead - the station still
  // measures and still archives, which is the part that matters.
  if (firstCycle) {
    appendSD(getTimestamp() + " | WATCHDOG | " + what +
             " on the first cycle after boot - not restarting", SD_DIAG_FILE);
    logWarn("Heap watchdog: %s on the first cycle after boot - not restarting.",
            what.c_str());
    return;
  }

  appendSD(getTimestamp() + " | RESTART | " + what + " - restarting | free=" +
           String(ESP.getFreeHeap()), SD_DIAG_FILE);
  logError("Heap watchdog: " + what + " - restarting.");

  sensorRailDown();          // flush and release the card before the reset
  Serial.flush();
  delay(100);
  ESP.restart();
}

// ---------- WiFi Connection (Preferences first, fallback hardcoded) ----------
bool connectWiFi() {
  // Check if already connected
  if (WiFi.status() == WL_CONNECTED) {
    logOk("WiFi already connected.");
    return true;
  }

  // The radio is powered down between bursts, so the mode has to be put back
  // before begin() will do anything at all.
  if (WiFi.getMode() == WIFI_OFF) WiFi.mode(WIFI_STA);

  // Try to get saved WiFi from Preferences
  preferences.begin("wifi", false);
  String savedSSID     = preferences.isKey("ssid") ? preferences.getString("ssid") : String();
  String savedPassword = preferences.isKey("pass") ? preferences.getString("pass") : String();
  preferences.end();

  // An SSID alone is enough: requiring a password too would send an open
  // network - which a field MiFi may well be - down the fallback path instead.
  // An empty passphrase is what the ESP32 wants for an open AP anyway.
  if (savedSSID.length() > 0) {
    logStep("Using saved WiFi: %s", savedSSID.c_str());
    WiFi.begin(savedSSID.c_str(), savedPassword.c_str());
  } else {
    // Fallback to hard-coded credentials (only if no saved credentials)
    logStep("No saved credentials - using fallback: %s", WIFI_SSID_FALLBACK);
    WiFi.begin(WIFI_SSID_FALLBACK, WIFI_PASSWORD_FALLBACK);
  }

  // Wait for connection
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    logOk("WiFi connected.");
    Serial.print(TAG_INFO " IP address: ");
    Serial.println(WiFi.localIP());
    return true;
  } else {
    Serial.println();
    logFail("WiFi connection failed.");
    return false;
  }
}

// ================== CONFIGURATION PORTAL ==================
// A purpose-built portal: the update interval is the front-page control and
// WiFiManager is one button among several rather than the entire UI.
//
// Served from our own WebServer so the page can be designed freely. WiFiManager
// is still the right tool for provisioning, but it wants port 80 too, so the
// two can never run at once - /api/wifi tears this server down, runs
// WiFiManager, then brings this one back for what remains of the window.

static WebServer portalServer(80);
static DNSServer portalDns;

static bool portalShouldClose    = false;   // "Done" pressed
static bool portalWantsWiFiSetup = false;   // hand off to WiFiManager
static bool portalRestartPending = false;
static bool portalUploadPending  = false;
static int  portalQueueDepth     = 0;       // cached: the SD rail may be off

// Which mode the portal is currently serving. The route handlers need it:
// /api/read is meaningless with the rail down, and /api/debug must not be
// honoured while already in debug.
static PortalMode portalMode      = PORTAL_NORMAL;
static bool       portalWantsDebug = false;  // "Enter debug mode" pressed

// Raised by runConfigPortal() when a session ran in debug mode, and cleared by
// loop(). It survives the call that raised it because it has to drive what
// happens on the OTHER side of the portal: one immediate cycle, with no second
// portal in front of it.
static bool debugSessionEnded = false;

// ---------- Helpers shared with the rest of the firmware ----------
int queuedRecordCount() {
  if (!SD.exists(SD_QUEUE_FILE)) return 0;
  File f = SD.open(SD_QUEUE_FILE, FILE_READ);
  if (!f) return 0;
  int n = 0;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() > 0) n++;
  }
  f.close();
  return n;
}

void setSensorPower(bool on) {
  digitalWrite(RELAY_PIN, on ? RELAY_ACTIVE : !RELAY_ACTIVE);
  relayOn = on;
}

// The sensor rail is power-cycled every duty cycle, so everything on it comes
// back in its reset state. Device-side configuration therefore has to be
// re-established on every power-up, not just once in setup():
//
//   SD      the card needs its full init sequence again. Skip it and the
//           driver still believes it is mounted, so every call fails with
//           "Card Failed! cmd: 0x0d" (CMD13 SEND_STATUS) and writes are lost.
//   INA219  begin() writes the calibration register, and that register lives
//           on the chip. Skip it and the current readings come back silently
//           wrong rather than erroring.
//   ADS1115 gain is held in the library object, but re-begin() is cheap and
//           confirms the device actually answered.
static bool sdMounted = false;

bool mountSD() {
  if (sdMounted) return true;
  if (!SD.begin(SD_CS)) {
    sdMounted = false;
    return false;
  }
  sdMounted = true;
  return true;
}

void unmountSD() {
  if (!sdMounted) return;
  SD.end();          // flush and release the card before it loses power
  sdMounted = false;
}

// Try both addresses the breakout boards use. Forced mode means the chip takes
// one measurement on demand and returns to sleep, rather than free-running.
static bool detectBME280() {
  if (!bme.begin(BME280_ADDR_A) && !bme.begin(BME280_ADDR_B)) return false;
  bme.setSampling(Adafruit_BME280::MODE_FORCED,
                  Adafruit_BME280::SAMPLING_X1,    // temperature (compensation only)
                  Adafruit_BME280::SAMPLING_X1,    // pressure
                  Adafruit_BME280::SAMPLING_NONE,  // humidity - DHT22 owns this
                  Adafruit_BME280::FILTER_OFF);
  return true;
}

void sensorRailUp() {
  setSensorPower(true);
  delay(SENSOR_RAIL_SETTLE_MS);

  // The DS3231 is on this rail too, so re-probe it before anything asks for a
  // timestamp - otherwise rtcOk/rtcTimeValid stay stuck at whatever the last
  // rail-down read concluded.
  {
    RtcBus bus;
    if (rtc.begin()) {
      rtcOk = true;
      refreshRtcValidity();
    } else {
      rtcOk = false;
      logError("RTC silent after power-up.");
    }
  }

  if (!mountSD()) {
    logFail("SD did not remount after power-up.");
  }
  flushBufferedErrors();     // there is somewhere to write again

  if (!batterySensor.begin()) logError("INA219 Battery silent after power-up.");
  if (!solarSensor.begin())   logError("INA219 Solar silent after power-up.");

  if (ads.begin(0x48)) ads.setGain(GAIN_TWOTHIRDS);
  else                 logError("ADS1115 silent after power-up.");

  dht.begin();

  ds18b20.begin();
  ds18b20Ok = (ds18b20.getDeviceCount() > 0);

  bool bmeWasOk = bmeOk;
  bmeOk = detectBME280();
  if (bmeOk && !bmeWasOk) logOk("BME280 detected - pressure now being recorded.");
}

void sensorRailDown() {
  unmountSD();
  setSensorPower(false);
}

static const char *clockSourceName() {
  if (rtcOk && rtcTimeValid) return ntpEverSynced ? "RTC, NTP synced" : "RTC";
  if (ntpEverSynced)         return "NTP only, no RTC";
  return "not set";
}

static void sendJson(bool ok, const String &message) {
  JsonDocument doc;
  doc["ok"] = ok;
  doc["message"] = message;
  String out;
  serializeJson(doc, out);
  portalServer.send(200, "application/json", out);
}

// ---------- Route handlers ----------
static void handleRoot() {
  portalServer.send_P(200, "text/html", PORTAL_INDEX_HTML);
}

static void handleStatus() {
  JsonDocument doc;
  doc["station"]     = stationCode;
  doc["intervalMin"] = sleepIntervalSec / 60;
  doc["time"]        = getTimestamp();
  doc["clockValid"]  = (rtcTimeValid || ntpEverSynced);
  doc["clockSource"] = clockSourceName();
  doc["uptimeSec"]   = (uint32_t)(millis() / 1000);
  doc["queueDepth"]  = portalQueueDepth;
  doc["debug"]       = (portalMode == PORTAL_DEBUG);

  // Negative means "unknown" - the page renders that as "now".
  long next = -1;
  if (rtcOk && rtcTimeValid && nextCycleEpoch > 0) {
    RtcBus bus;
    next = (long)nextCycleEpoch - (long)rtc.now().unixtime();
  }
  doc["nextCycleSec"] = next;

  JsonObject w = doc["wifi"].to<JsonObject>();
  bool up = (WiFi.status() == WL_CONNECTED);
  w["connected"] = up;
  w["ssid"]      = up ? WiFi.SSID() : String("");
  w["ip"]        = up ? WiFi.localIP().toString() : String("");
  w["rssi"]      = up ? WiFi.RSSI() : 0;

  doc["haveReading"] = haveLastRecord;
  if (haveLastRecord) {
    doc["temp"]     = lastRecord.temperature;
    doc["humidity"] = lastRecord.humidity;
    doc["windMS"]   = lastRecord.windMS;
    doc["gustMS"]   = lastRecord.windGustMS;
    doc["rain"]     = lastRecord.rainInterval_mm;
    doc["battV"]    = lastRecord.batteryVoltage;
    doc["battI"]    = lastRecord.batteryCurrent_mA;
    doc["solarV"]   = lastRecord.solarVoltage;
    doc["solarI"]   = lastRecord.solarCurrent_mA;
  }

  String out;
  serializeJson(doc, out);
  portalServer.send(200, "application/json", out);
}

static void handleSettings() {
  int    newInterval = 0;    // 0 = leave unchanged
  String newStation  = "";   // "" = leave unchanged
  String msg         = "";
  bool   ok          = true;

  // ---- Update interval (minutes in the UI, seconds in NVS) ----
  if (portalServer.hasArg("interval")) {
    String v = portalServer.arg("interval");
    v.trim();
    if (v.length() > 0) {
      int mins = v.toInt();
      if (mins <= 0) {
        // toInt() yields 0 for junk, so this catches typos that would
        // otherwise look exactly like a successful save.
        ok = false;
        msg += "Interval must be a whole number of minutes. ";
      } else {
        int secs = mins * 60;
        if (secs < SLEEP_MIN_SEC) secs = SLEEP_MIN_SEC;
        if (secs > SLEEP_MAX_SEC) secs = SLEEP_MAX_SEC;
        if (secs != sleepIntervalSec) {
          newInterval = secs;
          msg += "Interval set to " + String(secs / 60) + " min. ";
        } else {
          msg += "Interval unchanged. ";
        }
      }
    }
  }

  // ---- Station code ----
  if (portalServer.hasArg("station")) {
    String v = portalServer.arg("station");
    v.trim();
    if (v.length() > 0 && v != stationCode) {
      newStation = v;
      msg += "Station code set to " + v + ". ";
    }
  }

  if (newInterval > 0 || newStation.length() > 0) {
    saveConfig(newInterval, newStation);
  }

  // ---- Manual clock set (blank keeps the current time) ----
  if (portalServer.hasArg("clock")) {
    String v = portalServer.arg("clock");
    v.trim();
    if (v.length() > 0) {
      if (setRtcFromString(v)) {
        msg += "Clock set to " + getTimestamp() + ". ";
      } else {
        ok = false;
        msg += "Clock rejected - use YYYY-MM-DD HH:MM:SS. ";
        logError("Portal clock value rejected: " + v);
      }
    }
  }

  if (msg.length() == 0) msg = "Nothing to change.";
  sendJson(ok, msg);
}

static void handleWiFiSetup() {
  portalWantsWiFiSetup = true;
  sendJson(true, "Starting WiFi setup - rejoin " PORTAL_AP_SSID);
}

static void handleUpload() {
  if (WiFi.status() != WL_CONNECTED) {
    sendJson(false, "Not connected to WiFi - configure a network first");
    return;
  }
  portalUploadPending = true;
  sendJson(true, "Upload started - watch the queued count");
}

static void handleRestart() {
  portalRestartPending = true;
  sendJson(true, "Restarting...");
}

static void handleDone() {
  portalShouldClose = true;
  sendJson(true, portalMode == PORTAL_DEBUG
                 ? "Leaving debug mode - taking a reading now"
                 : "Portal closing - station going back to sleep");
}

static void handleDebugEnter() {
  if (portalMode == PORTAL_DEBUG) {
    sendJson(false, "Already in debug mode");
    return;
  }
  portalWantsDebug = true;
  sendJson(true, "Entering debug mode - the sensor rail stays up");
}

// Defined further down with the JSON builders, but needed here: an absent key
// is how a failed sensor stays distinguishable from a real zero, and that
// matters more in a diagnostic than anywhere else.
static void putIfReal(JsonDocument &doc, const char *key, float v);

// A live, non-destructive read - the reason debug mode exists.
//
// It deliberately goes through readSensors(), the same path a burst uses,
// rather than a lighter read of its own. A diagnostic that reads differently
// from the real thing validates nothing: the DHT retry behaviour, the forced
// BME280 conversion and the INA219 calibration all have to be the ones that
// will run when the record is actually built.
//
// Nothing here archives, enqueues or uploads.
static void handleDebugRead() {
  if (portalMode != PORTAL_DEBUG) {
    // Outside debug the sensor rail is powered down, so a read would return
    // whatever a dead I2C bus looks like.
    sendJson(false, "Live reads are only available in debug mode");
    return;
  }

  // readSensors() has no side effects on the pulse counters - the reset lives
  // in snapshotPulseCounters(), which only a burst calls - so this can run as
  // often as it is asked without disturbing the interval, the slices or the
  // gust window.
  DataRecord live;
  suppressErrorLog = true;
  readSensors(live);
  suppressErrorLog = false;

  JsonDocument doc;
  doc["ok"]   = true;
  doc["time"] = live.timestamp;

  putIfReal(doc, "temp",     live.temperature);
  putIfReal(doc, "humidity", live.humidity);
  putIfReal(doc, "pressure", live.pressure);
  putIfReal(doc, "battTemp", live.batteryTemp);
  putIfReal(doc, "solarV",   live.solarVoltage);
  putIfReal(doc, "solarI",   live.solarCurrent_mA);
  putIfReal(doc, "battV",    live.batteryVoltage);
  putIfReal(doc, "battI",    live.batteryCurrent_mA);

  // Raw volts alongside the derived values, for the same reason the record
  // carries both: the volts are ground truth. Watching AIN0 while turning the
  // vane is how VANE_V_MIN/MAX get verified, and that cannot be done from a
  // heading that was computed with the constants under suspicion.
  putIfReal(doc, "adc0", live.adc0);
  putIfReal(doc, "adc1", live.adc1);
  putIfReal(doc, "adc2", live.adc2);
  putIfReal(doc, "adc3", live.adc3);

  int dir = vaneDirectionInt(live.adc0);
  if (dir >= 0) doc["vaneDeg"] = dir;
  putIfReal(doc, "soilPct",    soilMoisturePct(live.adc1));
  putIfReal(doc, "irradiance", irradianceWm2(live.adc2));

  // readSensors() copies windMS / windGustMS / rain_mm from the last SNAPSHOT.
  // Rendering those here would show an interval-old number labelled "live" -
  // exactly the confusion debug mode exists to remove. Report the running
  // counters instead, with the window they cover so the figure can be judged.
  noInterrupts();
  uint32_t wp = windPulseCount;
  uint32_t rp = rainPulseCount;
  interrupts();

  float since = (float)(millis() - lastSnapshotMillis) / 1000.0f;
  if (since < 1.0f) since = 1.0f;

  doc["windPulses"] = wp;
  doc["rainTips"]   = rp;
  doc["sinceSec"]   = (uint32_t)since;
  doc["windMS"]     = ((float)wp / since) * WIND_MS_PER_HZ;
  doc["rainMM"]     = (float)rp * MM_PER_TIP;

  String out;
  serializeJson(doc, out);
  portalServer.send(200, "application/json", out);
}

static void handleNotFound() {
  // Captive-portal probes land here; bounce them to the real page.
  portalServer.sendHeader("Location",
                          String("http://") + WiFi.softAPIP().toString() + "/", true);
  portalServer.send(302, "text/plain", "");
}

// ---------- Server lifecycle ----------
static void startPortalServer() {
  portalServer.on("/",             HTTP_GET,  handleRoot);
  portalServer.on("/api/status",   HTTP_GET,  handleStatus);
  portalServer.on("/api/settings", HTTP_POST, handleSettings);
  portalServer.on("/api/wifi",     HTTP_POST, handleWiFiSetup);
  portalServer.on("/api/upload",   HTTP_POST, handleUpload);
  portalServer.on("/api/restart",  HTTP_POST, handleRestart);
  portalServer.on("/api/done",     HTTP_POST, handleDone);
  portalServer.on("/api/debug",    HTTP_POST, handleDebugEnter);
  portalServer.on("/api/read",     HTTP_GET,  handleDebugRead);
  portalServer.onNotFound(handleNotFound);
  portalServer.begin();
}

static void bringUpAccessPoint() {
  WiFi.mode(WIFI_AP_STA);          // keep STA so uploads still work while open
  WiFi.softAP(PORTAL_AP_SSID);
  delay(150);
  portalDns.start(53, "*", WiFi.softAPIP());
}

// Hand control to WiFiManager for provisioning, then take it back.
//
// timeoutSec is passed in rather than read from PORTAL_TIMEOUT because the
// short unattended window and a debug session want opposite things: 200 s is
// what closes the network form under you in the field, and in debug the budget
// is whatever is left of the mode's ceiling. It is never 0 - WiFiManager would
// then block forever, and the ceiling that exists to stop a station being
// abandoned mid-service would be defeated by the one screen you sit on longest.
static void runWiFiManagerHandoff(uint32_t timeoutSec) {
  logStep("Handing off to WiFiManager for WiFi setup (%lu s)...",
          (unsigned long)timeoutSec);
  portalServer.stop();
  portalDns.stop();
  delay(200);

  WiFiManager wm;
  wm.setConfigPortalTimeout(timeoutSec);
  wm.setAPCallback([](WiFiManager *) {
    logInfo("WiFiManager AP up: " PORTAL_AP_SSID);
  });

  // Same SSID as our own portal, so the phone reconnects to a familiar name.
  bool got = wm.startConfigPortal(PORTAL_AP_SSID, NULL);

  // WiFiManager leaves the credentials in the esp_wifi NVS area. connectWiFi()
  // does not read that area - it reads the "wifi" Preferences namespace - and
  // its explicit WiFi.begin() overwrites the esp_wifi copy with the fallback on
  // the very next burst. So without this copy a network changed here is
  // reported as saved, then silently discarded one cycle later.
  if (got) {
    String newSsid = wm.getWiFiSSID(true);
    String newPass = wm.getWiFiPass(true);
    if (newSsid.length() > 0) {
      preferences.begin("wifi", false);
      preferences.putString("ssid", newSsid);
      preferences.putString("pass", newPass);
      preferences.end();
      logOk("New WiFi credentials saved: %s", newSsid.c_str());
    } else {
      logWarn("WiFiManager connected but reported no SSID - nothing saved, "
              "the station will fall back to " WIFI_SSID_FALLBACK ".");
    }
  } else {
    logInfo("WiFi setup closed without changes.");
  }

  bringUpAccessPoint();
  startPortalServer();
}

static void doPortalUpload() {
  logStep("Forced upload from portal...");
  // In debug the rail is already up and has to stay up - the live reads depend
  // on it. Only take ownership of it when the portal found it down.
  const bool ownRail = (portalMode != PORTAL_DEBUG);
  if (ownRail) sensorRailUp();   // the SD card shares the gated rail
  processPendingQueue();
  portalQueueDepth = queueDepthCached;
  if (ownRail) sensorRailDown();
  logOk("Forced upload done, %d record(s) still queued.", portalQueueDepth);
}

// ---------- The portal itself ----------
void runConfigPortal(PortalMode mode) {
  // Not const: the portal can be promoted to debug in place, without the AP
  // going down under the phone that just asked for it.
  bool debug = (mode == PORTAL_DEBUG);
  portalMode = mode;

  if (debug) {
    logInfo("DEBUG MODE - rail stays up, no config timeout, %d min ceiling.",
            DEBUG_MAX_SEC / 60);
    // loop() drops the rail before the portal runs, and a live read with the
    // rail down returns whatever a dead I2C bus looks like.
    sensorRailUp();
  } else {
    logInfo("Config portal open for %d s - join \"%s\", then browse to http://192.168.4.1",
                  PORTAL_TIMEOUT, PORTAL_AP_SSID);
  }

  portalShouldClose    = false;
  portalWantsWiFiSetup = false;
  portalRestartPending = false;
  portalUploadPending  = false;
  portalWantsDebug     = false;

  // With the rail up the queue can be counted for real. Outside debug, use the
  // cached depth: powering the rail back up here would mean another full SD
  // init cycle purely to count lines we already counted.
  portalQueueDepth = debug ? queuedRecordCount() : queueDepthCached;

  wifi_mode_t previousMode = WiFi.getMode();
  bringUpAccessPoint();
  startPortalServer();

  // Two clocks, deliberately separate. `start` is the short unattended window
  // and is reset by a WiFiManager detour, because coming back from one should
  // not leave you seconds from a close. `entered` is the debug ceiling and is
  // never reset - a dead-man switch that any interaction could postpone would
  // not be one.
  unsigned long start     = millis();
  unsigned long entered   = millis();
  unsigned long windowMs  = (unsigned long)PORTAL_TIMEOUT * 1000UL;
  unsigned long ceilingMs = (unsigned long)DEBUG_MAX_SEC * 1000UL;

  while (true) {
    if (debug) {
      if ((millis() - entered) >= ceilingMs) {
        logWarn("Debug ceiling of %d min reached - resuming normal operation.",
                DEBUG_MAX_SEC / 60);
        break;
      }
    } else if ((millis() - start) >= windowMs) {
      break;
    }

    portalDns.processNextRequest();
    portalServer.handleClient();

    if (portalWantsWiFiSetup) {
      portalWantsWiFiSetup = false;
      uint32_t budget = PORTAL_TIMEOUT;
      if (debug) {
        // Whatever is left of the ceiling. Not unlimited: this is the screen
        // you sit on longest, so it is exactly where an abandoned session
        // would otherwise escape the dead-man switch.
        long leftMs = (long)ceilingMs - (long)(millis() - entered);
        budget = (leftMs > 1000) ? (uint32_t)(leftMs / 1000) : 30;
      }
      runWiFiManagerHandoff(budget);
      start = millis();          // fresh window after the detour
    }
    if (portalUploadPending) {
      portalUploadPending = false;
      doPortalUpload();
    }
    if (portalRestartPending) {
      logInfo("Restarting on portal request.");
      delay(300);                // let the response flush first
      ESP.restart();
    }
    if (portalWantsDebug) {
      portalWantsDebug = false;
      // Promote in place. Tearing the server down and re-entering would drop
      // the AP and make the phone rejoin, which is precisely the friction this
      // mode exists to remove - and the request came from that phone.
      debug      = true;
      portalMode = PORTAL_DEBUG;
      entered    = millis();     // the ceiling starts when debug starts
      sensorRailUp();            // live reads need power
      portalQueueDepth = queuedRecordCount();   // countable now the rail is up
      logInfo("DEBUG MODE - rail stays up, no config timeout, %d min ceiling.",
              DEBUG_MAX_SEC / 60);
    }
    if (portalShouldClose) {
      // %s, not the ternary directly: logOk() concatenates its format with the
      // tag as adjacent string literals, which only works for a literal.
      logOk("%s", debug ? "Debug mode closed by user." : "Portal closed by user.");
      break;
    }
    maybeCloseBuckets();   // the portal can hold the CPU for a long time
    delay(2);
  }

  portalServer.stop();
  portalDns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(previousMode);
  portalMode = PORTAL_NORMAL;

  // Tells loop() a debug session just finished, whichever way it was entered -
  // the portal button or a BOOT hold at power-up. One flag covers both because
  // what has to happen next is the same: one cycle, immediately, no portal.
  if (debug) debugSessionEnded = true;

  logInfo("Active settings: interval %d min, station %s, clock %s",
                sleepIntervalSec / 60, stationCode.c_str(), getTimestamp().c_str());
  logOk("%s", debug ? "Debug mode finished." : "Configuration portal finished.");
}

// ================== Display Sensor Values ==================
void printSensorValues(const DataRecord &rec) {
  char buf[UI_VALUE_W + 1];

  Serial.println();
  uiTitle("SENSOR READINGS");

  uiRow("Timestamp", rec.timestamp.c_str());
  uiRowInt("Interval", (long)rec.intervalSec, "s");

  snprintf(buf, sizeof(buf), "%8.2f  m/s   (%.2f mph)", rec.windMS, rec.windMPH);
  uiRow("Wind avg", buf);
  uiRowNum("Wind gust", rec.windGustMS, 2, "m/s");
  uiRowNum("Rain", rec.rainInterval_mm, 2, "mm", "this interval");

  // The raw counts everything above is derived from. Summed from the slices
  // rather than kept separately - if these two lines ever disagree with the
  // derived values, the constants are the thing to doubt, not the counting.
  uint32_t windSum = 0, rainSum = 0;
  for (uint8_t i = 0; i < rec.bucketCount; i++) {
    windSum += rec.bucketWind[i];
    rainSum += rec.bucketRain[i];
  }
  snprintf(buf, sizeof(buf), "%6lu pulse %4lu tip",
           (unsigned long)windSum, (unsigned long)rainSum);
  uiRow("Raw counts", buf);

  snprintf(buf, sizeof(buf), "%6u x %d s", rec.bucketCount, rec.bucketSec);
  uiRow("Slices", buf);

  if (rec.gustCount > 0) {
    snprintf(buf, sizeof(buf), "%6lu in %lu ms",
             (unsigned long)rec.gustCount, (unsigned long)rec.gustSpanMs);
  } else {
    snprintf(buf, sizeof(buf), "%6s  none closed", "--");
  }
  uiRow("Gust window", buf);

  uiRowNum("Air temp", rec.temperature, 2, "C");
  uiRowNum("Humidity", rec.humidity, 1, "%RH");
  uiRowNum("Pressure", rec.pressure, 2, "hPa", bmeOk ? "" : "(no BME280)");
  uiRowNum("Batt temp", rec.batteryTemp, 2, "C");

  snprintf(buf, sizeof(buf), "%8.3f V %8.2f mA", rec.solarVoltage, rec.solarCurrent_mA);
  uiRow("Solar", buf);
  snprintf(buf, sizeof(buf), "%8.3f V %8.2f mA", rec.batteryVoltage, rec.batteryCurrent_mA);
  uiRow("Battery", buf);

  uiRule();

  int dirDeg = vaneDirectionInt(rec.adc0);
  if (dirDeg >= 0) uiRowInt("Wind dir", (long)dirDeg, "deg");
  else             uiRow("Wind dir", "      --  deg   (out of range)");
  uiRowNum("Soil", soilMoisturePct(rec.adc1), 1, "%");
  uiRowNum("Irradiance", irradianceWm2(rec.adc2), 1, "W/m2");

  snprintf(buf, sizeof(buf), "%8.4f V %8.4f V", rec.adc0, rec.adc1);
  uiRow("raw AIN0/1", buf);
  snprintf(buf, sizeof(buf), "%8.4f V %8.4f V", rec.adc2, rec.adc3);
  uiRow("raw AIN2/3", buf);

  uiRule();
  Serial.println();
}


// ---------- SETUP ----------
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println();
  Serial.println(C_BOLD "=== ESP32 Weather Station (WIMEA API) ===" C_RESET);

  loadConfig();

  // BOOT (GPIO0) is the way into debug mode when the portal is not open - a
  // station whose burst is wedged, or one on a long interval you do not want to
  // wait out. Watched for a few seconds rather than sampled once, so the press
  // does not have to be timed to the millisecond as it used to.
  //
  // Deliberately a HOLD, not a tap: brushing the button while closing the
  // enclosure must not leave the station sitting in debug instead of logging.
  //
  // The gesture is power on FIRST, then press. GPIO0 held low at the instant of
  // reset puts the ESP32 into its serial bootloader, where it looks dead.
  pinMode(BOOT_PIN, INPUT_PULLUP);
  delay(10);
  logInfo("Hold BOOT within %d s to enter debug mode...", BOOT_WATCH_MS / 1000);

  bool bootHeld = false;
  unsigned long watchStart = millis();
  while ((millis() - watchStart) < BOOT_WATCH_MS && !bootHeld) {
    if (digitalRead(BOOT_PIN) == LOW) {
      unsigned long downSince = millis();
      while (digitalRead(BOOT_PIN) == LOW) {
        if ((millis() - downSince) >= BOOT_HOLD_MS) { bootHeld = true; break; }
        delay(10);
      }
    }
    delay(10);
  }

  // ---- Hardware Init ----
  pinMode(RELAY_PIN, OUTPUT);
  setSensorPower(true);
  delay(500);

  Wire.begin(I2C_SDA, I2C_SCL);

  // ---- SD Card ----
  // Mounted first so that every logError() below actually reaches error.log.
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!mountSD()) {
    // Can't logError() this one - there is nowhere to write it.
    logFail("SD card mount failed (CS=5).");
  } else {
    logOk("SD card mounted.");
    if (!SD.exists(SD_DATA_FILE)) {
      File f = SD.open(SD_DATA_FILE, FILE_WRITE);
      if (f) {
        f.println(SD_CSV_HEADER);
        f.close();
        logInfo("CSV header created.");
      }
    }
    if (!SD.exists(SD_ERROR_FILE)) {
      File f = SD.open(SD_ERROR_FILE, FILE_WRITE);
      if (f) { f.println("--- Error Log Started ---"); f.close(); }
    }
    if (!SD.exists(SD_QUEUE_FILE)) {
      File f = SD.open(SD_QUEUE_FILE, FILE_WRITE);
      if (f) { f.close(); }
    }
    if (!SD.exists(SD_DIAG_FILE)) {
      File f = SD.open(SD_DIAG_FILE, FILE_WRITE);
      if (f) { f.println("--- Diagnostics Log Started ---"); f.close(); }
    }
  }

  // ---- I2C bus scan ----
#if I2C_SCAN_ON_BOOT
  scanI2C("normal  (SDA 21 / SCL 22)");
  { RtcBus bus; scanI2C("swapped (SDA 22 / SCL 21)"); }
#endif

  // ---- RTC ----
  // Second, so the sensor errors below get stamped with a real time.
  {
    RtcBus bus;
    if (rtc.begin()) {
      rtcOk = true;
      logOk("RTC found.");
      if (rtc.lostPower()) {
        // Flat coin cell, or first power-up of a new board.
        rtcTimeValid = false;
        logWarn("RTC lost power - time is not trustworthy until NTP sync.");
      } else {
        refreshRtcValidity();
      }
      logInfo("RTC reads %s EAT (%s)", getTimestamp().c_str(),
                    rtcTimeValid ? "valid" : "NEEDS SYNC");
    } else {
      rtcOk = false;
      rtcTimeValid = false;
      logError("RTC not found.");
    }
  }

  // ---- Boot diagnostics ----
  // Card and clock are both up, so the line lands stamped. Deliberately ahead of
  // the sensors: a station that browns out halfway through init should still
  // have managed to say why it restarted.
  logBootDiagnostics();

  // ---- Remaining I2C sensors ----
  if (ads.begin(0x48)) { ads.setGain(GAIN_TWOTHIRDS); logOk("ADS1115 found."); }
  else logError("ADS1115 not found.");

  if (batterySensor.begin()) logOk("INA219 battery found.");
  else logError("INA219 Battery not found.");
  if (solarSensor.begin()) logOk("INA219 solar found.");
  else logError("INA219 Solar not found.");

  dht.begin();
  logOk("DHT22 ready.");

  pinMode(WIND_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(WIND_PIN), windISR, FALLING);
  pinMode(RAIN_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(RAIN_PIN), rainISR, FALLING);
  // Counting starts now, so this is the start of the first interval - and of its
  // first slice.
  lastSnapshotEpoch  = currentEpoch();
  lastSnapshotMillis = millis();
  bucketStartMillis  = lastSnapshotMillis;
  logOk("Wind and rain interrupts armed.");

  // ---- BME280 (pressure) ----
  bmeOk = detectBME280();

  // ---- DS18B20 ----
  ds18b20.begin();
  if (ds18b20.getDeviceCount() > 0) {
    ds18b20Ok = true;
    logOk("DS18B20 found.");
  } else {
    ds18b20Ok = false;
    logFail("DS18B20 not found.");
    logError("DS18B20 not found on pin " + String(DS18B20_PIN));
  }

  // ---- WiFi Connection ----
  WiFi.mode(WIFI_STA);
  bool wifiConnected = connectWiFi();   // Preferences first, fallback second

  // ---- Clock ----
  if (wifiConnected) {
    syncTimeFromNTP();
  }

  // ---- BOOT Button: Field Debug Mode ----
  // No explicit "run a cycle after this" is needed: setup() returning drops
  // straight into loop() at PHASE 1, which is exactly that.
  if (bootHeld) {
    logInfo("BOOT held - entering debug mode.");
    runConfigPortal(PORTAL_DEBUG);
    // Credentials may have changed in there.
    wifiConnected = connectWiFi();
    maybeResyncRTC();   // no-op if the portal already set the clock by hand
  }

  // ---- If still no WiFi, print warning ----
  if (!wifiConnected) {
    logError("No WiFi available at boot.");
  }

  if (!rtcTimeValid && !ntpEverSynced) {
    logError("Clock not set - readings will be logged to SD but not transmitted. "
             "Set it via the config portal or connect to WiFi.");
  }

  // Seed the schedule and the queue depth so the portal is accurate on cycle one.
  nextCycleMillis = millis() + (unsigned long)sleepIntervalSec * 1000UL;
  uint32_t bootEpoch = currentEpoch();
  if (bootEpoch > 0) nextCycleEpoch = bootEpoch + (uint32_t)sleepIntervalSec;
  queueDepthCached = queuedRecordCount();

  Serial.println(bmeOk ? TAG_OK   " BME280 present - pressure will be recorded."
                       : TAG_INFO " No BME280 fitted - pressure stays empty until one is plugged in.");
  Serial.println();
  Serial.println(C_BOLD "--- Entering Main Loop ---" C_RESET);
  Serial.println();
}

// ---------- LOOP (one duty cycle, then back to sleep) ----------
void loop() {
  // ============================================
  // PHASE 1: MEASURE (sensor rail up, radio still down)
  // ============================================
  sensorRailUp();             // power up, remount SD, re-init the I2C devices

  // Close out the interval: snapshot and reset the wind/rain counters, then
  // derive average wind, gust and accumulated rainfall from them.
  snapshotPulseCounters();

  // Fix the next burst time now, while the rail is up and the RTC is
  // reachable. Re-deriving it here every cycle means millis() drift over one
  // interval never accumulates across cycles.
  nextCycleMillis = millis() + (unsigned long)sleepIntervalSec * 1000UL;
  uint32_t burstEpoch = currentEpoch();
  nextCycleEpoch = (burstEpoch > 0) ? burstEpoch + (uint32_t)sleepIntervalSec : 0;

  DataRecord rec;
  readSensors(rec);
  lastRecord     = rec;       // the config portal reads this
  haveLastRecord = true;
  printSensorValues(rec);

  // The archive is permanent and takes everything - even an unstamped record,
  // because the readings are still real and the sentinel timestamp makes the
  // gap obvious. The upload queue only takes what the server can accept.
  archiveRecord(rec);
  if (rec.timestamp == TIMESTAMP_INVALID) {
    logError("Clock not set - record archived but not queued for upload.");
  } else {
    enqueueRecord(rec);
  }
  logOk("Record written to SD.");

  // The safety net, checked here rather than after the uploads for two reasons.
  // The reading is already archived and queued, and snapshotPulseCounters()
  // zeroed the wind and rain counters a moment ago at the top of this cycle, so
  // a restart from here forfeits almost nothing - whereas one after PHASE 2
  // would throw away every pulse counted during it. And nothing frees memory
  // during the sleep window, so the figure here is the one the end of the last
  // cycle would have given; acting on it now means acting BEFORE a round of
  // uploads that a block this small is unlikely to survive anyway.
  //
  // Deliberately above the #if below: the card has to still be mounted, in
  // either rail configuration, for the restart to be logged.
  heapWatchdog();

#if SD_INDEPENDENT_POWER
  sensorRailDown();
  logStep("Sensor rail OFF.");
#endif

  // ============================================
  // PHASE 2: TRANSMIT
  // ============================================
  // The queue drain covers the record just written as well as any backlog, so
  // there is no separate "send the current reading" step to duplicate it.
  if (connectWiFi()) {
    maybeResyncRTC();         // opportunistic: first chance, then once a day
    if (!processPendingQueue()) {
      logWarn("Queue not fully drained - the rest waits for next cycle.");
    }
  } else {
    logError("No WiFi this cycle - the queue is left untouched.");
  }

  // The end of a cycle is the heap at its worst - the uploads have just run -
  // which is the reading the trend wants. Needs the card, so it goes before the
  // rail drops. The watchdog that acts on this trend ran at the top of the
  // cycle; see the note there.
  noteHeapLow();

  sensorRailDown();
  logStep("Sensor rail OFF.");

  // ============================================
  // PHASE 3: CONFIG PORTAL
  // ============================================
  if (debugSessionEnded) {
    // This cycle exists only because a debug session ended - either the portal
    // one just below on the previous pass, or a BOOT hold back in setup().
    // Opening a portal in front of it would mean closing the same page twice
    // to get the station to sleep.
    debugSessionEnded = false;
    logStep("Portal skipped - this cycle follows a debug session.");
  } else {
    runConfigPortal(PORTAL_NORMAL);

    // The portal can promote itself to debug mode. If it did, everything
    // measured at the top of this cycle predates the servicing, so the cycle is
    // restarted rather than sent. Whoever was working on the station is still
    // standing next to it, and the fresh burst is what proves the whole
    // pipeline - sensors, SD, WiFi, the server - before they walk away. It is
    // also the only way to field-test a WiFi network changed a moment ago.
    //
    // The rail is deliberately left up: PHASE 1 re-initialises it in a few
    // hundred ms rather than power-cycling the SD card for nothing.
    if (debugSessionEnded) {
      logStep("Debug mode over - taking a reading now.");
      return;                // straight back to PHASE 1, skipping the sleep
    }
  }

  // ============================================
  // PHASE 4: LIGHT SLEEP (still counting pulses)
  // ============================================
  // Default state from here until the next burst. The CPU is clock-gated but
  // RAM and peripherals survive, and a wind or rain pulse wakes it briefly to
  // count before going straight back down.
  //
  // The deadline was fixed at the top of this burst, while the rail was still
  // up. Nothing below reads the RTC, so this works whether or not the DS3231
  // shares the gated rail.
  long sleepLeft = (long)(nextCycleMillis - millis()) / 1000;
  if (sleepLeft < 0) sleepLeft = 0;
  logStep("Sleeping %ld s until the next burst (wind/rain still counted)...",
                sleepLeft);
  Serial.flush();   // the UART has to drain before the clock stops

  while ((long)(millis() - nextCycleMillis) < 0) {
    sleepUntilNextEventOrDeadline();
    maybeCloseBuckets();     // every wake is a chance to close a slice on time
  }

  logStep("Sleep period finished.");
  Serial.println();
}

// ================== SENSOR READING ==================
void readSensors(DataRecord &rec) {
  rec.timestamp = getTimestamp();

  // ---- Wind & rain ----
  // Already computed by snapshotPulseCounters() at the top of the duty cycle;
  // reading them here would reset the counters a second time.
  rec.windMPH      = windSpeed_MPH;
  rec.windMS       = windSpeed_MS;
  rec.windGustMS   = windGust_MS;
  rec.rainInterval_mm = rain_mm;

  // Whole arrays, not just the populated head: rec is a stack object, and the
  // tail would otherwise carry whatever was there before into the CSV.
  memcpy(rec.bucketWind, lastBucketWind, sizeof(rec.bucketWind));
  memcpy(rec.bucketRain, lastBucketRain, sizeof(rec.bucketRain));
  rec.bucketCount = lastBucketCount;
  rec.bucketSec   = BUCKET_SEC;
  rec.gustCount   = windGustCount;
  rec.gustSpanMs  = windGustSpanMs;

  // ---- DHT22 (Air Temperature & Humidity) ----
  // Retry rather than discard a whole interval's temperature on one bad
  // checksum. The spacing also covers the sensor's power-up settling time,
  // which the rail power-cycle makes relevant on every burst.
  float t = NAN, h = NAN;
  int attempt = 1;
  for (; attempt <= DHT_READ_ATTEMPTS; attempt++) {
    t = dht.readTemperature();
    h = dht.readHumidity();
    if (!isnan(t) && !isnan(h)) break;
    if (attempt < DHT_READ_ATTEMPTS) delay(DHT_RETRY_DELAY_MS);
  }

  if (isnan(t) || isnan(h)) {
    logError("DHT22 read failed after " + String(DHT_READ_ATTEMPTS) +
             " attempts - check VCC (3.3-6 V), the data pull-up and the pin.");
    rec.temperature = NAN;      // NAN travels all the way to a NULL column
    rec.humidity    = NAN;
  } else {
    if (attempt > 1) logWarn("DHT22 needed %d attempts.", attempt);
    rec.temperature = t;
    rec.humidity    = h;
  }

  // ---- BME280 (station pressure) ----
  // Absent for now, so this stays NAN and the column stays NULL.
  if (bmeOk && bme.takeForcedMeasurement()) {
    float pa = bme.readPressure();                 // library returns pascals
    rec.pressure = isnan(pa) ? NAN : (pa / 100.0f);  // hPa
  } else {
    rec.pressure = NAN;
  }

  // ---- DS18B20 (Battery Temperature) ----
  if (ds18b20Ok) {
    ds18b20.requestTemperatures();
    float battTemp = ds18b20.getTempCByIndex(0);
    if (battTemp != DEVICE_DISCONNECTED_C) {
      rec.batteryTemp = battTemp;
    } else {
      logError("DS18B20 read failed (disconnected).");
      rec.batteryTemp = NAN;
      ds18b20Ok = false;
    }
  } else {
    rec.batteryTemp = NAN;
  }

  // ---- ADS1115 ----
  for (int i = 0; i < 4; i++) {
    int16_t raw = ads.readADC_SingleEnded(i);
    float v = ads.computeVolts(raw);
    switch (i) {
      case 0: rec.adc0 = v; break;
      case 1: rec.adc1 = v; break;
      case 2: rec.adc2 = v; break;
      case 3: rec.adc3 = v; break;
    }
  }

  // ---- INA219 Solar ----
  float sV = solarSensor.getBusVoltage_V();
  float sA = solarSensor.getCurrent_mA();
  if (isnan(sV) || isnan(sA)) {
    logError("INA219 Solar read failed.");
    rec.solarVoltage = NAN;     // 0.0 would read as a genuine measurement
    rec.solarCurrent_mA = NAN;
  } else {
    rec.solarVoltage = sV;
    rec.solarCurrent_mA = sA;
  }

  // ---- INA219 Battery ----
  float bV = batterySensor.getBusVoltage_V();
  float bA = batterySensor.getCurrent_mA();
  if (isnan(bV) || isnan(bA)) {
    logError("INA219 Battery read failed.");
    rec.batteryVoltage = NAN;
    rec.batteryCurrent_mA = NAN;
  } else {
    rec.batteryVoltage = bV;
    rec.batteryCurrent_mA = bA;
  }

  rec.intervalSec = (uint32_t)(lastIntervalSec + 0.5f);
  rec.sentMask    = 0;
  rec.retries     = 0;
}


// ================== CLOCK ==================
// Everything here works in East Africa Time (UTC+3). The DS3231 stores EAT
// directly, so SD rows and API payloads need no conversion.

static void formatDateTime(char *buf, size_t len,
                           int y, int mo, int d, int h, int mi, int s) {
  snprintf(buf, len, "%04d-%02d-%02d %02d:%02d:%02d", y, mo, d, h, mi, s);
}

// ---------- Timestamp ----------
// Returns TIMESTAMP_INVALID rather than a plausible-looking lie when the clock
// was never set - a wrong timestamp is worse than an obviously missing one.
String getTimestamp() {
  char buf[20];

  // currentEpoch() reads the DS3231 when its rail is up and carries the last
  // good reading forward on millis() when it is not.
  uint32_t e = currentEpoch();
  if (e > 0) {
    DateTime t(e);
    formatDateTime(buf, sizeof(buf), t.year(), t.month(), t.day(),
                   t.hour(), t.minute(), t.second());
    return String(buf);
  }

  // Fallback: the ESP32's own clock. Only trustworthy once NTP has answered
  // this boot, and it does not survive a power cut - hence the RTC.
  if (ntpEverSynced) {
    struct tm t;
    if (getLocalTime(&t, 0) && (t.tm_year + 1900) >= RTC_MIN_VALID_YEAR) {
      formatDateTime(buf, sizeof(buf), t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                     t.tm_hour, t.tm_min, t.tm_sec);
      return String(buf);
    }
  }

  return String(TIMESTAMP_INVALID);
}

// ---------- Is the RTC holding a believable time? ----------
bool refreshRtcValidity() {
  if (!rtcOk || !relayOn) {
    // Nothing to test with the rail down - leave the previous verdict alone.
    return rtcTimeValid;
  }
  RtcBus bus;
  DateTime now = rtc.now();
  rtcTimeValid = now.isValid() && now.year() >= RTC_MIN_VALID_YEAR;
  if (rtcTimeValid) {
    clockRefEpoch  = now.unixtime();
    clockRefMillis = millis();
  }
  return rtcTimeValid;
}

// ---------- NTP -> DS3231 ----------
bool syncTimeFromNTP() {
  if (WiFi.status() != WL_CONNECTED) return false;

  lastNtpAttemptMs = millis();
  logStep("Requesting time from NTP...");
  // The offset makes getLocalTime() hand back EAT directly, which is exactly
  // what we want to write into the DS3231.
  configTime(TZ_OFFSET_SEC, 0, NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3);

  struct tm t;
  if (!getLocalTime(&t, NTP_SYNC_TIMEOUT_MS)) {
    logError("NTP sync failed (no response).");
    return false;
  }
  if ((t.tm_year + 1900) < RTC_MIN_VALID_YEAR) {
    logError("NTP returned implausible year " + String(t.tm_year + 1900));
    return false;
  }

  DateTime ntpTime(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                   t.tm_hour, t.tm_min, t.tm_sec);

  if (rtcOk) {
    RtcBus bus;
    DateTime before = rtc.now();
    long drift = before.isValid()
                 ? (long)ntpTime.unixtime() - (long)before.unixtime()
                 : 0;
    rtc.adjust(ntpTime);
    rtcTimeValid   = true;
    clockRefEpoch  = ntpTime.unixtime();
    clockRefMillis = millis();
    logOk("RTC set from NTP (was off by %+ld s)", drift);
  } else {
    logWarn("NTP time acquired, but there is no RTC to store it in.");
  }

  ntpEverSynced = true;
  lastNtpSyncMs = millis();
  logInfo("Time is now %s EAT", getTimestamp().c_str());
  return true;
}

// ---------- Sync on first opportunity, then once a day ----------
void maybeResyncRTC() {
  if (WiFi.status() != WL_CONNECTED) return;

  // Synced recently enough - the DS3231 drifts about a minute a year.
  if (ntpEverSynced && (millis() - lastNtpSyncMs) < NTP_RESYNC_INTERVAL_MS) return;

  // Never synced: keep trying, but not every cycle. A WiFi link that associates
  // without a route to the internet would otherwise burn NTP_SYNC_TIMEOUT_MS and
  // an error-log line on every single wake.
  if (!ntpEverSynced && lastNtpAttemptMs != 0 &&
      (millis() - lastNtpAttemptMs) < NTP_RETRY_INTERVAL_MS) return;

  syncTimeFromNTP();
}

// ---------- Manual clock set (config portal) ----------
// Accepts "YYYY-MM-DD HH:MM:SS", "YYYY-MM-DD HH:MM", or the browser's
// datetime-local form "YYYY-MM-DDTHH:MM". Interpreted as EAT.
bool setRtcFromString(const String &input) {
  String s = input;
  s.trim();
  s.replace("T", " ");
  if (s.length() < 16) return false;

  int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
  int n = sscanf(s.c_str(), "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &se);
  if (n < 5) return false;
  if (n == 5) se = 0;

  if (y < RTC_MIN_VALID_YEAR || y > 2099 || mo < 1 || mo > 12 || d < 1 || d > 31 ||
      h < 0 || h > 23 || mi < 0 || mi > 59 || se < 0 || se > 59) return false;

  DateTime dt(y, mo, d, h, mi, se);
  if (!dt.isValid()) return false;

  // The config portal runs with the sensor rail down and the DS3231 is on that
  // rail, so the write would go nowhere. Bring the rail up just long enough to
  // land it, then put it back as it was.
  bool railWasDown = !relayOn;
  if (railWasDown) {
    setSensorPower(true);
    delay(SENSOR_RAIL_SETTLE_MS);
    RtcBus bus;
    rtcOk = rtc.begin();
  }

  if (!rtcOk) {
    if (railWasDown) setSensorPower(false);
    logError("Manual clock set requested but the RTC did not answer.");
    return false;
  }

  {
    RtcBus bus;
    rtc.adjust(dt);
  }
  rtcTimeValid   = true;
  clockRefEpoch  = dt.unixtime();
  clockRefMillis = millis();

  if (railWasDown) setSensorPower(false);
  logOk("RTC set manually to %s EAT", getTimestamp().c_str());
  return true;
}

// ================== RECORD STORAGE & UPLOAD QUEUE ==================
// Two files, two jobs:
//   datalog.txt  permanent archive, appended once per reading, never rewritten
//   queue.txt    upload buffer; a record leaves it only once delivered
// A record is written to both during the burst, so a failed upload can never
// lose data - the worst case is a delayed one.

// Queue lines carry two extra columns the archive does not need: the delivery
// mask (which endpoints already accepted this record) and a retry count.
// A missing reading is written as an empty field, not as a number. Writing
// String(NAN, 2) would emit "nan", and reading it back with toFloat() yields
// 0.0 - silently converting "no reading" into a real-looking zero on the first
// queue round-trip.
static String fmtField(float v, int dp) {
  if (isnan(v)) return String();
  return String(v, dp);
}

// The per-minute slices are pipe-separated so the whole list rides inside a
// single CSV field. parseLineToRecord() validates a line by counting commas, and
// a variable-length list written with commas would make every record look
// malformed - or worse, line up by accident and shift every column.
static String packCounts(const uint16_t *v, uint8_t n) {
  String s;
  s.reserve((size_t)n * 6);        // up to five digits plus a separator
  for (uint8_t i = 0; i < n; i++) {
    if (i) s += '|';
    s += (unsigned)v[i];
  }
  return s;
}

// Returns how many entries were recovered. An empty field yields 0, which is
// what a record written before any slice closed looks like.
static uint8_t unpackCounts(const String &f, uint16_t *v, uint8_t maxN) {
  uint8_t n = 0;
  int start = 0;
  while (start < (int)f.length() && n < maxN) {
    int bar = f.indexOf('|', start);
    if (bar < 0) bar = f.length();
    if (bar == start) break;                       // stray separator
    v[n++] = (uint16_t)f.substring(start, bar).toInt();
    start = bar + 1;
  }
  return n;
}

static float parseField(const String &f) {
  if (f.length() == 0) return NAN;
  // -999 was the old sentinel; treat it as missing so pre-existing queue lines
  // do not come back as a temperature of minus nine hundred.
  float v = f.toFloat();
  if (v <= -998.0f && v >= -1000.0f) return NAN;
  return v;
}

// Queue lines carry two extra columns the archive does not need: the delivery
// mask (which endpoints already accepted this record) and a retry count.
String recordToLine(const DataRecord &rec, bool includeQueueFields) {
  // Queue lines lead with a format tag so a stale one is rejected rather than
  // parsed with today's column positions. The archive has a CSV header instead
  // and is never read back, so it does not carry the tag.
  String line = includeQueueFields ? String(QUEUE_FORMAT_TAG) + "," : String();
  // Two lists of up to MAX_BUCKETS entries make this line long and its growth
  // append-by-append. Reserve once rather than let the heap fragment.
  line.reserve(200 + (size_t)rec.bucketCount * 12);
  line += rec.timestamp + "," +
                fmtField(rec.windMPH, 2) + "," +
                fmtField(rec.windMS, 2) + "," +
                fmtField(rec.windGustMS, 2) + "," +
                fmtField(rec.rainInterval_mm, 2) + "," +
                fmtField(rec.temperature, 2) + "," +
                fmtField(rec.humidity, 1) + "," +
                fmtField(rec.pressure, 2) + "," +
                fmtField(rec.batteryTemp, 2) + "," +
                fmtField(rec.solarVoltage, 3) + "," +
                fmtField(rec.solarCurrent_mA, 2) + "," +
                fmtField(rec.batteryVoltage, 3) + "," +
                fmtField(rec.batteryCurrent_mA, 2) + "," +
                fmtField(rec.adc0, 4) + "," +
                fmtField(rec.adc1, 4) + "," +
                fmtField(rec.adc2, 4) + "," +
                fmtField(rec.adc3, 4) + "," +
                String(rec.intervalSec) + "," +
                String(rec.bucketSec) + "," +
                packCounts(rec.bucketWind, rec.bucketCount) + "," +
                packCounts(rec.bucketRain, rec.bucketCount) + "," +
                String(rec.gustCount) + "," +
                String(rec.gustSpanMs);
  if (includeQueueFields) {
    line += "," + String(rec.sentMask) + "," + String(rec.retries);
  }
  return line;
}

bool parseLineToRecord(const String &line, DataRecord &rec) {
  // Count separators before indexing anything - walking a corrupt line into a
  // fixed array is how this function used to write past the end.
  int commas = 0;
  for (unsigned int i = 0; i < line.length(); i++) {
    if (line.charAt(i) == ',') commas++;
  }
  if (commas + 1 != REC_FIELDS_QUEUE) return false;

  String fields[REC_FIELDS_QUEUE];
  int start = 0, n = 0;
  for (int i = 0; i < commas; i++) {
    int comma = line.indexOf(',', start);
    fields[n++] = line.substring(start, comma);
    start = comma + 1;
  }
  fields[n++] = line.substring(start);

  // Written by a different firmware layout - the columns would not line up.
  if (fields[0] != QUEUE_FORMAT_TAG) return false;

  rec.timestamp         = fields[1];
  rec.windMPH           = parseField(fields[2]);
  rec.windMS            = parseField(fields[3]);
  rec.windGustMS        = parseField(fields[4]);
  rec.rainInterval_mm   = parseField(fields[5]);
  rec.temperature       = parseField(fields[6]);
  rec.humidity          = parseField(fields[7]);
  rec.pressure          = parseField(fields[8]);
  rec.batteryTemp       = parseField(fields[9]);
  rec.solarVoltage      = parseField(fields[10]);
  rec.solarCurrent_mA   = parseField(fields[11]);
  rec.batteryVoltage    = parseField(fields[12]);
  rec.batteryCurrent_mA = parseField(fields[13]);
  rec.adc0              = parseField(fields[14]);
  rec.adc1              = parseField(fields[15]);
  rec.adc2              = parseField(fields[16]);
  rec.adc3              = parseField(fields[17]);
  rec.intervalSec       = (uint32_t)fields[18].toInt();
  rec.bucketSec         = (uint16_t)fields[19].toInt();

  // rec is a stack object here, so the tail past the recovered entries has to be
  // cleared rather than inherited from whatever occupied that memory before.
  memset(rec.bucketWind, 0, sizeof(rec.bucketWind));
  memset(rec.bucketRain, 0, sizeof(rec.bucketRain));
  uint8_t nWind = unpackCounts(fields[20], rec.bucketWind, MAX_BUCKETS);
  uint8_t nRain = unpackCounts(fields[21], rec.bucketRain, MAX_BUCKETS);
  // Written in lockstep, so they can only disagree if the line was damaged.
  // Take the shorter: the slices would no longer pair up beyond that point.
  rec.bucketCount       = (nWind < nRain) ? nWind : nRain;

  rec.gustCount         = (uint32_t)fields[22].toInt();
  rec.gustSpanMs        = (uint32_t)fields[23].toInt();
  rec.sentMask          = (uint8_t)fields[24].toInt();
  rec.retries           = (uint8_t)fields[25].toInt();
  return true;
}

void archiveRecord(const DataRecord &rec) {
  if (!appendSD(recordToLine(rec, false), SD_DATA_FILE)) {
    logError("Failed to append to the archive.");
  }
}

void enqueueRecord(const DataRecord &rec) {
  if (!appendSD(recordToLine(rec, true), SD_QUEUE_FILE)) {
    logError("Failed to append to the upload queue.");
    return;
  }
  queueDepthCached++;
}

// ================== TRANSMISSION ==================

// Four outcomes, not three. The split that matters is the middle two: a record
// the server answered about has had an attempt spent on it, a record that never
// reached a server has not. Collapsing them is what used to discard good
// readings - one every MAX_RETRIES cycles, for as long as an outage lasted.
enum SendOutcome {
  SEND_OK,          // every endpoint accepted it
  SEND_TRANSIENT,   // the server answered, with a fault of its own (5xx)
  SEND_NO_LINK,     // never reached a server at all - no connection, or no reply
  SEND_PERMANENT    // the server refused it; retrying will not help
};

// ---------- One encrypted connection per upload round ----------
// All three endpoints live on the same host, so there is no reason to negotiate
// a separate connection for each. The firmware used to do exactly that: three
// per record, up to thirty per round, and some of the memory behind each was
// never returned. That is the leak - about 1.1 kB a cycle - and it stopped the
// station once the largest free block could no longer fit a handshake.
//
// The client below is opened on the first POST of a round and torn down at the
// end of it; HTTPClient's keep-alive carries it across all three endpoints and
// every record in between. Holding it through the sleep window would pin tens
// of kB for nothing, so it is closed rather than kept.
static WiFiClientSecure *uploadClient = nullptr;

static bool openUploadClient() {
  if (uploadClient) return true;

  uploadClient = new WiFiClientSecure();
  if (!uploadClient) {
    logError("Out of memory opening the upload connection.");
    return false;
  }
  // No certificate check. This is unchanged from the deprecated
  // HTTPClient::begin(url) it replaces, which passed a NULL CA and did the same
  // thing silently - the station carries no cert store and has no trustworthy
  // clock at first boot. Spelled out here because the old call hid it.
  uploadClient->setInsecure();
  uploadClient->setHandshakeTimeout(TLS_HANDSHAKE_TIMEOUT_S);
  return true;
}

static void closeUploadClient() {
  if (!uploadClient) return;
  uploadClient->stop();
  delete uploadClient;
  uploadClient = nullptr;
}

static int httpPost(const String &url, const String &payload) {
  if (!openUploadClient()) return -1;

  HTTPClient http;
  http.setReuse(true);          // load-bearing: this is what keeps the session open
  if (!http.begin(*uploadClient, url)) return -1;
  http.addHeader("Content-Type", "application/json");
  http.setConnectTimeout(HTTP_CONNECT_TIMEOUT_MS);
  http.setTimeout(HTTP_RESPONSE_TIMEOUT_MS);
  int code = http.POST(payload);
  http.end();                   // hands the socket back for reuse, does not close it
  return code;
}

// Only add a key when the reading is real. An omitted key leaves the Django
// column NULL, which is the honest representation of a failed sensor - a -999
// or a 0.0 would be indistinguishable from a measurement.
static void putIfReal(JsonDocument &doc, const char *key, float v) {
  if (!isnan(v)) doc[key] = v;
}

static String buildWeatherJson(const DataRecord &rec) {
  JsonDocument doc;
  doc["station_code"] = stationCode;
  doc["timestamp"]    = rec.timestamp;
  doc["interval_s"]   = rec.intervalSec;

  putIfReal(doc, "temperature", rec.temperature);   // degC, DHT22
  putIfReal(doc, "humidity",    rec.humidity);      // %RH, DHT22
  putIfReal(doc, "pressure",    rec.pressure);      // hPa, BME280 station pressure
  putIfReal(doc, "rain",        rec.rainInterval_mm);  // mm this interval
  putIfReal(doc, "wind_speed",  rec.windMS);        // m/s, interval mean
  putIfReal(doc, "wind_gust",   rec.windGustMS);    // m/s, highest 3 s average

  // Raw counts, sliced into bucket_s windows, oldest first. Same reasoning as
  // the pin voltages below, one step further: these are integers, so unlike a
  // voltage they carry no measurement error at all. rain and wind_speed above
  // are just these totals times a constant.
  //
  // Sliced rather than summed because a total is a one-way door: minutes can be
  // added up server-side, but an interval total can never be taken apart again.
  doc["bucket_s"] = rec.bucketSec;
  JsonArray wp = doc["wind_pulses"].to<JsonArray>();
  JsonArray rt = doc["rain_tips"].to<JsonArray>();
  for (uint8_t i = 0; i < rec.bucketCount; i++) {
    wp.add(rec.bucketWind[i]);
    rt.add(rec.bucketRain[i]);
  }

  // The gust window. gust_count 0 means no 3 s window ever closed, so wind_gust
  // is the interval mean standing in for a gust rather than a measured one.
  doc["gust_count"]   = rec.gustCount;
  doc["gust_span_ms"] = rec.gustSpanMs;

  // Raw ADS1115 pin voltages, plus the values derived from them. Both are sent:
  // the raw volts stay ground truth, so a wrong constant in calibration.h is
  // repairable with one SQL UPDATE instead of being lost data.
  putIfReal(doc, "wind_direction_v",  rec.adc0);
  putIfReal(doc, "soil_moisture_v",   rec.adc1);
  putIfReal(doc, "solar_radiation_v", rec.adc2);

  int dir = vaneDirectionInt(rec.adc0);
  if (dir >= 0) doc["wind_direction"] = dir;             // integer 0-359
  putIfReal(doc, "soil_moisture",   soilMoisturePct(rec.adc1));
  putIfReal(doc, "solar_radiation", irradianceWm2(rec.adc2));

  String out;
  serializeJson(doc, out);
  return out;
}

static String buildVoltageJson(const DataRecord &rec) {
  JsonDocument doc;
  doc["station_code"] = stationCode;
  doc["timestamp"]    = rec.timestamp;

  putIfReal(doc, "volt_batt",    rec.batteryVoltage);
  putIfReal(doc, "volt_solar",   rec.solarVoltage);
  putIfReal(doc, "battery_temp", rec.batteryTemp);   // degC, DS18B20

  String out;
  serializeJson(doc, out);
  return out;
}

static String buildCurrentJson(const DataRecord &rec) {
  JsonDocument doc;
  doc["station_code"] = stationCode;
  doc["timestamp"]    = rec.timestamp;

  // The API stores amps; the INA219 reports milliamps.
  if (!isnan(rec.batteryCurrent_mA)) doc["curr_batt"]  = rec.batteryCurrent_mA / 1000.0;
  if (!isnan(rec.solarCurrent_mA))   doc["curr_solar"] = rec.solarCurrent_mA / 1000.0;

  String out;
  serializeJson(doc, out);
  return out;
}

// Post only the endpoints this record still owes. Without the mask a partial
// success would be re-sent in full next cycle and duplicate rows server-side.
static SendOutcome sendRecord(DataRecord &rec) {
  const char *urls[3]  = { WEATHER_URL, VOLTAGE_URL, CURRENT_URL };
  const char *names[3] = { "weather", "voltage", "current" };
  const uint8_t bits[3] = { SENT_WEATHER, SENT_VOLTAGE, SENT_CURRENT };

  bool anyTransient = false;
  bool anyPermanent = false;
  bool anyNoLink    = false;

  for (int i = 0; i < 3; i++) {
    if (rec.sentMask & bits[i]) continue;         // already delivered

    String payload;
    if (i == 0)      payload = buildWeatherJson(rec);
    else if (i == 1) payload = buildVoltageJson(rec);
    else             payload = buildCurrentJson(rec);

    // Print exactly what goes on the wire - this is the ground truth when a
    // field is not landing in the database.
    logSend("POST %s", names[i]);
    Serial.printf(C_DIM "       %s" C_RESET "\n", payload.c_str());

    int code = httpPost(urls[i], payload);

    if (code >= 200 && code < 300) {
      rec.sentMask |= bits[i];
      logOk("%s accepted (HTTP %d)", names[i], code);
    } else if (code >= 400 && code < 500) {
      anyPermanent = true;
      logError(String(names[i]) + " POST refused (HTTP " + String(code) + ")");
    } else if (code > 0) {
      // 5xx: the server answered, so the record reached it and this attempt is
      // the record's to carry. Nothing to gain from the remaining endpoints now.
      anyTransient = true;
      logError(String(names[i]) + " POST failed (HTTP " + String(code) + ")");
      break;
    } else {
      // Negative, so HTTPClient never got an answer: no connection at all (-1),
      // or the server too slow to reply (-11). Nothing was learned about the
      // record, so this must not be charged to it.
      anyNoLink = true;
      logError(String(names[i]) + " POST could not reach the server (error " +
               String(code) + ")");
      break;
    }
  }

  if (rec.sentMask == SENT_ALL) return SEND_OK;
  // A dead link outranks the rest, and both outrank a refusal: those are the
  // outcomes that leave the record's own validity untested, and the safe
  // reading of "untested" is "send it again". Same reason the fall-through at
  // the bottom is SEND_NO_LINK - the cheapest wrong answer is a retry.
  if (anyNoLink)                return SEND_NO_LINK;
  if (anyTransient)             return SEND_TRANSIENT;
  if (anyPermanent)             return SEND_PERMANENT;
  return SEND_NO_LINK;
}

// ================== QUEUE DRAIN ==================
// Streams through a temp file rather than loading the queue into RAM: a week
// offline is ~2000 records, which as a vector of Strings will exhaust the heap.
// The live file is only replaced once the replacement is closed, so a power cut
// mid-drain costs nothing.
bool processPendingQueue() {
  if (!SD.exists(SD_QUEUE_FILE)) return true;

  File in = SD.open(SD_QUEUE_FILE, FILE_READ);
  if (!in) {
    logError("Cannot open queue for reading.");
    return false;
  }
  if (in.size() == 0) {
    in.close();
    return true;
  }

  if (SD.exists(SD_QUEUE_TMP)) SD.remove(SD_QUEUE_TMP);
  File out = SD.open(SD_QUEUE_TMP, FILE_WRITE);
  if (!out) {
    in.close();
    logError("Cannot open queue temp file - leaving queue untouched.");
    return false;
  }

  int sent = 0, refused = 0, kept = 0;
  int budget = MAX_PENDING_CYCLE;
  bool stopSending = false;

  while (in.available()) {
    // A full drain is up to MAX_PENDING_CYCLE records across three endpoints, so
    // this loop can outlast a slice on its own.
    maybeCloseBuckets();

    String line = in.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    // Once the link has failed, copy the rest across verbatim and try again
    // next cycle. Same when this cycle's send budget is spent.
    if (stopSending || budget <= 0) {
      out.println(line);
      kept++;
      continue;
    }

    DataRecord rec;
    if (!parseLineToRecord(line, rec)) {
      logError("Malformed queue line moved to rejected.txt");
      appendSD(line, SD_REJECTED_FILE);
      refused++;
      continue;
    }

    budget--;
    SendOutcome outcome = sendRecord(rec);

    if (outcome == SEND_OK) {
      sent++;
      continue;                                   // drops off the queue
    }

    // Only a failure the server actually answered counts against this record.
    // An outage says nothing about the reading, and charging it to the record
    // is what used to throw good data away while the network was down.
    if (outcome != SEND_NO_LINK) rec.retries++;

    if (outcome == SEND_PERMANENT || rec.retries >= MAX_RETRIES) {
      // Park it so one bad record cannot block everything behind it forever.
      appendSD(recordToLine(rec, true), SD_REJECTED_FILE);
      logError("Record " + rec.timestamp + " moved to rejected.txt after " +
               String(rec.retries) + " attempt(s).");
      refused++;
      continue;
    }

    out.println(recordToLine(rec, true));
    kept++;
    stopSending = true;
  }

  in.close();
  out.close();

  // The round is over: hand the connection's working memory back rather than
  // holding it through the sleep window.
  closeUploadClient();

  // Swap only now that the replacement is complete on disk.
  if (!SD.remove(SD_QUEUE_FILE)) {
    logError("Could not remove the old queue file.");
    SD.remove(SD_QUEUE_TMP);
    return false;
  }
  if (!SD.rename(SD_QUEUE_TMP, SD_QUEUE_FILE)) {
    logError("Could not rename the queue temp file - queue is in queue.tmp.");
    return false;
  }

  queueDepthCached = kept;
  logInfo("Queue: %d sent, %d rejected, %d still waiting.",
                sent, refused, kept);
  return (kept == 0);
}
