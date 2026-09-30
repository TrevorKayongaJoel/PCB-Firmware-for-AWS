#ifndef PORTAL_HTML_H
#define PORTAL_HTML_H

#include <Arduino.h>

// Single-page config portal, served straight from flash with send_P().
// Everything is inline - the AP has no internet, so an external stylesheet or
// font would just hang the page. Light theme on purpose: this gets read on a
// phone in direct sunlight.
static const char PORTAL_INDEX_HTML[] PROGMEM = R"HTMLPAGE(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>AWS Setup</title>
<style>
  *{box-sizing:border-box;margin:0;padding:0}
  body{font:16px/1.5 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;
       background:#eef1f5;color:#16202c;padding:16px;max-width:560px;margin:0 auto}
  header{margin-bottom:20px}
  h1{font-size:20px;letter-spacing:-.01em}
  .sub{font:600 14px ui-monospace,Menlo,Consolas,monospace;color:#5a6b7d;margin-top:2px}
  .card{background:#fff;border:1px solid #d5dde6;border-radius:12px;
        padding:16px;margin-bottom:14px}
  .card.primary{border:2px solid #1668d6;box-shadow:0 2px 10px rgba(22,104,214,.12)}
  h2{font-size:13px;text-transform:uppercase;letter-spacing:.06em;
     color:#5a6b7d;margin-bottom:12px}
  .card.primary h2{color:#1668d6}
  label{display:block;font-size:14px;font-weight:600;margin:12px 0 5px}
  label:first-of-type{margin-top:0}
  input{width:100%;padding:12px;font-size:17px;border:1px solid #b9c5d2;
        border-radius:8px;background:#fff;color:#16202c;font-family:inherit}
  input:focus{outline:2px solid #1668d6;outline-offset:-1px;border-color:#1668d6}
  .bigrow{display:flex;align-items:center;gap:12px}
  .bigrow input{font-size:34px;font-weight:700;text-align:center;padding:14px;
                letter-spacing:-.02em}
  .unit{font-size:17px;font-weight:600;color:#5a6b7d;white-space:nowrap}
  .hint{font-size:13px;color:#5a6b7d;margin-top:8px}
  button{width:100%;padding:13px;font-size:16px;font-weight:600;
         border:0;border-radius:8px;cursor:pointer;margin-top:12px;
         font-family:inherit;background:#1668d6;color:#fff}
  button:active{transform:translateY(1px)}
  button.ghost{background:#fff;color:#16202c;border:1px solid #b9c5d2}
  button.warn{background:#fff;color:#b3261e;border:1px solid #e5b4b0}
  dl{display:grid;grid-template-columns:auto 1fr;gap:7px 14px;font-size:14px}
  dt{color:#5a6b7d}
  dd{text-align:right;font-weight:600;
     font-family:ui-monospace,Menlo,Consolas,monospace;overflow-wrap:anywhere}
  dd.bad{color:#b3261e}
  dd.good{color:#146c43}
  #toast{position:fixed;left:50%;transform:translateX(-50%);bottom:-90px;
         background:#16202c;color:#fff;padding:12px 20px;border-radius:8px;
         font-size:14px;font-weight:600;transition:bottom .25s;
         max-width:90vw;text-align:center}
  #toast.show{bottom:20px}
  #toast.err{background:#b3261e}
  /* Debug mode. Amber rather than the usual blue, because the one mistake that
     matters here is walking away without noticing the station is still in it. */
  .card.debug{border:2px solid #b06000;background:#fffaf2}
  .card.debug h2{color:#b06000}
  button.debug{background:#b06000}
  #banner{display:none;background:#b06000;color:#fff;padding:10px 14px;
          border-radius:8px;font-size:14px;font-weight:600;margin-bottom:14px}
  #banner.show{display:block}
  .live dd{font-size:15px}
  .live dd.stale{opacity:.45}
</style>
</head>
<body>

<header>
  <h1>Automatic Weather Station</h1>
  <div class="sub" id="h-station">connecting...</div>
</header>

<div id="banner">Debug mode - the station is not logging or uploading on its
  own. Exit when you are done.</div>

<section class="card debug" id="c-live" style="display:none">
  <h2>Live readings</h2>
  <p class="hint" style="margin:0 0 12px">Read straight from the sensors, right
     now. Nothing here is saved, queued or uploaded.</p>
  <dl class="live">
    <dt>Read at</dt><dd id="l-time">-</dd>
    <dt>Air temp</dt><dd id="l-temp">-</dd>
    <dt>Humidity</dt><dd id="l-hum">-</dd>
    <dt>Pressure</dt><dd id="l-press">-</dd>
    <dt>Batt temp</dt><dd id="l-btemp">-</dd>
    <dt>Battery</dt><dd id="l-batt">-</dd>
    <dt>Solar</dt><dd id="l-solar">-</dd>
    <dt>Wind now</dt><dd id="l-wind">-</dd>
    <dt>Rain so far</dt><dd id="l-rain">-</dd>
    <dt>Wind dir</dt><dd id="l-vane">-</dd>
    <dt>Soil</dt><dd id="l-soil">-</dd>
    <dt>Irradiance</dt><dd id="l-irr">-</dd>
  </dl>
  <h2 style="margin:16px 0 10px">Raw ADC volts</h2>
  <p class="hint" style="margin:0 0 10px">Ground truth. Turn the vane through a
     full circle and watch AIN0 to check its span; AIN3 is unconnected.</p>
  <dl class="live">
    <dt>AIN0 vane</dt><dd id="l-a0">-</dd>
    <dt>AIN1 soil</dt><dd id="l-a1">-</dd>
    <dt>AIN2 solar</dt><dd id="l-a2">-</dd>
    <dt>AIN3 n/c</dt><dd id="l-a3">-</dd>
  </dl>
  <button class="debug" id="b-read">Read sensors now</button>
  <label style="margin-top:14px">
    <input type="checkbox" id="b-auto" style="width:auto;margin-right:8px">
    Keep reading every 5 s
  </label>
</section>

<section class="card primary">
  <h2>Update interval</h2>
  <form id="f-interval">
    <div class="bigrow">
      <input type="number" id="interval" min="1" max="60" step="1" required>
      <span class="unit">minutes</span>
    </div>
    <p class="hint">How often the station wakes to measure, log and transmit.
       Between bursts it sleeps to save battery.</p>
    <button type="submit">Save interval</button>
  </form>
</section>

<section class="card">
  <h2>Station</h2>
  <form id="f-station">
    <label for="station">Station code</label>
    <input type="text" id="station" maxlength="20">
    <label for="clock">Set clock (EAT)</label>
    <input type="text" id="clock" placeholder="YYYY-MM-DD HH:MM:SS">
    <p class="hint">Leave the clock blank to keep the current time. It is set
       automatically from the internet whenever WiFi is available.</p>
    <button type="submit">Save station settings</button>
  </form>
</section>

<section class="card">
  <h2>Status</h2>
  <dl>
    <dt>Time</dt><dd id="s-time">-</dd>
    <dt>Clock source</dt><dd id="s-clock">-</dd>
    <dt>Next reading</dt><dd id="s-next">-</dd>
    <dt>WiFi</dt><dd id="s-wifi">-</dd>
    <dt>Queued uploads</dt><dd id="s-queue">-</dd>
    <dt>Air temp</dt><dd id="s-temp">-</dd>
    <dt>Humidity</dt><dd id="s-hum">-</dd>
    <dt>Wind avg / gust</dt><dd id="s-wind">-</dd>
    <dt>Rain</dt><dd id="s-rain">-</dd>
    <dt>Battery</dt><dd id="s-batt">-</dd>
    <dt>Solar</dt><dd id="s-solar">-</dd>
    <dt>Uptime</dt><dd id="s-up">-</dd>
  </dl>
</section>

<section class="card">
  <h2>More</h2>
  <button class="ghost" id="b-wifi">Configure WiFi network</button>
  <button class="ghost" id="b-upload">Upload queued readings now</button>
  <button class="debug" id="b-debug">Enter debug mode</button>
  <button class="warn" id="b-restart">Restart station</button>
  <button id="b-done">Done - sleep now</button>
</section>

<div id="toast"></div>

<script>
var $ = function(id){ return document.getElementById(id); };
var touched = {};
var toastTimer;
var inDebug = false;   // mirrors /api/status; drives which cards are shown
var reading = false;   // one live read in flight at a time
var autoTimer;

function toast(msg, isErr){
  var t = $('toast');
  t.textContent = msg;
  t.className = 'show' + (isErr ? ' err' : '');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(function(){ t.className = ''; }, 2600);
}

// Don't let a status refresh overwrite a field the user is editing.
function mark(id){
  $(id).addEventListener('input', function(){ touched[id] = true; });
}
mark('interval'); mark('station'); mark('clock');

function fmtDur(s){
  if (s < 0) return 'now';
  if (s < 60) return s + 's';
  var m = Math.floor(s / 60), h = Math.floor(m / 60);
  if (h > 0) return h + 'h ' + (m % 60) + 'm';
  return m + 'm ' + (s % 60) + 's';
}

function set(id, txt, cls){
  var el = $(id);
  el.textContent = txt;
  el.className = cls || '';
}

function refresh(){
  fetch('/api/status').then(function(r){ return r.json(); }).then(function(d){
    $('h-station').textContent = d.station;
    if (!touched['interval']) $('interval').value = d.intervalMin;
    if (!touched['station'])  $('station').value  = d.station;

    set('s-time', d.time, d.clockValid ? '' : 'bad');
    set('s-clock', d.clockSource, d.clockValid ? 'good' : 'bad');
    set('s-next', fmtDur(d.nextCycleSec));
    if (d.wifi.connected) set('s-wifi', d.wifi.ssid + '  ' + d.wifi.ip, 'good');
    else set('s-wifi', 'not connected', 'bad');
    set('s-queue', d.queueDepth + (d.queueDepth === 1 ? ' record' : ' records'),
        d.queueDepth > 0 ? 'bad' : 'good');

    if (d.haveReading){
      set('s-temp',  d.temp.toFixed(1) + ' C');
      set('s-hum',   d.humidity.toFixed(0) + ' %');
      set('s-wind',  d.windMS.toFixed(1) + ' / ' + d.gustMS.toFixed(1) + ' m/s');
      set('s-rain',  d.rain.toFixed(1) + ' mm');
      set('s-batt',  d.battV.toFixed(2) + ' V  ' + d.battI.toFixed(0) + ' mA');
      set('s-solar', d.solarV.toFixed(2) + ' V  ' + d.solarI.toFixed(0) + ' mA');
    } else {
      ['s-temp','s-hum','s-wind','s-rain','s-batt','s-solar'].forEach(function(i){
        set(i, 'no reading yet');
      });
    }
    set('s-up', fmtDur(d.uptimeSec));

    // Debug mode changes what the page is for, so it changes what it shows.
    inDebug = !!d.debug;
    $('banner').className = inDebug ? 'show' : '';
    $('c-live').style.display = inDebug ? '' : 'none';
    $('b-debug').style.display = inDebug ? 'none' : '';
    $('b-done').textContent = inDebug ? 'Exit debug - take a reading now'
                                      : 'Done - sleep now';
  }).catch(function(){ /* mid-handoff or busy; the next tick retries */ });
}

// ---- Live sensor reads (debug mode only) ----
// Values are dimmed while a read is in flight, so a slow one - the DHT22 retry
// path can take several seconds - never looks like a fresh reading that just
// happens to be unchanged.
function num(v, dp, unit){
  return (v === undefined || v === null) ? '--' : v.toFixed(dp) + ' ' + unit;
}

function readNow(){
  if (!inDebug || reading) return;
  reading = true;
  var dds = document.querySelectorAll('#c-live dd');
  for (var i = 0; i < dds.length; i++) dds[i].classList.add('stale');

  fetch('/api/read').then(function(r){ return r.json(); }).then(function(d){
    if (!d.ok) { toast(d.message || 'Read failed', true); return; }
    $('l-time').textContent  = d.time;
    $('l-temp').textContent  = num(d.temp, 2, 'C');
    $('l-hum').textContent   = num(d.humidity, 1, '%');
    $('l-press').textContent = num(d.pressure, 2, 'hPa');
    $('l-btemp').textContent = num(d.battTemp, 2, 'C');
    $('l-batt').textContent  = num(d.battV, 3, 'V') + '  ' + num(d.battI, 0, 'mA');
    $('l-solar').textContent = num(d.solarV, 3, 'V') + '  ' + num(d.solarI, 0, 'mA');

    // Labelled with the window they cover: these are the running counters since
    // the last burst, not the stale interval average the status card shows.
    $('l-wind').textContent = num(d.windMS, 2, 'm/s') +
        '  (' + d.windPulses + ' pulses / ' + d.sinceSec + 's)';
    $('l-rain').textContent = num(d.rainMM, 2, 'mm') +
        '  (' + d.rainTips + ' tips)';

    $('l-vane').textContent = (d.vaneDeg === undefined) ? 'out of range'
                                                        : d.vaneDeg + ' deg';
    $('l-soil').textContent = num(d.soilPct, 1, '%');
    $('l-irr').textContent  = num(d.irradiance, 1, 'W/m2');
    $('l-a0').textContent   = num(d.adc0, 4, 'V');
    $('l-a1').textContent   = num(d.adc1, 4, 'V');
    $('l-a2').textContent   = num(d.adc2, 4, 'V');
    $('l-a3').textContent   = num(d.adc3, 4, 'V');
  }).catch(function(){
    toast('Read failed', true);
  }).then(function(){
    reading = false;
    var d2 = document.querySelectorAll('#c-live dd');
    for (var j = 0; j < d2.length; j++) d2[j].classList.remove('stale');
  });
}

function post(path, body, okMsg){
  return fetch(path, {
    method: 'POST',
    headers: {'Content-Type':'application/x-www-form-urlencoded'},
    body: body || ''
  }).then(function(r){ return r.json(); }).then(function(d){
    toast(d.message || okMsg, !d.ok);
    if (d.ok) { touched = {}; refresh(); }
    return d;
  }).catch(function(){ toast('Station did not respond', true); });
}

$('f-interval').addEventListener('submit', function(e){
  e.preventDefault();
  post('/api/settings', 'interval=' + encodeURIComponent($('interval').value),
       'Interval saved');
});

$('f-station').addEventListener('submit', function(e){
  e.preventDefault();
  var b = 'station=' + encodeURIComponent($('station').value) +
          '&clock=' + encodeURIComponent($('clock').value);
  post('/api/settings', b, 'Saved').then(function(){ $('clock').value = ''; });
});

$('b-upload').addEventListener('click', function(){
  toast('Uploading...');
  post('/api/upload', '', 'Upload finished');
});

$('b-wifi').addEventListener('click', function(){
  if (!confirm('The setup network will restart so you can pick a WiFi network. '
             + 'Rejoin the station\'s setup network afterwards. Continue?')) return;
  post('/api/wifi', '', 'Switching to WiFi setup...');
});

$('b-debug').addEventListener('click', function(){
  if (!confirm('Enter debug mode? The station stays awake with the sensors '
             + 'powered so you can take live readings, and stops logging until '
             + 'you exit. Continue?')) return;
  post('/api/debug', '', 'Entering debug mode...');
});

$('b-read').addEventListener('click', readNow);

$('b-auto').addEventListener('change', function(){
  clearInterval(autoTimer);
  // 5 s, not faster: the read is synchronous on the station and the AM2302
  // will not produce a new sample more often than every 2 s anyway.
  if (this.checked) { readNow(); autoTimer = setInterval(readNow, 5000); }
});

$('b-restart').addEventListener('click', function(){
  if (!confirm('Restart the station now?')) return;
  post('/api/restart', '', 'Restarting...');
});

$('b-done').addEventListener('click', function(){
  post('/api/done', '', 'Closing portal - station going to sleep');
});

refresh();
setInterval(refresh, 3000);
</script>
</body>
</html>
)HTMLPAGE";

#endif
