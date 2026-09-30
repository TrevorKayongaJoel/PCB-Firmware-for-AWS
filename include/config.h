#ifndef CONFIG_H
#define CONFIG_H

// ========== Libraries ==========
#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_ADS1X15.h>
#include <Adafruit_INA219.h>
#include <RTClib.h>
#include <DHT.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <vector>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Adafruit_BME280.h>
#include <time.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <esp_sleep.h>
#include <driver/gpio.h>
#include <esp_system.h>   // esp_reset_reason()

// ========== WiFi Credentials (FALLBACK - used only if no saved credentials) ==========
#define WIFI_SSID_FALLBACK     "AWS_MIFI"
#define WIFI_PASSWORD_FALLBACK "A220855I"
#define PORTAL_TIMEOUT         200     // seconds the config portal stays open
#define PORTAL_AP_SSID         "AWS-Conf"   // same SSID for our portal and WiFiManager

// ========== Pin Definitions ==========
#define RELAY_PIN       13
#define RELAY_ACTIVE    HIGH
#define I2C_SDA         21
#define I2C_SCL         22

// The gateway PCB lands the DS3231 module's SDA pad on GPIO22 and its SCL pad
// on GPIO21 - mirrored from the ADS1115 and both INA219s, which sit the normal
// way round on the same two wires. The boards are already made, so the swap is
// undone in software; see RtcBus in main.cpp.
#define RTC_SDA         22
#define RTC_SCL         21

// Boot-time I2C scan on both pin mappings. Diagnostic only - set to 0 once the
// DS3231 is answering.
#define I2C_SCAN_ON_BOOT       1
#define WIND_PIN        27
#define RAIN_PIN        26
#define DHTPIN          25
#define DS18B20_PIN     32

#define SD_CS           5
#define SD_MOSI         23
#define SD_MISO         19
#define SD_SCK          18

// ========== Sensor Constants ==========
#define DHTTYPE         DHT22

// The AM2302 needs ~1 s after power-up before its first reading is valid, and
// returns a bad checksum now and then even when healthy. It will not produce a
// new sample faster than every 2 s, so retries have to be spaced accordingly.
#define DHT_READ_ATTEMPTS   3
#define DHT_RETRY_DELAY_MS  2200

// BME280 - not fitted yet. Detected on the bus at power-up, so plugging it in
// starts producing pressure with no reflash. Sends STATION pressure; mean
// sea-level pressure is derived server-side from the station altitude.
#define BME280_ADDR_A   0x76
#define BME280_ADDR_B   0x77
#define MM_PER_TIP      0.2
#define DEBOUNCE_US     100000      // 100 ms

// ========== Pulse Counting ==========
#define WIND_DEBOUNCE_US   5000UL      // 5 ms; at 30 Hz max the period is 33 ms
#define GUST_WINDOW_US     3000000UL   // 3 s - the standard gust averaging window
#define WIND_MPH_PER_HZ    1.5f
#define WIND_MS_PER_HZ     0.67f
#define REED_RELEASE_MS    50          // bounded wait for a reed contact to reopen

// ========== Light Sleep ==========
#define SLEEP_CHUNK_SEC    60          // wake at least this often to re-check the RTC

// ========== Time / RTC ==========
// The DS3231 holds LOCAL time: East Africa Time (UTC+3, no DST).
// SD logs and API payloads therefore all carry EAT.
#define TZ_OFFSET_SEC          (3 * 3600)
#define NTP_SERVER_1           "pool.ntp.org"
#define NTP_SERVER_2           "time.google.com"
#define NTP_SERVER_3           "time.nist.gov"
#define NTP_SYNC_TIMEOUT_MS    20000UL   // 10 s was tight when DNS is slow
#define NTP_RESYNC_INTERVAL_MS (24UL * 60UL * 60UL * 1000UL)   // re-sync daily
#define NTP_RETRY_INTERVAL_MS  (30UL * 60UL * 1000UL)          // floor between failed attempts
#define RTC_MIN_VALID_YEAR     2025   // older than this means the clock was never set
#define TIMESTAMP_INVALID      "1970-01-01 00:00:00"

