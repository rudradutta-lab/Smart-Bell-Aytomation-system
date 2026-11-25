/*
  SMART BELL - ESP32 AUTOMATION SYSTEM
  Board: ESP32 Dev Module
  Relay IN: GPIO 26 (active-low by default)

  Features:
    - Mobile-first web dashboard matching the supplied Smart Bell screenshot
    - Manual ON / STOP control
    - Countdown timer presets and custom timer
    - Automatic ring duration and selectable beep patterns
    - Three daily schedules, saved in ESP32 Preferences (NVS)
    - Device information and live Wi-Fi / clock status
    - NTP clock in India Standard Time (IST)

  Libraries (included with the ESP32 Arduino board package):
    WiFi, WebServer, Preferences

  IMPORTANT:
    1. Enter your Wi-Fi credentials below.
    2. This sketch assumes an ACTIVE-LOW relay module.
    3. Use a relay module that accepts 3.3V logic on its IN pin.
*/

#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <time.h>

// -------------------- USER SETTINGS --------------------
const char* WIFI_SSID = "POCO";
const char* WIFI_PASS = "Rudr@9039";

constexpr uint8_t BUZZER_PIN = 26;
constexpr bool RELAY_ACTIVE_LOW = true;
constexpr long GMT_OFFSET_SEC = 5L * 3600L + 30L * 60L; // India Standard Time
constexpr int DAYLIGHT_OFFSET_SEC = 0;
constexpr uint8_t MAX_SCHEDULES = 3;

// -------------------- GLOBAL OBJECTS --------------------
WebServer server(80);
Preferences preferences;

struct DailySchedule {
  bool enabled;
  uint16_t minuteOfDay;
  uint32_t lastFiredDate;
};

DailySchedule schedules[MAX_SCHEDULES];

// Pattern IDs: 0=Continuous, 1=1s ON/1s OFF, 2=Double beep, 3=Short beep
uint8_t selectedPattern = 0;
uint16_t ringDurationSec = 10; // Automatic timer/schedule ring duration

bool relayOutputOn = false;
bool alarmActive = false;
bool alarmManual = false;
bool timerPending = false;
bool ntpConfigured = false;

uint32_t alarmStartedAt = 0;
uint32_t alarmDurationMs = 0;
uint32_t timerDueAt = 0;
uint32_t timerTotalSec = 0;
uint32_t lastWiFiRetryAt = 0;
uint32_t lastScheduleCheckAt = 0;

// -------------------- RELAY CONTROL --------------------
void writeRelay(bool on) {
  if (RELAY_ACTIVE_LOW) {
    if (on) {
      pinMode(BUZZER_PIN, OUTPUT);
      digitalWrite(BUZZER_PIN, LOW);   // Pull LOW to activate active-low relay
    } else {
      pinMode(BUZZER_PIN, INPUT);      // High-impedance OFF, matching the original wiring
    }
  } else {
    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, on ? HIGH : LOW);
  }
  relayOutputOn = on;
}

void stopAll() {
  alarmActive = false;
  alarmManual = false;
  timerPending = false;
  alarmDurationMs = 0;
  timerTotalSec = 0;
  writeRelay(false);
}

void startManualAlarm() {
  timerPending = false;
  alarmActive = true;
  alarmManual = true;
  alarmStartedAt = millis();
  alarmDurationMs = 0;
}

void startAutomaticAlarm() {
  timerPending = false;
  alarmActive = true;
  alarmManual = false;
  alarmStartedAt = millis();
  alarmDurationMs = (uint32_t)ringDurationSec * 1000UL;
  timerTotalSec = 0;
}

void startCountdown(uint32_t seconds) {
  alarmActive = false;
  alarmManual = false;
  writeRelay(false);
  timerPending = true;
  timerTotalSec = seconds;
  timerDueAt = millis() + seconds * 1000UL;
}

// -------------------- ALARM STATE MACHINE --------------------
bool patternWantsRelayOn(uint32_t elapsedMs) {
  switch (selectedPattern) {
    case 1: // One second ON, one second OFF
      return (elapsedMs % 2000UL) < 1000UL;

    case 2: { // Two short beeps, then a pause
      uint32_t phase = elapsedMs % 1800UL;
      return (phase < 220UL) || (phase >= 440UL && phase < 660UL);
    }

    case 3: // Short beep once per second
      return (elapsedMs % 1000UL) < 200UL;

    case 0:
    default: // Continuous ring
      return true;
  }
}

