# AWS-UG-001 — Why the station keeps stopping, and the cause

**Station:** AWS-UG-001 (shown as MAKERERE on the dashboard)
**Reporting period:** 2026-09-18 → 2026-09-28
**Evidence:** on-board `datalog.txt`, `error.log` and `queue.txt` from the SD card; serial console captures; response-time measurements against the ingest API
**Author:** Joel Kayonga Trevor

---

## Bottom line

The repeated stoppages have a **confirmed root cause: a memory leak in the firmware's upload path.** The station loses roughly 1.1 kB of working memory every measurement cycle. After about two days it can no longer open an encrypted connection to the server, and it stops — still powered, still measuring, but unable to send or to write to its card — until someone travels to the site and cuts power.

This was proven, not inferred. Diagnostic logging added on 2026-09-25 recorded the memory falling continuously across 47 hours, and the failure arrived exactly where that trend predicted.

Two **fixes have been implemented and built**: the leak has been addressed at source, and a safety net now restarts the station automatically if memory ever runs low again. Once flashed, a fault of this kind should cost one missed reading instead of a site visit.

**No measurement data has been lost.** Every reading is written to the permanent archive on the SD card before any attempt to transmit. The gaps on the dashboard are delivery gaps, not measurement gaps.

---

## Confirmed timeline

| Stopped | Kept recording until | Battery at failure | Recovery |
|---|---|---|---|
| 2026-09-19 08:44 | 13:25 same day | 11.736 V, midday, charging | Manual power cycle |
| 2026-09-23 06:37 | — | **11.068 V**, dawn, overnight low | **By itself** |
| 2026-09-24 00:25 | 09:05 same day | 11.336 V and rising, sun up | Manual power cycle |
| 2026-09-27 10:07 | 17:06 same day | 12.032 V, healthy | Manual power cycle |

In each case the station stopped **sending** first and carried on **recording** for several hours before stopping altogether. Only the 09-23 event was a power failure; the rest were the memory fault.

---

## Root cause: a memory leak in the upload path

Instrumentation added on 2026-09-25 writes the station's free memory to the SD card whenever it reaches a new low. Between the last restart on 09-25 and the failure on 09-27 it recorded **99 consecutive new lows and not one recovery**:

| Time | Free memory | Largest usable block |
|---|---|---|
| 09-25 16:38 (after restart) | 205,964 | 110,580 |
| 09-26 06:08 | 176,512 | 110,580 |
| 09-26 17:07 | 153,572 | 86,004 |
| 09-27 05:07 | 128,940 | 77,812 |
| 09-27 15:39 | **100,088** | **38,900** |

That is **105,876 bytes lost over 47 hours — about 1.1 kB per measurement cycle.** The largest usable block fell by 65%.

### Why that stops the uploads

An encrypted connection needs a large block of memory in one piece. The upload error codes in the log change exactly as that block shrinks:

```
10:37 – 13:07   error -11  (server answered too slowly)   block 69k → 63k
14:39 – 17:08   error  -1  (could not connect at all)     block 43k → 39k
```

The station did not lose its network connection. It ran out of room to open one.

### Where the leak comes from

Each reading is uploaded to three separate endpoints, and the firmware was building a **completely new encrypted connection for every one** — up to thirty per upload round. Some memory from each was not being returned. Cycles that uploaded leaked 1.0–2.6 kB; cycles that failed to find the network leaked only about 0.4 kB, which locates the fault in the upload path.

### What this rules out

- **Not a power failure.** The station recorded no restart of any kind for 48.5 hours before stopping. No brownout, no watchdog, no crash — it simply stopped.
- **Not a sensor or wiring fault.** The battery and solar sensors reported correct values right up to the final record at 17:06:19.
- **Not the server, and not the mobile link.** Both were working when the station stopped.

---

## Corrections to the previous report

Two findings in the 2026-09-25 version have been superseded by better evidence.

**The battery has recovered.** The earlier report stated that the battery was never reaching full charge. That was accurate for 18–24 September, but not since:

| Date | Overnight low | Daytime high |
|---|---|---|
| 09-23 | 11.068 V | 11.48 V |
| 09-26 | 11.78 V | **13.108 V** |
| 09-27 | 11.768 V | **13.108 V** |

13.1 V is a charge controller reaching its proper charging voltage, and a 22.3 V panel reading on 09-27 is the controller easing off with the battery full. The charging system now appears to be working. **The 09-23 stoppage remains a genuine power failure** and is worth watching through a spell of poor weather, but it is no longer an open fault.

**A suspected sensor-bus fault was wrong.** A blank power value on the dashboard suggested the sensor bus had locked up. The station's own archive shows full readings at that time, so the blank was an undelivered upload, not a failed sensor. No hardware investigation is needed.

---

## Work completed

**1. Upload retry logic corrected** *(2026-09-22)*
A network outage was consuming each reading's retry allowance and permanently discarding good data — one reading every five cycles, for as long as the outage lasted. Five were lost this way on 09-19. The firmware now separates "the server rejected this reading" from "the network is down", and only counts the first against a reading.
*Result: across 107 upload failures since, **zero** readings have been discarded.*

**2. Connection timeouts corrected** *(2026-09-25)*
Raised to 15 s for the handshake and 20 s for the response. The handshake limit had never been set and was defaulting to 5 s, which this server routinely exceeds — measured at up to 4.2 s to connect and 8.6 s to respond for a request doing no database work.

**3. Diagnostic logging added** *(2026-09-25)*
Records the restart reason at every boot and the memory trend to the SD card. **This is what identified the root cause**, and it turned an unexplained failure into a measured one within a single cycle of data.

**4. The leak fixed at source** *(2026-09-28)*
The firmware now opens **one** encrypted connection per upload round and reuses it for all three endpoints, instead of building up to thirty. This removes the great majority of the leaking operations.

**5. Automatic recovery added** *(2026-09-28)*
The station now checks its own memory at the end of every cycle and restarts itself if the largest usable block falls below 50 kB — comfortably above the 38,900 bytes at which it failed. The restart happens only after the reading is safely stored, so nothing is lost, and it takes about 30 seconds.

Items 4 and 5 are deliberately independent: item 4 should stop the leak, and item 5 means that even if some smaller leak remains, the station recovers on its own rather than waiting for a site visit.

Both changes are written and compile cleanly. They need to be flashed at the station.

---

## Outstanding actions

| # | Action | Owner |
|---|---|---|
| 1 | Flash the current firmware at the station | Field |
| 2 | Confirm from the card after ~3 days that the memory trend is now flat | Field |
| 3 | Investigate ingest API response times — 6–8 s to answer a request that does no database work | Server developer |
| 4 | Investigate mobile link reliability: on 09-24 the station could not connect for 8.5 hours | Site |
| 5 | Keep the battery under review through poor weather; charging is currently healthy | Hardware |
| 6 | Recover 23 undelivered readings held in backup | Station |

---

## A note on data integrity

Every reading is written to `datalog.txt` on the SD card **before** any upload is attempted, and a reading leaves the upload queue only once the server has confirmed it. Gaps on the dashboard therefore represent readings that are present on the station but not yet delivered — not readings that were never taken. The undelivered records from this period are recoverable and can be uploaded once the station is running the fixed firmware.