// ========== Config Limits ==========
#define SLEEP_MIN_SEC          60
#define SLEEP_MAX_SEC          3600
#define STATION_CODE_MAXLEN    20

// ========== Field Debug Mode ==========
// A servicing mode entered onsite. Three things differ from the normal portal:
// the sensor rail stays UP so readings can be taken live, the config window
// never times out, and readings taken here are never archived, queued or
// uploaded - they exist only to be looked at.
//
// The ceiling is not about battery; one session is a few tens of mAh. It is
// that a station left in debug writes no records and uploads nothing, so the
// price of walking away is a hole in datalog.txt.
#define DEBUG_MAX_SEC      900     // 15 min dead-man switch on the whole mode

// BOOT (GPIO0) is the only button on the devkit that software can read - EN is
// wired straight to the chip's enable pin and never reaches the firmware. Held
// after power-up it is the way back into a station whose portal never opens.
//
// The gesture is "power on FIRST, then press and hold". GPIO0 held low at the
// instant of reset puts the ESP32 into its serial bootloader, where it does
// nothing at all and looks dead.
#define BOOT_PIN            0
#define BOOT_HOLD_MS        2000   // held this long to count as deliberate
#define BOOT_WATCH_MS       3000   // how long after boot the hold is watched for

// The portal runs in one of two modes. Passing the mode rather than branching
// on a global keeps the WiFiManager handoff, the DNS server and every route
// shared between them.
enum PortalMode { PORTAL_NORMAL, PORTAL_DEBUG };

// ========== Per-minute buckets ==========
// Wind pulses and rain tips are the only quantities accumulated continuously -
// everything else is a point sample taken once per burst while the rail is up.
// So they are also the only ones with structure inside the interval, and a
// 30 min total throws that structure away for good: totals can be summed out of
// minutes, minutes can never be recovered from a total.
//
// Splitting them costs a counter read on a wake we are already taking. No rail,
// no radio, no SD - which is why the resolution can be finer than the
// transmission interval instead of being tied to it.
#define BUCKET_SEC     60
// +2 slots of headroom: a bucket can only ever close late (the boundary test is
// >=), never early, so the count cannot exceed the ideal - but do not let wake
// jitter be the thing that proves that wrong.
#define MAX_BUCKETS    ((SLEEP_MAX_SEC / BUCKET_SEC) + 2)

// ========== Server Endpoints ==========
#define BASE_URL        "https://api.wimea-ict.net/api/ingest/"
#define WEATHER_URL     BASE_URL "weather/"
#define VOLTAGE_URL     BASE_URL "voltage/"
#define CURRENT_URL     BASE_URL "current/"

// ========== Transmission Timeouts ==========
// Measured against this server: up to 4.2 s to open a connection and 8.6 s
// to answer a request that does no database work. The handshake limit had
// never been set at all and was defaulting to 5 s, which made a slow but
// perfectly healthy server indistinguishable from an outage.
//
// Mind the units: setHandshakeTimeout() on WiFiClientSecure takes SECONDS,
// both HTTPClient limits take MILLISECONDS. Getting that wrong is silent -
// it just sets a limit 1000x out in one direction or the other.
#define TLS_HANDSHAKE_TIMEOUT_S    15
#define HTTP_CONNECT_TIMEOUT_MS    15000
#define HTTP_RESPONSE_TIMEOUT_MS   20000

// ========== SD Files ==========
// datalog.txt is the permanent archive - append only, never rewritten.
// queue.txt is the upload buffer - records leave it only once delivered.
// rejected.txt holds what the server permanently refused, so one bad record
// can never block the queue behind it.
#define SD_DATA_FILE     "/datalog.txt"
#define SD_QUEUE_FILE    "/queue.txt"
#define SD_QUEUE_TMP     "/queue.tmp"
#define SD_REJECTED_FILE "/rejected.txt"
#define SD_ERROR_FILE    "/error.log"
// diag.log is the flight recorder: one line per boot saying why the station
// restarted, and one line each time free heap sets a new low. A continuous
// run of new lows with no recovery is a leak; a flat trend with the odd dip
// is normal churn. Written only on a new low, so the file stays short over
// months of healthy running and grows a line per cycle exactly when
// something is wrong.
#define SD_DIAG_FILE     "/diag.log"