void updateAutomation() {
  const uint32_t now = millis();

  if (timerPending && (int32_t)(now - timerDueAt) >= 0) {
    startAutomaticAlarm();
    Serial.println("Countdown finished: Smart Bell activated.");
  }

  if (alarmActive) {
    if (!alarmManual && (uint32_t)(now - alarmStartedAt) >= alarmDurationMs) {
      alarmActive = false;
      alarmManual = false;
      writeRelay(false);
      Serial.println("Automatic ring completed.");
    } else {
      bool requestedOutput = patternWantsRelayOn(now - alarmStartedAt);
      if (requestedOutput != relayOutputOn) {
        writeRelay(requestedOutput);
      }
    }
  } else if (relayOutputOn) {
    writeRelay(false);
  }
}

// -------------------- TIME / DAILY SCHEDULES --------------------
bool getCurrentLocalTime(struct tm &timeInfo) {
  time_t now = time(nullptr);
  if (now < 1700000000) return false; // NTP time not set yet
  localtime_r(&now, &timeInfo);
  return true;
}

uint32_t dateKeyFromTm(const struct tm &timeInfo) {
  return (uint32_t)(timeInfo.tm_year + 1900) * 10000UL +
         (uint32_t)(timeInfo.tm_mon + 1) * 100UL +
         (uint32_t)timeInfo.tm_mday;
}

void configureNtpIfNeeded() {
  if (!ntpConfigured && WiFi.status() == WL_CONNECTED) {
    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC,
               "pool.ntp.org", "time.google.com", "time.nist.gov");
    ntpConfigured = true;
    Serial.println("NTP time synchronization requested (IST).");
  }
}

void checkDailySchedules() {
  struct tm localTime;
  if (!getCurrentLocalTime(localTime)) return;

  uint32_t today = dateKeyFromTm(localTime);
  uint16_t currentMinute = (uint16_t)(localTime.tm_hour * 60 + localTime.tm_min);

  for (uint8_t i = 0; i < MAX_SCHEDULES; i++) {
    if (schedules[i].enabled &&
        schedules[i].minuteOfDay == currentMinute &&
        schedules[i].lastFiredDate != today) {

      schedules[i].lastFiredDate = today;
      preferences.putUInt((String("fired") + String(i)).c_str(), today);
      startAutomaticAlarm();
      Serial.printf("Daily schedule %u activated.\n", (unsigned)(i + 1));
    }
  }
}

void loadSavedSettings() {
  preferences.begin("smartbell", false);

  selectedPattern = preferences.getUChar("pattern", 0);
  if (selectedPattern > 3) selectedPattern = 0;

  ringDurationSec = preferences.getUShort("duration", 10);
  if (ringDurationSec < 1 || ringDurationSec > 600) ringDurationSec = 10;

  const uint16_t defaultMinutes[MAX_SCHEDULES] = { 7 * 60, 12 * 60, 18 * 60 };
  for (uint8_t i = 0; i < MAX_SCHEDULES; i++) {
    String enabledKey = String("en") + String(i);
    String timeKey = String("time") + String(i);
    String firedKey = String("fired") + String(i);

    schedules[i].enabled = preferences.getBool(enabledKey.c_str(), false);
    schedules[i].minuteOfDay = preferences.getUShort(timeKey.c_str(), defaultMinutes[i]);
    if (schedules[i].minuteOfDay >= 1440) schedules[i].minuteOfDay = defaultMinutes[i];
    schedules[i].lastFiredDate = preferences.getUInt(firedKey.c_str(), 0);
  }
}

// -------------------- WEB PAGE --------------------
const char PAGE[] PROGMEM = R"SMARTBELLPAGE(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<meta name="theme-color" content="#0a1220">
<title>Smart Bell</title>
<style>
:root{color-scheme:dark;--bg:#0a1220;--panel:#121d30;--panel2:#101a2b;--line:#26344b;--muted:#8794aa;--text:#edf3ff;--blue:#4d78f5;--green:#16b85b;--red:#ed3b46;--purple:#7654ee}
*{box-sizing:border-box}html{min-height:100%;background:var(--bg)}body{margin:0;background:var(--bg);color:var(--text);font-family:Arial,Helvetica,sans-serif;min-height:100vh;padding:18px 15px 30px}
.app{width:100%;max-width:560px;margin:0 auto}.top{display:flex;align-items:center;justify-content:space-between;gap:12px;margin:8px 2px 24px}.brand{display:flex;align-items:center;gap:13px;min-width:0}.brand-icon{height:58px;width:58px;border-radius:17px;display:grid;place-items:center;font-size:30px;background:linear-gradient(140deg,#4b7cff,#7852ee);box-shadow:0 8px 24px #4d69ed35}.brand-icon svg{width:31px;height:31px}.bell-orb svg{width:96px;height:96px}.brand-name{font-size:21px;font-weight:700}.brand-sub{color:var(--muted);font-size:14px;margin-top:5px;white-space:nowrap}.clock{font-size:19px;font-weight:700;letter-spacing:1px;white-space:nowrap;color:#dce6fb}
.nav{display:grid;grid-template-columns:repeat(4,1fr);gap:4px;padding:6px;background:var(--panel2);border:1px solid var(--line);border-radius:18px;margin-bottom:20px}.nav button{padding:13px 3px;border:0;border-radius:13px;background:transparent;color:#909db2;font-size:14px}.nav button.active{background:var(--blue);color:#fff;box-shadow:0 5px 16px #426cf044}
.page{display:none}.page.active{display:block}.card{background:var(--panel);border:1px solid var(--line);border-radius:23px;padding:22px;margin-bottom:18px;box-shadow:0 9px 24px #00000012}.hero{text-align:center;padding:30px 22px 23px}.bell-orb{height:190px;width:190px;max-width:60vw;max-height:60vw;border-radius:50%;margin:0 auto 28px;background:linear-gradient(145deg,#293f7c,#17294d);display:grid;place-items:center;color:#aabef4;font-size:88px;box-shadow:inset 0 2px 12px #ffffff08}.bell-orb.on{background:linear-gradient(145deg,#52334f,#342047);color:#ffb1b9;box-shadow:0 0 32px #ef445433}.bell-orb.wait{background:linear-gradient(145deg,#254c83,#172b51);color:#91c5ff}.state{font-size:27px;font-weight:700;margin-bottom:8px}.hint{font-size:16px;color:var(--muted);min-height:20px}.countdown{font-size:30px;font-weight:700;letter-spacing:2px;color:#8dbdff;margin-top:13px;font-variant-numeric:tabular-nums}.actions{display:grid;grid-template-columns:1fr 1fr;gap:13px;margin-top:26px}.btn{border:0;border-radius:15px;padding:17px 10px;color:#fff;font-size:16px;cursor:pointer;min-height:54px}.btn:active,.chip:active{transform:scale(.98)}.on-btn{background:linear-gradient(135deg,#18bd60,#0e9e50)}.stop-btn{background:linear-gradient(135deg,#f0444d,#d92c3a)}.primary{background:linear-gradient(120deg,#397cf7,#7351ec)}.secondary{background:#24334b;border:1px solid #3a4b67}.section-title{font-size:14px;letter-spacing:1.4px;color:#9ba9c0;font-weight:700;margin:2px 0 20px;text-transform:uppercase}.small-note{font-size:13px;color:var(--muted);line-height:1.5}.pattern-line{display:flex;align-items:center;justify-content:space-between;gap:12px}.pattern-name{font-size:16px;color:#e4ecfb}.text-link{background:transparent;color:#8eb4ff;border:0;font-size:14px;padding:8px;cursor:pointer}
.chips{display:flex;flex-wrap:wrap;gap:9px}.chip{border:1px solid #2b3d58;background:#1a2941;color:#e3ebfc;border-radius:24px;padding:12px 16px;font-size:14px;cursor:pointer}.chip.selected{border-color:#5b83f6;background:#263f71}.field-grid{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin-top:19px}.field label{display:block;color:var(--muted);font-size:12px;margin-bottom:8px;text-transform:uppercase}.field input,.field select{width:100%;background:#0b1423;border:1px solid #2c3c56;color:#fff;border-radius:13px;padding:14px 12px;font-size:16px;min-width:0;outline:none}.field input:focus,.field select:focus{border-color:#5b83f6}.wide{width:100%;margin-top:15px}.schedule-row{display:grid;grid-template-columns:1fr 1.2fr auto;gap:9px;align-items:center;padding:13px 0;border-bottom:1px solid #27364d}.schedule-row:last-child{border-bottom:0}.slot-name{font-size:14px;font-weight:700}.slot-time{min-width:0}.slot-time input{width:100%;background:#0b1423;border:1px solid #34435c;border-radius:11px;padding:11px;color:#fff;font-size:15px}.toggle{width:43px;height:26px;accent-color:#4d78f5}.schedule-actions{display:flex;align-items:center;justify-content:space-between;gap:10px;margin-top:18px}.schedule-help{font-size:12px;line-height:1.5;color:var(--muted);margin-top:12px}.device-grid{display:grid;grid-template-columns:1fr 1fr;gap:10px}.device-item{background:#0c1627;border:1px solid #27364d;border-radius:14px;padding:14px;min-width:0}.device-label{font-size:11px;color:var(--muted);text-transform:uppercase;margin-bottom:8px}.device-value{font-size:15px;font-weight:700;overflow-wrap:anywhere}.status-dot{display:inline-block;width:8px;height:8px;border-radius:50%;background:#20c879;margin-right:7px}.status-dot.offline{background:#f05b65}.footer{text-align:center;color:#68778e;font-size:12px;padding:7px 0 10px}.footer strong{color:#70a7ff}
@media(max-width:380px){body{padding:14px 11px 25px}.card{padding:18px}.hero{padding:25px 15px 19px}.bell-orb{height:160px;width:160px;font-size:76px}.nav button{font-size:12px}.brand-icon{height:52px;width:52px}.brand-name{font-size:19px}.clock{font-size:16px}.schedule-row{grid-template-columns:.8fr 1.2fr auto}.chip{padding:11px 13px}}
</style>
</head>
<body>
<div class="app">
  <header class="top">
    <div class="brand">
      <div class="brand-icon"><svg viewBox="0 0 24 24" aria-hidden="true"><path d="M18 8a6 6 0 0 0-12 0c0 7-3 7-3 9h18c0-2-3-2-3-9M10 21h4" fill="none" stroke="white" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round"/></svg></div>
      <div><div class="brand-name">Smart Bell</div><div class="brand-sub" id="connectionText">Connecting...</div></div>
    </div>
    <div class="clock" id="clock">--:--:--</div>
  </header>

  <nav class="nav">
    <button class="active" data-page="control" onclick="showPage('control',this)">Control</button>
    <button data-page="schedule" onclick="showPage('schedule',this)">Schedule</button>
    <button data-page="patterns" onclick="showPage('patterns',this)">Patterns</button>
    <button data-page="device" onclick="showPage('device',this)">Device</button>
  </nav>

  <main>
    <section class="page active" id="page-control">
      <div class="card hero">
        <div class="bell-orb" id="bellOrb"><svg viewBox="0 0 24 24" aria-hidden="true"><path d="M18 8a6 6 0 0 0-12 0c0 7-3 7-3 9h18c0-2-3-2-3-9M10 21h4" fill="none" stroke="currentColor" stroke-width="1.35" stroke-linecap="round" stroke-linejoin="round"/></svg></div>
        <div class="state" id="stateText">Bell is OFF</div>
        <div class="hint" id="stateHint">Ready</div>
        <div class="countdown" id="countdown" hidden>00:00</div>
        <div class="actions">
          <button class="btn on-btn" onclick="controlBell(1)">Turn ON</button>
          <button class="btn stop-btn" onclick="controlBell(0)">Stop</button>
        </div>
      </div>

      <div class="card">
        <div class="section-title">BEEP PATTERN</div>
        <div class="pattern-line"><div><div class="pattern-name" id="patternSummary">Continuous ring</div><div class="small-note" id="durationSummary">Ring duration: 10 seconds</div></div><button class="text-link" onclick="showPage('patterns',document.querySelector('[data-page=patterns]'))">Change</button></div>
      </div>

      <div class="card">
        <div class="section-title">RING AFTER TIMER</div>
        <div class="chips">
          <button class="chip" onclick="startTimer(10)">10s</button>
          <button class="chip" onclick="startTimer(30)">30s</button>
          <button class="chip" onclick="startTimer(60)">1 min</button>
          <button class="chip" onclick="startTimer(300)">5 min</button>
          <button class="chip" onclick="startTimer(600)">10 min</button>
        </div>
        <div class="field-grid">
          <div class="field"><label for="customMin">MIN</label><input id="customMin" type="number" min="0" max="1440" value="0" inputmode="numeric"></div>
          <div class="field"><label for="customSec">SEC</label><input id="customSec" type="number" min="0" max="59" value="15" inputmode="numeric"></div>
        </div>
        <button class="btn primary wide" onclick="startCustomTimer()">Start</button>
        <button class="btn secondary wide" onclick="controlBell(0)">Cancel Timer</button>
      </div>
    </section>

    <section class="page" id="page-schedule">
      <div class="card">
        <div class="section-title">DAILY SCHEDULE</div>
        <div class="small-note">Set up to three daily bell times. Each enabled schedule repeats every day using the ESP32's synchronized clock.</div>
        <div id="scheduleList" style="margin-top:15px">
          <div class="small-note">Loading schedules...</div>
        </div>
        <div class="schedule-actions"><span class="small-note">Schedules are saved on the ESP32.</span><button class="btn primary" style="padding:12px 18px" onclick="saveAllSchedules()">Save</button></div>
        <div class="schedule-help">Daily scheduling requires Wi-Fi internet access so the ESP32 can synchronize time. Time zone: India (IST).</div>
      </div>
    </section>

    <section class="page" id="page-patterns">
      <div class="card">
        <div class="section-title">BEEP PATTERN</div>
        <div class="field"><label for="patternSelect">RING STYLE</label><select id="patternSelect"><option value="0">Continuous ring</option><option value="1">1 sec ON / 1 sec OFF</option><option value="2">Double beep</option><option value="3">Short beep every second</option></select></div>
        <div class="field" style="margin-top:18px"><label for="ringDuration">RING DURATION (SECONDS)</label><input id="ringDuration" type="number" min="1" max="600" value="10" inputmode="numeric"></div>
        <div class="small-note" style="margin-top:10px">This duration is used for timer-triggered and scheduled rings. Manual Turn ON stays active until you press Stop.</div>
        <button class="btn primary wide" onclick="savePatternSettings()">Save Pattern Settings</button>
      </div>
    </section>

    <section class="page" id="page-device">
      <div class="card">
        <div class="section-title">DEVICE INFORMATION</div>
        <div class="device-grid">
          <div class="device-item"><div class="device-label">Connection</div><div class="device-value" id="deviceConnection">Checking...</div></div>
          <div class="device-item"><div class="device-label">Wi-Fi Signal</div><div class="device-value" id="deviceRssi">--</div></div>
          <div class="device-item"><div class="device-label">IP Address</div><div class="device-value" id="deviceIp">--</div></div>
          <div class="device-item"><div class="device-label">Uptime</div><div class="device-value" id="deviceUptime">--</div></div>
          <div class="device-item"><div class="device-label">Clock Sync</div><div class="device-value" id="deviceClock">Waiting</div></div>
          <div class="device-item"><div class="device-label">Relay Output</div><div class="device-value" id="deviceRelay">OFF</div></div>
        </div>
        <button class="btn secondary wide" onclick="refreshStatus()">Refresh Device Status</button>
      </div>
      <div class="card"><div class="section-title">HARDWARE</div><div class="small-note">Board: ESP32 Dev Module<br>Relay control: GPIO 26<br>Relay logic: Active LOW<br>Time zone: Asia/Kolkata (IST)</div></div>
    </section>
  </main>
  <footer class="footer">Powered by <strong>ESP32</strong> · Smart Bell Automation</footer>
</div>

<script>
const patternNames = ['Continuous ring','1 sec ON / 1 sec OFF','Double beep','Short beep every second'];
let lastStatus = null;

function showPage(name, button) {
  document.querySelectorAll('.page').forEach(p => p.classList.remove('active'));
  document.getElementById('page-' + name).classList.add('active');
  document.querySelectorAll('.nav button').forEach(b => b.classList.remove('active'));
  const target = button || document.querySelector('[data-page="' + name + '"]');
  if (target) target.classList.add('active');
  if (name === 'schedule') loadSchedules();
  if (name === 'patterns') loadPatternSettings();
  if (name === 'device') refreshStatus();
}

async function request(url) {
  const response = await fetch(url, {cache:'no-store'});
  const body = await response.text();
  if (!response.ok) throw new Error(body || 'Request failed');
  return body;
}

async function controlBell(state) {
  try { await request('/api/control?state=' + state); await refreshStatus(); }
  catch (e) { alert('ESP32 connection failed. Check Wi-Fi and try again.'); }
}

async function startTimer(seconds) {
  if (!Number.isInteger(seconds) || seconds < 1 || seconds > 86400) return;
  try { await request('/api/timer?sec=' + seconds); await refreshStatus(); }
  catch (e) { alert('Could not start timer. Check the ESP32 connection.'); }
}

function startCustomTimer() {
  const minutes = Number(document.getElementById('customMin').value);
  const seconds = Number(document.getElementById('customSec').value);
  if (!Number.isInteger(minutes) || !Number.isInteger(seconds) || minutes < 0 || minutes > 1440 || seconds < 0 || seconds > 59) {
    alert('Enter valid minutes (0-1440) and seconds (0-59).'); return;
  }
  const total = minutes * 60 + seconds;
  if (total < 1 || total > 86400) { alert('Timer must be between 1 second and 24 hours.'); return; }
  startTimer(total);
}

async function refreshStatus() {
  try {
    const data = JSON.parse(await request('/api/status'));
    lastStatus = data;
    document.getElementById('clock').textContent = data.clock;
    document.getElementById('connectionText').textContent = data.connected ? ('Connected · ' + data.ip) : 'Connecting...';

    const orb = document.getElementById('bellOrb');
    const state = document.getElementById('stateText');
    const hint = document.getElementById('stateHint');
    const countdown = document.getElementById('countdown');
    orb.className = 'bell-orb';

    if (data.alarm) {
      state.textContent = 'Bell is ON';
      hint.textContent = data.manual ? 'Manual ringing · press Stop to silence' : 'Automatic ringing';
      orb.classList.add('on');
      countdown.hidden = true;
    } else if (data.pending) {
      state.textContent = 'Timer is SET';
      hint.textContent = 'Bell will ring automatically';
      orb.classList.add('wait');
      countdown.hidden = false;
      countdown.textContent = formatTime(data.left);
    } else {
      state.textContent = 'Bell is OFF';
      hint.textContent = 'Ready';
      countdown.hidden = true;
    }

    document.getElementById('patternSummary').textContent = patternNames[data.pattern] || patternNames[0];
    document.getElementById('durationSummary').textContent = 'Ring duration: ' + data.duration + ' seconds';
    document.getElementById('deviceConnection').innerHTML = data.connected ? '<span class="status-dot"></span>Online' : '<span class="status-dot offline"></span>Offline';
    document.getElementById('deviceRssi').textContent = data.connected ? data.rssi + ' dBm' : '--';
    document.getElementById('deviceIp').textContent = data.ip || '--';
    document.getElementById('deviceUptime').textContent = formatUptime(data.uptime);
    document.getElementById('deviceClock').textContent = data.timeSynced ? 'Synchronized' : 'Waiting for NTP';
    document.getElementById('deviceRelay').textContent = data.relay ? 'ON' : 'OFF';
  } catch (e) {
    document.getElementById('connectionText').textContent = 'Connection lost';
    document.getElementById('deviceConnection').textContent = 'Offline';
  }
}

function formatTime(total) {
  const h = Math.floor(total / 3600);
  const m = Math.floor((total % 3600) / 60);
  const s = total % 60;
  return h > 0 ? String(h).padStart(2,'0') + ':' + String(m).padStart(2,'0') + ':' + String(s).padStart(2,'0') : String(m).padStart(2,'0') + ':' + String(s).padStart(2,'0');
}

function formatUptime(seconds) {
  const d = Math.floor(seconds / 86400);
  const h = Math.floor((seconds % 86400) / 3600);
  const m = Math.floor((seconds % 3600) / 60);
  return (d ? d + 'd ' : '') + h + 'h ' + m + 'm';
}

async function loadSchedules() {
  try {
    const data = JSON.parse(await request('/api/schedules'));
    const container = document.getElementById('scheduleList');
    container.innerHTML = '';
    data.slots.forEach((slot, i) => {
      const row = document.createElement('div');
      row.className = 'schedule-row';
      row.innerHTML = '<div class="slot-name">Schedule ' + (i + 1) + '</div>' +
        '<div class="slot-time"><input type="time" id="scheduleTime' + i + '" value="' + slot.time + '"></div>' +
        '<input class="toggle" type="checkbox" id="scheduleEnabled' + i + '" aria-label="Enable schedule ' + (i + 1) + '">';
      container.appendChild(row);
      document.getElementById('scheduleEnabled' + i).checked = slot.enabled;
    });
  } catch (e) {
    document.getElementById('scheduleList').textContent = 'Could not load schedules. Check the ESP32 connection.';
  }
}

async function saveAllSchedules() {
  try {
    for (let i = 0; i < 3; i++) {
      const enabled = document.getElementById('scheduleEnabled' + i).checked ? 1 : 0;
      const time = document.getElementById('scheduleTime' + i).value;
      if (!time) throw new Error('Choose a time for each schedule slot.');
      await request('/api/schedule?slot=' + i + '&enabled=' + enabled + '&time=' + encodeURIComponent(time));
    }
    alert('Schedules saved on ESP32.');
  } catch (e) { alert(e.message || 'Could not save schedules.'); }
}

async function loadPatternSettings() {
  if (!lastStatus) await refreshStatus();
  if (lastStatus) {
    document.getElementById('patternSelect').value = String(lastStatus.pattern);
    document.getElementById('ringDuration').value = lastStatus.duration;
  }
}

async function savePatternSettings() {
  const pattern = Number(document.getElementById('patternSelect').value);
  const duration = Number(document.getElementById('ringDuration').value);
  if (!Number.isInteger(pattern) || pattern < 0 || pattern > 3 || !Number.isInteger(duration) || duration < 1 || duration > 600) {
    alert('Choose a valid pattern and ring duration from 1 to 600 seconds.'); return;
  }
  try {
    await request('/api/settings?pattern=' + pattern + '&duration=' + duration);
    await refreshStatus();
    alert('Pattern settings saved.');
  } catch (e) { alert('Could not save settings.'); }
}

setInterval(refreshStatus, 1000);
refreshStatus();
</script>
</body>
</html>
)SMARTBELLPAGE";

// -------------------- HTTP HANDLERS --------------------
void sendText(int code, const char* type, const String &message) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, type, message);
}

void handleRoot() {
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "text/html; charset=utf-8", PAGE);
}

void handleControl() {
  if (!server.hasArg("state")) {
    sendText(400, "text/plain", "Missing state");
    return;
  }
  if (server.arg("state") == "1") startManualAlarm();
  else stopAll();
  sendText(200, "text/plain", "OK");
}

void handleTimer() {
  if (!server.hasArg("sec")) {
    sendText(400, "text/plain", "Missing seconds");
    return;
  }
  long seconds = server.arg("sec").toInt();
  if (seconds < 1 || seconds > 86400) {
    sendText(400, "text/plain", "Timer must be between 1 and 86400 seconds");
    return;
  }
  startCountdown((uint32_t)seconds);
  sendText(200, "text/plain", "Timer started");
}

void handleSettings() {
  if (!server.hasArg("pattern") || !server.hasArg("duration")) {
    sendText(400, "text/plain", "Missing settings");
    return;
  }
  long pattern = server.arg("pattern").toInt();
  long duration = server.arg("duration").toInt();
  if (pattern < 0 || pattern > 3 || duration < 1 || duration > 600) {
    sendText(400, "text/plain", "Invalid pattern or duration");
    return;
  }
  selectedPattern = (uint8_t)pattern;
  ringDurationSec = (uint16_t)duration;
  preferences.putUChar("pattern", selectedPattern);
  preferences.putUShort("duration", ringDurationSec);
  sendText(200, "text/plain", "Settings saved");
}

void handleSchedules() {
  String json = "{\"slots\":[";
  for (uint8_t i = 0; i < MAX_SCHEDULES; i++) {
    if (i) json += ",";
    uint8_t hour = schedules[i].minuteOfDay / 60;
    uint8_t minute = schedules[i].minuteOfDay % 60;
    char timeText[6];
    snprintf(timeText, sizeof(timeText), "%02u:%02u", (unsigned)hour, (unsigned)minute);
    json += "{\"enabled\":";
    json += schedules[i].enabled ? "true" : "false";
    json += ",\"time\":\"";
    json += timeText;
    json += "\"}";
  }
  json += "]}";
  sendText(200, "application/json", json);
}

void handleSaveSchedule() {
  if (!server.hasArg("slot") || !server.hasArg("enabled") || !server.hasArg("time")) {
    sendText(400, "text/plain", "Missing schedule fields");
    return;
  }

  long slot = server.arg("slot").toInt();
  String timeValue = server.arg("time");
  if (slot < 0 || slot >= MAX_SCHEDULES || timeValue.length() != 5 || timeValue.charAt(2) != ':') {
    sendText(400, "text/plain", "Invalid schedule slot or time");
    return;
  }

  const uint8_t digitPositions[] = {0, 1, 3, 4};
  for (uint8_t p = 0; p < 4; p++) {
    char c = timeValue.charAt(digitPositions[p]);
    if (c < '0' || c > '9') {
      sendText(400, "text/plain", "Time must use HH:MM format");
      return;
    }
  }
  long hour = timeValue.substring(0, 2).toInt();
  long minute = timeValue.substring(3, 5).toInt();
  if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
    sendText(400, "text/plain", "Time must be HH:MM");
    return;
  }

  uint8_t index = (uint8_t)slot;
  schedules[index].enabled = server.arg("enabled") == "1";
  schedules[index].minuteOfDay = (uint16_t)(hour * 60 + minute);
  preferences.putBool((String("en") + String(index)).c_str(), schedules[index].enabled);
  preferences.putUShort((String("time") + String(index)).c_str(), schedules[index].minuteOfDay);
  sendText(200, "text/plain", "Schedule saved");
}

void handleStatus() {
  struct tm localTime;
  bool timeSynced = getCurrentLocalTime(localTime);
  char clockText[9] = "--:--:--";
  if (timeSynced) {
    snprintf(clockText, sizeof(clockText), "%02d:%02d:%02d",
             localTime.tm_hour, localTime.tm_min, localTime.tm_sec);
  }

  uint32_t secondsLeft = 0;
  if (timerPending) {
    int32_t remainingMs = (int32_t)(timerDueAt - millis());
    if (remainingMs > 0) secondsLeft = ((uint32_t)remainingMs + 999UL) / 1000UL;
  }

  bool connected = WiFi.status() == WL_CONNECTED;
  String ip = connected ? WiFi.localIP().toString() : "";
  int rssi = connected ? WiFi.RSSI() : 0;

  String json = "{";
  json += "\"connected\":"; json += connected ? "true" : "false";
  json += ",\"ip\":\""; json += ip; json += "\"";
  json += ",\"rssi\":"; json += String(rssi);
  json += ",\"uptime\":"; json += String(millis() / 1000UL);
  json += ",\"clock\":\""; json += clockText; json += "\"";
  json += ",\"timeSynced\":"; json += timeSynced ? "true" : "false";
  json += ",\"alarm\":"; json += alarmActive ? "true" : "false";
  json += ",\"manual\":"; json += alarmManual ? "true" : "false";
  json += ",\"pending\":"; json += timerPending ? "true" : "false";
  json += ",\"left\":"; json += String(secondsLeft);
  json += ",\"total\":"; json += String(timerTotalSec);
  json += ",\"relay\":"; json += relayOutputOn ? "true" : "false";
  json += ",\"pattern\":"; json += String(selectedPattern);
  json += ",\"duration\":"; json += String(ringDurationSec);
  json += "}";

  sendText(200, "application/json", json);
}

// -------------------- SETUP / LOOP --------------------
void setup() {
  Serial.begin(115200);
  delay(100);

  // Drive an active-low relay HIGH immediately so it starts OFF.
  writeRelay(false);
  loadSavedSettings();

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.print("Connecting to Wi-Fi");
  uint32_t startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && (uint32_t)(millis() - startAttempt) < 15000UL) {
    delay(300);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Wi-Fi connected.");
    Serial.print("Open Smart Bell: http://");
    Serial.println(WiFi.localIP());
    configureNtpIfNeeded();
  } else {
    Serial.println("Wi-Fi not connected yet. The ESP32 will keep retrying.");
  }

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/control", HTTP_GET, handleControl);
  server.on("/api/timer", HTTP_GET, handleTimer);
  server.on("/api/settings", HTTP_GET, handleSettings);
  server.on("/api/schedules", HTTP_GET, handleSchedules);
  server.on("/api/schedule", HTTP_GET, handleSaveSchedule);
  server.onNotFound([]() { sendText(404, "text/plain", "Not found"); });
  server.begin();
  Serial.println("Smart Bell web server started on port 80.");
}

void loop() {
  server.handleClient();

  if (WiFi.status() == WL_CONNECTED) {
    configureNtpIfNeeded();
    if ((uint32_t)(millis() - lastScheduleCheckAt) >= 1000UL) {
      lastScheduleCheckAt = millis();
      checkDailySchedules();
    }
  } else if ((uint32_t)(millis() - lastWiFiRetryAt) >= 15000UL) {
    lastWiFiRetryAt = millis();
    WiFi.reconnect();
  }

  updateAutomation();
  delay(2);
}