// WindPulses and RainTips are pipe-separated lists, one entry per Bucket_s,
// oldest first. A pipe keeps them inside a single CSV field so the fixed field
// count that parseLineToRecord() depends on survives a variable-length list.
#define SD_CSV_HEADER "Timestamp,WindMPH,WindMS,WindGust_MS,Rain_mm,Temp_C,Humidity%,Pressure_hPa,BatteryTemp_C,Solar_V,Solar_mA,Batt_V,Batt_mA,ADC0,ADC1,ADC2,ADC3,Interval_s,Bucket_s,WindPulses,RainTips,GustCount,GustSpanMs"

// Field counts for the CSV record format. Queue lines carry a format tag in
// front and two extra columns behind (delivery mask + retry count) that the
// archive does not need.
//
// The tag exists because field counts alone are not enough: when the record
// format gained columns, old queue lines still fell inside the accepted count
// range and were parsed with the new indices, shifting every field. Bump the
// tag whenever the layout changes and stale lines are rejected outright.
#define QUEUE_FORMAT_TAG  "QV4"
#define REC_FIELDS_BASE   23
#define REC_FIELDS_QUEUE  26

// Bits in DataRecord::sentMask - one per ingest endpoint.
#define SENT_WEATHER  0x01
#define SENT_VOLTAGE  0x02
#define SENT_CURRENT  0x04
#define SENT_ALL      0x07

// How many error lines to hold in RAM when the SD rail is powered down.
#define ERROR_BUFFER_SLOTS 8

// The SD card shares the sensor rail on this build, so the rail has to stay up
// while the upload queue is being drained. Set to 1 if SD has its own supply -
// the sensor rail then drops as soon as the readings are taken, as the pipeline
// describes.
#define SD_INDEPENDENT_POWER 0

// Time for the sensor rail, the SD card and the I2C devices to come up after
// the relay closes.
#define SENSOR_RAIL_SETTLE_MS  500

// ========== Queue Settings ==========
#define MAX_RETRIES         5
#define MAX_PENDING_CYCLE   10

// ========== Heap Watchdog ==========
// TLS needs its working memory in ONE contiguous block, so it is the largest
// free block - not the total free heap - that decides whether an upload can
// start at all. The station failed at 38,900 bytes on 2026-09-27 with over
// 100 kB still free in aggregate.
//
// 50 kB sits comfortably above that failure point while leaving room for a
// normal round to run. This is a safety net, not the fix, and it is
// deliberately independent of the connection reuse in httpPost(): if some
// other leak remains, it costs one missed reading instead of a site visit.
#define HEAP_MIN_BLOCK_BYTES   51200

// ========== Global Objects ==========
extern Adafruit_ADS1115      ads;
extern Adafruit_INA219      batterySensor;
extern Adafruit_INA219      solarSensor;
extern RTC_DS3231           rtc;
extern DHT                  dht;
extern Preferences          preferences;
extern OneWire              oneWire;
extern DallasTemperature    ds18b20;
extern Adafruit_BME280      bme;

extern int sleepIntervalSec;
extern String stationCode;
extern bool relayOn;
extern bool ds18b20Ok;
extern bool bmeOk;          // BME280 answered on the I2C bus this power-up
extern bool rtcOk;          // DS3231 responded on the I2C bus
extern bool rtcTimeValid;   // ...and the time it holds is trustworthy
extern bool haveLastRecord;     // a reading has been taken since boot
extern uint32_t nextCycleEpoch; // RTC epoch of the next scheduled burst
extern int queueDepthCached;         // records waiting, tracked without touching SD
extern unsigned long nextCycleMillis; // millis() deadline for the next burst

// ========== Data Record ==========
struct DataRecord {
  String timestamp;
  float windMPH;
  float windMS;
  float windGustMS;         // highest 3 s average within the interval
  float rainInterval_mm;     // mm in THIS interval only - not cumulative
  float temperature;        // Air temp (DHT22)
  float humidity;           // Air humidity (DHT22)
  float pressure;           // Station pressure (BME280), hPa - NAN until fitted
  float batteryTemp;        // Battery temperature (DS18B20)
  float solarVoltage;
  float solarCurrent_mA;
  float batteryVoltage;
  float batteryCurrent_mA;
  float adc0, adc1, adc2, adc3;   // raw ADS1115 volts: vane, soil, solar rad, unused
  uint32_t intervalSec;           // seconds that rain / windMS average over

  // Raw counts, split into BUCKET_SEC slices, oldest first. These are the
  // ground truth: windMS and rainInterval_mm are just these times a constant,
  // so a recalibration is a server-side change instead of lost data. The slices
  // always sum to the interval total - closeBucket() guarantees it.
  uint16_t bucketWind[MAX_BUCKETS];   // wind pulses in each slice
  uint16_t bucketRain[MAX_BUCKETS];   // rain tips in each slice
  uint8_t  bucketCount;               // slices actually populated
  // Carried per record rather than read from BUCKET_SEC at send time, so a
  // record queued before a change to that constant is still described correctly
  // when it finally goes out.
  uint16_t bucketSec;

  // The winning 3 s window behind windGustMS. Not recoverable from the slices
  // above - a gust lives inside a minute, not across it - so it is carried
  // separately or it is gone.
  uint32_t gustCount;        // pulses in that window; 0 means no gust was measured
  uint32_t gustSpanMs;       // width of that window

  uint8_t sentMask;         // which ingest endpoints already accepted this record
  uint8_t retries;
};


// Most recent reading, kept in RAM so the config portal can display it.
extern DataRecord lastRecord;

// ========== Function Prototypes ==========
void loadConfig();
void saveConfig(int newInterval, const String &newStation);
void readSensors(DataRecord &rec);
bool appendSD(const String &line, const char *filename);   // false if the write failed
void logError(const String &message);
String getTimestamp();
void runConfigPortal(PortalMode mode = PORTAL_NORMAL);
void printSensorValues(const DataRecord &rec);   // <-- ADDED
bool connectWiFi();                              // <-- ADDED

// ---- Config portal ----
int  queuedRecordCount();          // lines currently waiting in the upload queue
void setSensorPower(bool on);      // relay that gates the sensor rail
void sensorRailUp();               // power up AND re-initialise everything on it
void sensorRailDown();             // unmount SD cleanly, then cut power
bool mountSD();                    // (re)run the card init sequence
void unmountSD();                  // flush and release before power is removed

// ---- Pulse counting & light sleep ----
uint32_t currentEpoch();               // RTC epoch, or 0 when the clock is untrustworthy
void snapshotPulseCounters();          // close the interval: reset counters, compute results
void maybeCloseBuckets();              // close any BUCKET_SEC slice whose time is up
void sleepUntilNextEventOrDeadline();  // light sleep until a pulse or the next burst

// ---- Time / RTC ----
bool syncTimeFromNTP();                      // fetch NTP time and store it in the DS3231
void maybeResyncRTC();                       // sync on first chance, then once a day
bool refreshRtcValidity();                   // re-check whether the RTC time is trustworthy
bool setRtcFromString(const String &input);  // manual set, "YYYY-MM-DD HH:MM:SS" (EAT)

// ---- Queue functions ----
void enqueueRecord(const DataRecord &rec);
bool processPendingQueue();
bool parseLineToRecord(const String &line, DataRecord &rec);
String recordToLine(const DataRecord &rec, bool includeQueueFields = false);
void archiveRecord(const DataRecord &rec);   // append to the permanent datalog
void flushBufferedErrors();                  // write RAM-held errors once SD is back

// ---- Diagnostics ----
void logBootDiagnostics();   // restart reason + heap, one line per boot
void noteHeapLow();          // one line each time free heap sets a new low
void heapWatchdog();         // restart if the largest free block is too small

#endif
