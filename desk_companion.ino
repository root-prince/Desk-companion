// ============================================================
//  DESK COMPANION v4.1
//  ESP32 + 0.96" SSD1306 128x64 (blue) + MPU6050 + DHT11
//  + rotary encoder + foil touch + buzzer + AP web dashboard
//  + HX711 load cell under the notepad (post-sprint notes check)
// ============================================================
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>
#include <Preferences.h>
#include <sys/time.h>
#include <time.h>

// ---------------- User config ----------------
const char* AP_SSID = "DeskCompanion";
const char* AP_PASS = "focus123";          // CHANGE ME (min 8 chars)
#define ENC_STEPS_PER_DETENT 4             // set 2 if one click = 2 steps
#define ENC_REVERSE false                  // true if knob direction is backwards
const uint8_t CONTRAST_NORMAL = 0x9F;      // lower = longer blue OLED life
const uint8_t CONTRAST_DIM    = 0x08;      // used while sleeping
const float   TOUCH_RATIO     = 0.70;      // touch = reading < baseline*ratio

// ---------------- Notes check (load cell) ----------------
const bool          NOTES_ENABLED         = true;            // false = skip the notes step entirely
const unsigned long NOTES_WRITE_TARGET_MS = 20000UL;         // writing time needed to count as "notes done"
const unsigned long NOTES_TIMEOUT_MS      = 5UL * 60UL * 1000UL;  // give up (counts as skipped) after this long
const float         HX_NOISE_MULT         = 6.0f;            // threshold = idle noise * this ...
const float         HX_MIN_THRESH         = 1500.0f;         // ... but never below this (raw ADC counts)
const int           HX_CAL_SAMPLES        = 15;              // samples used to measure idle noise at boot
#define HX_DEBUG 0                                           // 1 = print load-cell activity to Serial (for tuning)

// ---------------- Display ----------------
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
Preferences prefs;
WebServer server(80);
DNSServer dnsServer;
portMUX_TYPE dataMux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE hxMux   = portMUX_INITIALIZER_UNLOCKED;
uint8_t oledAddr = 0x3C;
uint8_t curContrast = 0;

// ---------------- Pins ----------------
#define PIN_I2C_SDA    25
#define PIN_I2C_SCL    26
#define PIN_BUZZER     18
#define PIN_FOIL_TOUCH 4
#define PIN_DHT        19
#define DHTTYPE        DHT11
#define PIN_ENC_CLK    32
#define PIN_ENC_DT     33
#define PIN_ENC_SW     27
#define PIN_HX_DOUT    16
#define PIN_HX_SCK     17
DHT dht(PIN_DHT, DHTTYPE);

// ---------------- Climate ----------------
volatile float currentTemp = 0.0;
volatile float currentHumidity = 0.0;
volatile bool  dhtValid = false;
unsigned long lastDhtReadTime = 0;
const unsigned long DHT_READ_INTERVAL = 2000;
const float TEMP_HOT_THRESHOLD  = 30.0;
const float TEMP_COLD_THRESHOLD = 18.0;

// ---------------- MPU6050 ----------------
const uint8_t MPU_ADDR = 0x68;
const uint8_t REG_CONFIG = 0x1A;
const uint8_t REG_ACCEL_CONFIG = 0x1C;
const uint8_t REG_PWR_MGMT_1 = 0x6B;
const uint8_t REG_ACCEL_XOUT_H = 0x3B;
bool mpuOnline = false;
int  mpuFails = 0;
unsigned long lastMpuRetry = 0;
float shakeEnergy = 0.0;
const float SHAKE_FORCE_CUTOFF = 1.4;
const float SHAKE_TRIGGER_SCORE = 55.0;
const float WAKE_MOTION_THRESHOLD = 0.35;

// ---------------- Load cell (HX711) ----------------
volatile bool hxOnline = false;           // found + calibrated at boot
float   hxNoise = 0, hxThresh = HX_MIN_THRESH, hxActivity = 0;
int32_t hxLast = 0;
bool    hxHaveLast = false, hxWriting = false;
unsigned long hxLastSample = 0;

// ---------------- Encoder / button ----------------
const int SPRINT_PRESETS[] = {25, 40, 55};
int selectedPresetIdx = 0;
bool menuActive = false;
unsigned long menuTimeout = 0;
volatile int32_t encRaw = 0;
volatile uint8_t encPrev = 0;
int32_t lastDetent = 0;
const int8_t ENC_TABLE[16] = {0,-1,1,0, 1,0,0,-1, -1,0,0,1, 0,1,-1,0};

bool btnStable = HIGH, btnLastRead = HIGH, btnLongFired = false;
unsigned long btnChangeT = 0, btnPressT = 0;
unsigned long inputLockUntil = 0;           // ignore input right after boot

enum HintType { HINT_NONE, HINT_HOLD_STOP, HINT_ON_BREAK, HINT_HOLD_SKIP };
HintType hintType = HINT_NONE;
unsigned long hintUntil = 0;

// ---------------- Pomodoro ----------------
enum PomoState { POMO_IDLE, POMO_TRANSITION, POMO_RUNNING, POMO_NOTES, POMO_WATER_ALERT, POMO_MEAL_ALERT };
volatile PomoState pomoState = POMO_IDLE;
volatile unsigned long pomoTimerStart = 0;
volatile unsigned long currentPomoDuration = 25UL * 60UL * 1000UL;
int chosenSprintMins = 25;
unsigned long transitionStart = 0;
const unsigned long TRANSITION_MS = 1100;
const unsigned long POMO_MEAL_DURATION = 15UL * 60UL * 1000UL;
const unsigned long WATER_RECHIME_MS = 5UL * 60UL * 1000UL;
unsigned long lastWaterChime = 0;
bool halfwayAlerted = false;
volatile bool mealLogged = false;

// post-sprint notes step
unsigned long notesStart = 0;
unsigned long notesWriteMs = 0;
int pendingDone = 0;                      // pomos done today, used after notes to pick water/meal alert

bool hourglassFlipped = false;
unsigned long nextFlipCheck = 0;
unsigned long flipRestoreTime = 0;

int cfgWaterFreq = 2;
int cfgMealLimit = 4;

// ---------------- Stats (shared across cores) ----------------
struct Stats { int pomos, water, meals, pets, focusMins, notes, skipped; };
Stats stats = {0, 0, 0, 0, 0, 0, 0};
int statsKey = 0;                         // yyyymmdd the stats belong to
volatile bool statsDirty = false;
unsigned long lastStatsSave = 0;
volatile int currentStreak = 0;
int lastStudyKey = 0;

// ---------------- Time ----------------
struct tm currentTm;
const char* DAYS_OF_WEEK[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
volatile bool timeSynced = false;         // synced from phone this boot
volatile bool requestTimeSave = false;
unsigned long lastEpochSave = 0;
unsigned long lastDayCheck = 0;

// ---------------- Touch / Wi-Fi ----------------
volatile bool wifiEnabled = false;
volatile bool requestWifiToggle = false;
float touchBaseline = 60;
float touchThreshold = 42;
uint8_t touchHits = 0;
bool touchState = false;
unsigned long lastTouchSample = 0;
unsigned long touchStartTime = 0;
bool touchActive = false;
bool longHoldTriggered = false;
const unsigned long LONG_HOLD_DURATION = 7000;

volatile bool isPetting = false;
unsigned long petEndTime = 0;

// ---------------- Bot lifecycle ----------------
enum BotState { STATE_ACTIVE, STATE_DROOPY, STATE_SLEEPING, STATE_WAKING, STATE_DIZZY };
volatile BotState currentState = STATE_ACTIVE;
unsigned long stateTimer = 0;
unsigned long dizzyEndTime = 0;

const int leftEyeCenterX = 44;
const int rightEyeCenterX = 84;
const int eyeCenterY = 32;
const int eyeWidth = 24;
const int maxEyeHeight = 26;
const int eyeCornerRadius = 5;

float currentEyeHeight = 26.0, targetEyeHeight = 26.0;
float currentTopLidCut = 0.0, targetTopLidCut = 0.0;
float currentGazeX = 0, currentGazeY = 0;
int targetGazeX = 0, targetGazeY = 0;
unsigned long nextGazeShift = 0;

enum BlinkStage { BLINK_IDLE, BLINK_CLOSING, BLINK_OPENING };
BlinkStage blinkState = BLINK_IDLE;
unsigned long nextBlinkTime = 0;
unsigned long lastFrameTime = 0;

// ============================================================
//  Non-blocking sound engine
// ============================================================
struct Note { uint16_t f; uint16_t d; };
#define SND_Q 48
Note sndQ[SND_Q];
uint8_t sndHead = 0, sndTail = 0;
unsigned long sndNoteEnd = 0;
bool sndBusy = false;

void sndPush(uint16_t f, uint16_t d) {
  uint8_t n = (sndTail + 1) % SND_Q;
  if (n == sndHead) return;
  sndQ[sndTail] = {f, d};
  sndTail = n;
}
bool sndIdle() { return sndHead == sndTail && !sndBusy; }
void sndStop() { sndHead = sndTail; noTone(PIN_BUZZER); sndBusy = false; }
void sndUpdate(unsigned long now) {
  if (sndBusy && (long)(now - sndNoteEnd) < 0) return;
  if (sndHead == sndTail) {
    if (sndBusy) { noTone(PIN_BUZZER); sndBusy = false; }
    return;
  }
  Note n = sndQ[sndHead];
  sndHead = (sndHead + 1) % SND_Q;
  if (n.f) tone(PIN_BUZZER, n.f); else noTone(PIN_BUZZER);
  sndNoteEnd = now + n.d;
  sndBusy = true;
}
void sndFlushBlocking() { while (!sndIdle()) { sndUpdate(millis()); delay(1); } }
void sndSeq(const uint16_t* s, int pairs) {
  sndStop();
  for (int i = 0; i < pairs; i++) sndPush(s[i * 2], s[i * 2 + 1]);
}

void soundEncoderTick()   { if (sndIdle()) sndPush(2400, 6); }
void soundEncoderSelect() { const uint16_t s[] = {1200,40, 0,5, 1800,60}; sndSeq(s, 3); }
void soundBootChime()     { const uint16_t s[] = {523,70, 0,10, 659,70, 0,10, 784,70, 0,10, 1046,150}; sndSeq(s, 7); }
void soundHalfwayChime()  { const uint16_t s[] = {880,70, 0,10, 1175,120}; sndSeq(s, 3); }
void soundHourglassFlip() { const uint16_t s[] = {1400,30, 0,5, 900,40}; sndSeq(s, 3); }
void soundPet()           { const uint16_t s[] = {1046,60, 0,5, 1568,80}; sndSeq(s, 3); }
void soundStartSprint()   { const uint16_t s[] = {784,80, 0,10, 1046,100, 0,10, 1568,160}; sndSeq(s, 5); }
void soundPomoComplete()  { const uint16_t s[] = {659,90, 0,10, 880,90, 0,10, 1318,180}; sndSeq(s, 5); }
void soundNotesDone()     { const uint16_t s[] = {988,60, 0,10, 1318,60, 0,10, 1760,140}; sndSeq(s, 5); }
void soundCancel()        { const uint16_t s[] = {900,60, 0,10, 600,60, 0,10, 400,140}; sndSeq(s, 5); }
void soundWifiOn()        { const uint16_t s[] = {1000,50, 0,10, 1500,50, 0,10, 2000,80}; sndSeq(s, 5); }
void soundWifiOff()       { const uint16_t s[] = {2000,50, 0,10, 1500,50, 0,10, 1000,80}; sndSeq(s, 5); }
void soundBreakOver()     { const uint16_t s[] = {784,90, 0,30, 784,90, 0,30, 1046,200}; sndSeq(s, 5); }
void soundError()         { const uint16_t s[] = {300,300, 0,200, 300,300, 0,200, 300,300}; sndSeq(s, 5); }
// queued (don't interrupt previous fanfare)
void soundWaterAlert()    { sndPush(0,60); sndPush(1760,60); sndPush(0,20); sndPush(1760,60); }
void soundMealAlert()     { sndPush(0,60); sndPush(523,100); sndPush(0,30); sndPush(659,100); sndPush(0,30); sndPush(523,160); }
void soundDizzy() {
  sndStop();
  for (int f = 800; f >= 250; f -= 110) { sndPush(f, 30); sndPush(0, 5); }
}

// ============================================================
//  Time / date helpers
// ============================================================
bool applySystemTime(int y, int m, int d, int hr, int mn, int sc) {
  if (y < 2024 || y > 2099 || m < 1 || m > 12 || d < 1 || d > 31 ||
      hr < 0 || hr > 23 || mn < 0 || mn > 59 || sc < 0 || sc > 59) return false;
  struct tm t = {0};
  t.tm_year = y - 1900; t.tm_mon = m - 1; t.tm_mday = d;
  t.tm_hour = hr; t.tm_min = mn; t.tm_sec = sc; t.tm_isdst = 0;
  time_t epoch = mktime(&t);
  if (t.tm_mday != d) return false;         // e.g. 31 Feb rejected
  struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
  settimeofday(&tv, NULL);
  return true;
}

int dateKey(const struct tm& t) { return (t.tm_year + 1900) * 10000 + (t.tm_mon + 1) * 100 + t.tm_mday; }

long keyToDayNum(int key) {
  struct tm t = {0};
  t.tm_year = key / 10000 - 1900;
  t.tm_mon  = (key / 100) % 100 - 1;
  t.tm_mday = key % 100;
  t.tm_hour = 12;
  return (long)(mktime(&t) / 86400);
}
int daysBetween(int a, int b) { return (int)(keyToDayNum(b) - keyToDayNum(a)); }

void saveEpoch() { prefs.putLong64("epoch", (int64_t)time(NULL)); }

void saveStats() {
  Stats s;
  portENTER_CRITICAL(&dataMux); s = stats; portEXIT_CRITICAL(&dataMux);
  prefs.putInt("sKey", statsKey);
  prefs.putInt("sPomo", s.pomos);
  prefs.putInt("sWater", s.water);
  prefs.putInt("sMeal", s.meals);
  prefs.putInt("sPet", s.pets);
  prefs.putInt("sFocus", s.focusMins);
  prefs.putInt("sNote", s.notes);
  prefs.putInt("sSkip", s.skipped);
  statsDirty = false;
}

void addStat(int* field, int amount) {
  portENTER_CRITICAL(&dataMux); *field += amount; portEXIT_CRITICAL(&dataMux);
  statsDirty = true;
}

void checkDayRollover() {
  int today = dateKey(currentTm);
  // New day -> reset daily stats (ignore clock going backwards)
  if (today != statsKey && (statsKey == 0 || daysBetween(statsKey, today) > 0)) {
    portENTER_CRITICAL(&dataMux);
    stats = Stats{};
    portEXIT_CRITICAL(&dataMux);
    statsKey = today;
    saveStats();
  }
  // Broken streak -> show 0 immediately
  if (lastStudyKey && currentStreak > 0 && daysBetween(lastStudyKey, today) >= 2) {
    currentStreak = 0;
    prefs.putInt("streak", 0);
  }
}

void registerStudyPomo(int mins) {
  addStat(&stats.pomos, 1);
  addStat(&stats.focusMins, mins);
  int today = dateKey(currentTm);
  if (today == lastStudyKey) return;
  if (lastStudyKey == 0) {
    currentStreak = 1;
  } else {
    int d = daysBetween(lastStudyKey, today);
    if (d < 0) return;                       // clock behind, don't corrupt streak
    currentStreak = (d == 1) ? currentStreak + 1 : 1;
  }
  lastStudyKey = today;
  prefs.putInt("streak", currentStreak);
  prefs.putInt("lastKey", lastStudyKey);
}

// ============================================================
//  Web dashboard
// ============================================================
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Desk Companion</title>
<style>
:root{--bg:#0f172a;--card:#1e293b;--border:#334155;--text:#f8fafc;--muted:#94a3b8;--accent:#38bdf8;--green:#34d399;--amber:#fbbf24;--red:#f87171}
*{box-sizing:border-box;margin:0;padding:0;font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif}
body{background:var(--bg);color:var(--text);padding:1.25rem;display:flex;justify-content:center}
.container{max-width:640px;width:100%;display:flex;flex-direction:column;gap:1rem}
header{display:flex;justify-content:space-between;align-items:center;border-bottom:1px solid var(--border);padding-bottom:.75rem}
h1{font-size:1.25rem;font-weight:700;color:var(--accent)}
.badge{font-size:.75rem;background:#064e3b;color:var(--green);border:1px solid var(--green);padding:.25rem .6rem;border-radius:9999px;text-transform:uppercase;font-weight:600}
.warn{background:#78350f;color:var(--amber);border:1px solid var(--amber);border-radius:.5rem;padding:.6rem;font-size:.85rem;display:none}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(130px,1fr));gap:.75rem}
.card{background:var(--card);border:1px solid var(--border);border-radius:.75rem;padding:1rem}
.card-title{font-size:.75rem;color:var(--muted);text-transform:uppercase;margin-bottom:.35rem}
.card-value{font-size:1.45rem;font-weight:700}
.row{display:flex;justify-content:space-between;font-size:.9rem;margin-bottom:.5rem}
.section-title{font-size:.85rem;font-weight:600;margin-bottom:.75rem;color:var(--muted);text-transform:uppercase}
.form-group{display:flex;flex-direction:column;gap:.4rem;margin-bottom:.75rem}
input[type="datetime-local"]{background:#0f172a;border:1px solid var(--border);color:var(--text);padding:.5rem;border-radius:.5rem;font-size:.9rem}
.actions{display:grid;grid-template-columns:1fr 1fr;gap:.75rem;margin-top:.5rem}
button{background:var(--accent);color:#0f172a;border:none;font-weight:600;padding:.75rem;border-radius:.5rem;cursor:pointer}
button.secondary{background:var(--border);color:var(--text)}
button.danger{background:#7f1d1d;color:var(--red)}
</style>
</head>
<body>
<div class="container">
  <header><h1>Desk Companion</h1><span class="badge" id="botState">Active</span></header>
  <div class="warn" id="syncWarn">Clock restored after restart and may be behind. Tap Sync below.</div>
  <div class="grid">
    <div class="card"><div class="card-title">Study Streak</div><div class="card-value" id="valStreak" style="color:var(--amber)">0 Days 🔥</div></div>
    <div class="card"><div class="card-title">Companion Time</div><div class="card-value" id="valClock" style="font-size:1.15rem;margin-top:.35rem">--:--</div></div>
    <div class="card"><div class="card-title">Pomos Today</div><div class="card-value" id="valPomos" style="color:var(--green)">0</div></div>
    <div class="card"><div class="card-title">Notes Written</div><div class="card-value" id="valNotes" style="color:var(--green)">0</div></div>
    <div class="card"><div class="card-title">Water</div><div class="card-value" id="valWater" style="color:var(--accent)">0</div></div>
    <div class="card"><div class="card-title">Meals</div><div class="card-value" id="valMeals">0</div></div>
  </div>
  <div class="card">
    <div class="section-title">Date & Clock Calibration</div>
    <button style="width:100%;margin-bottom:.75rem" onclick="syncDeviceTime()">⚡ Sync Phone Time & Date</button>
    <div class="form-group">
      <label style="font-size:.8rem;color:var(--muted)">Manual Date & Time:</label>
      <input type="datetime-local" id="manualDateTime">
    </div>
    <button class="secondary" style="width:100%" onclick="applyManualTime()">Set Manual Date/Time</button>
  </div>
  <div class="card">
    <div class="row"><span>Focus Mode:</span><strong id="pomoStatus">Idle</strong></div>
    <div class="row"><span>Focus Time Today:</span><strong id="valFocus">0 mins</strong></div>
    <div class="row"><span>Notes Skipped:</span><strong id="valSkip">0</strong></div>
    <div class="row"><span>Notepad Sensor:</span><strong id="valHx">--</strong></div>
    <div class="row"><span>Room Climate:</span><strong id="valEnv">--°C | --%</strong></div>
    <div class="row"><span>Head Pets:</span><strong id="valPets">0</strong></div>
  </div>
  <div class="actions">
    <button class="secondary" onclick="window.location.href='/download-csv'">Download CSV Log</button>
    <button class="danger" onclick="resetSession()">Reset Today</button>
  </div>
</div>
<script>
function fmt(s){const m=Math.floor(s/60),x=s%60;return String(m).padStart(2,'0')+':'+String(x).padStart(2,'0');}
async function updateDashboard(){
  try{
    const d=await (await fetch('/api/status')).json();
    botState.textContent=d.state;
    pomoStatus.textContent=d.pomo+(d.remain>0?' ('+fmt(d.remain)+' left)':'');
    valStreak.textContent=d.streak+' Days 🔥';
    valClock.textContent=d.day+' '+d.time+' ('+d.date+')';
    valPomos.textContent=d.pomos; valWater.textContent=d.water; valMeals.textContent=d.meals;
    valNotes.textContent=d.notes; valSkip.textContent=d.skip;
    valHx.textContent=d.hx?'online':'not found';
    valFocus.textContent=d.focus+' mins';
    valEnv.textContent=d.dht?(d.temp.toFixed(1)+'°C | '+Math.round(d.hum)+'%'):'sensor error';
    valPets.textContent=d.pets;
    syncWarn.style.display=d.synced?'none':'block';
  }catch(e){}
}
async function postTime(dt,sec){
  const r=await fetch('/api/set-time',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({year:dt.getFullYear(),month:dt.getMonth()+1,day:dt.getDate(),hour:dt.getHours(),minute:dt.getMinutes(),second:sec})});
  alert(r.ok?'Clock set: '+dt.toLocaleString():'Invalid date/time');
  updateDashboard();
}
function syncDeviceTime(){const n=new Date();postTime(n,n.getSeconds());}
function applyManualTime(){const v=manualDateTime.value;if(!v){alert('Pick a date & time first!');return;}postTime(new Date(v),0);}
async function resetSession(){if(confirm("Reset today's counters?")){await fetch('/api/reset',{method:'POST'});updateDashboard();}}
setInterval(updateDashboard,2000);updateDashboard();
</script>
</body>
</html>
)rawliteral";

int jsonInt(const String& body, const char* key, int def) {
  String k = String("\"") + key + "\":";
  int i = body.indexOf(k);
  if (i < 0) return def;
  return body.substring(i + k.length()).toInt();
}

const char* pomoName() {
  switch (pomoState) {
    case POMO_TRANSITION:  return "Starting";
    case POMO_RUNNING:     return "Running";
    case POMO_NOTES:       return "Writing Notes";
    case POMO_WATER_ALERT: return "Water Check";
    case POMO_MEAL_ALERT:  return "Meal Break";
    default:               return "Idle";
  }
}
const char* stateName() {
  if (isPetting) return "Loved";
  switch (currentState) {
    case STATE_ACTIVE:   return "Active";
    case STATE_DROOPY:   return "Droopy";
    case STATE_SLEEPING: return "Sleeping";
    case STATE_WAKING:   return "Waking";
    default:             return "Dizzy";
  }
}

void WebServerTask(void *pvParameters) {
  auto serveHome = []() {
    server.sendHeader("Cache-Control", "no-cache");
    server.send_P(200, "text/html", INDEX_HTML);
  };
  server.on("/", HTTP_GET, serveHome);
  server.on("/generate_204", HTTP_GET, serveHome);
  server.on("/hotspot-detect.html", HTTP_GET, serveHome);

  server.on("/api/status", HTTP_GET, []() {
    struct tm t; time_t n = time(NULL); localtime_r(&n, &t);   // own copy, thread-safe
    Stats s;
    portENTER_CRITICAL(&dataMux); s = stats; portEXIT_CRITICAL(&dataMux);

    unsigned long remain = 0, ms = millis();
    if (pomoState == POMO_RUNNING) {
      unsigned long el = ms - pomoTimerStart;
      remain = (currentPomoDuration > el) ? (currentPomoDuration - el) / 1000 : 0;
    } else if (pomoState == POMO_MEAL_ALERT) {
      unsigned long el = ms - pomoTimerStart;
      remain = (POMO_MEAL_DURATION > el) ? (POMO_MEAL_DURATION - el) / 1000 : 0;
    }

    char buf[520];
    snprintf(buf, sizeof(buf),
      "{\"state\":\"%s\",\"pomo\":\"%s\",\"streak\":%d,\"day\":\"%s\",\"time\":\"%02d:%02d\","
      "\"date\":\"%02d/%02d/%04d\",\"temp\":%.1f,\"hum\":%.1f,\"dht\":%s,\"pomos\":%d,\"water\":%d,"
      "\"meals\":%d,\"pets\":%d,\"focus\":%d,\"notes\":%d,\"skip\":%d,\"hx\":%s,\"synced\":%s,\"remain\":%lu}",
      stateName(), pomoName(), (int)currentStreak, DAYS_OF_WEEK[t.tm_wday], t.tm_hour, t.tm_min,
      t.tm_mday, t.tm_mon + 1, t.tm_year + 1900, (float)currentTemp, (float)currentHumidity,
      dhtValid ? "true" : "false", s.pomos, s.water, s.meals, s.pets, s.focusMins, s.notes, s.skipped,
      hxOnline ? "true" : "false", timeSynced ? "true" : "false", remain);
    server.send(200, "application/json", String(buf));
  });

  server.on("/api/set-time", HTTP_POST, []() {
    if (!server.hasArg("plain")) { server.send(400, "text/plain", "No body"); return; }
    String b = server.arg("plain");
    bool ok = applySystemTime(jsonInt(b, "year", -1), jsonInt(b, "month", -1), jsonInt(b, "day", -1),
                              jsonInt(b, "hour", 12), jsonInt(b, "minute", 0), jsonInt(b, "second", 0));
    if (ok) { requestTimeSave = true; server.send(200, "text/plain", "OK"); }
    else server.send(400, "text/plain", "Invalid");
  });

  server.on("/download-csv", HTTP_GET, []() {
    struct tm t; time_t n = time(NULL); localtime_r(&n, &t);
    Stats s;
    portENTER_CRITICAL(&dataMux); s = stats; portEXIT_CRITICAL(&dataMux);
    char csv[460];
    snprintf(csv, sizeof(csv),
      "Metric,Value\nCurrent_Streak_Days,%d\nDate,%02d/%02d/%04d\nDay,%s\nPomodoros_Completed,%d\n"
      "Total_Focus_Minutes,%d\nNotes_Completed,%d\nNotes_Skipped,%d\nWater_Logged,%d\nMeals_Logged,%d\nHead_Pets,%d\n",
      (int)currentStreak, t.tm_mday, t.tm_mon + 1, t.tm_year + 1900, DAYS_OF_WEEK[t.tm_wday],
      s.pomos, s.focusMins, s.notes, s.skipped, s.water, s.meals, s.pets);
    server.sendHeader("Content-Disposition", "attachment; filename=session_log.csv");
    server.send(200, "text/csv", String(csv));
  });

  server.on("/api/reset", HTTP_POST, []() {
    portENTER_CRITICAL(&dataMux); stats = Stats{}; portEXIT_CRITICAL(&dataMux);
    statsDirty = true;
    server.send(200, "text/plain", "OK");
  });

  server.onNotFound(serveHome);

  for (;;) {
    if (requestWifiToggle) {
      requestWifiToggle = false;
      bool on = !wifiEnabled;
      if (on) {
        WiFi.mode(WIFI_AP);
        WiFi.softAP(AP_SSID, AP_PASS);
        dnsServer.start(53, "*", WiFi.softAPIP());
        server.begin();
      } else {
        server.stop();
        dnsServer.stop();
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_OFF);
      }
      wifiEnabled = on;
    }
    if (wifiEnabled) {
      dnsServer.processNextRequest();
      server.handleClient();
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

// ============================================================
//  Forward declarations
// ============================================================
void drawBoxEyes(int ox, int oy, int h, int topLidCut);
void drawSleepingEyes(int ox, int oy);
void drawHappyArcEyes(int ox, int oy);
void drawDizzyEyes();
void drawTemplateSmile(int ox, int oy);
void drawSmallGentleSmile(int ox, int oy);
void drawCuteWarmSmile(int ox, int oy);
void drawShiveringMouth(int ox, int oy);
void drawChompMouth(unsigned long now);
void drawDizzyMouth();
void drawSweatDrip(unsigned long now);
void drawSnowflakeIcon(int x, int y);
void drawWaterDropIcon();
void drawFloatingZ(unsigned long now);
void drawNotesProgress(unsigned long now);
void drawTopHeader(unsigned long now);
void drawEncoderMenu();
void drawTransition(unsigned long now);
void drawHourglassBot(unsigned long now);
void drawHint(unsigned long now);
void scheduleNextGaze();
void scheduleNextBlink();
void updateBotLifecycle(unsigned long now);
void handleBlinkLogic(unsigned long now);
void runAnimatedBootSequence();
void printCentered(const char* s, int y, int cx = 64);

// ============================================================
//  Encoder ISR
// ============================================================
void IRAM_ATTR encISR() {
  uint8_t s = (digitalRead(PIN_ENC_CLK) << 1) | digitalRead(PIN_ENC_DT);
  encPrev = ((encPrev << 2) | s) & 0x0F;
  encRaw += ENC_TABLE[encPrev];
}

int32_t encDetent() {
  int32_t r = encRaw;
  if (ENC_REVERSE) r = -r;
  return (r >= 0) ? r / ENC_STEPS_PER_DETENT : (r - (ENC_STEPS_PER_DETENT - 1)) / ENC_STEPS_PER_DETENT;
}

// ============================================================
//  MPU helpers
// ============================================================
void mpuWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(MPU_ADDR); Wire.write(reg); Wire.write(val); Wire.endTransmission();
}
bool initMPU() {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(REG_PWR_MGMT_1);
  Wire.write(0x00);
  if (Wire.endTransmission() != 0) return false;
  mpuWrite(REG_CONFIG, 0x03);        // DLPF ~44Hz
  mpuWrite(REG_ACCEL_CONFIG, 0x08);  // +-4g -> 8192 LSB/g
  return true;
}
void i2cBusRecover() {
  Wire.end();
  delay(2);
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(100000);
  Wire.setTimeOut(25);
}

// ============================================================
//  HX711 load cell (bit-banged, channel A, gain 128)
//  Used to detect pen activity on the notepad: writing makes the
//  reading jitter far more than an idle pad does.
// ============================================================
bool hxReady() { return digitalRead(PIN_HX_DOUT) == LOW; }

// Returns false if no conversion is ready. SCK stays high only ~2us per
// pulse, well under the 60us that would put the HX711 to sleep.
bool hxRead(int32_t& out) {
  if (!hxReady()) return false;
  uint32_t v = 0;
  portENTER_CRITICAL(&hxMux);
  for (int i = 0; i < 24; i++) {
    digitalWrite(PIN_HX_SCK, HIGH);
    delayMicroseconds(1);
    v = (v << 1) | (uint32_t)digitalRead(PIN_HX_DOUT);
    digitalWrite(PIN_HX_SCK, LOW);
    delayMicroseconds(1);
  }
  digitalWrite(PIN_HX_SCK, HIGH);    // 25th pulse: next read = channel A, gain 128
  delayMicroseconds(1);
  digitalWrite(PIN_HX_SCK, LOW);
  portEXIT_CRITICAL(&hxMux);
  if (v & 0x800000UL) v |= 0xFF000000UL;   // sign-extend 24 -> 32 bit
  out = (int32_t)v;
  return true;
}

// Measures the idle noise floor (hands off the pad) and sets the writing threshold.
void hxCalibrate() {
  hxOnline = false;
  int32_t v = 0;
  unsigned long t0 = millis();
  while (!hxRead(v)) {                       // wait for first conversion; none = not connected
    if (millis() - t0 > 1500) return;
    delay(5);
  }
  float sum = 0;
  int n = 0;
  int32_t prev = v;
  t0 = millis();
  while (n < HX_CAL_SAMPLES && millis() - t0 < 3000) {
    if (hxRead(v)) { sum += fabsf((float)(v - prev)); prev = v; n++; }
    delay(2);
  }
  if (n < 5) return;
  hxNoise = sum / n;
  hxThresh = fmaxf(hxNoise * HX_NOISE_MULT, HX_MIN_THRESH);
  hxLast = prev;
  hxHaveLast = true;
  hxActivity = 0;
  hxWriting = false;
  hxLastSample = millis();
  hxOnline = true;
}

// Call every loop. Cheap when no new sample is ready.
void hxUpdate(unsigned long now) {
  int32_t raw;
  if (!hxRead(raw)) return;
  unsigned long dt = now - hxLastSample;
  hxLastSample = now;
  if (dt > 250) dt = 250;
  if (!hxHaveLast) { hxLast = raw; hxHaveLast = true; return; }

  float ad = fabsf((float)(raw - hxLast));
  hxLast = raw;
  float cap = hxThresh * 4.0f;               // one big bump (placing a book) can't fake "writing"
  if (ad > cap) ad = cap;
  hxActivity += (ad - hxActivity) * 0.35f;

  if (hxWriting) { if (hxActivity < hxThresh * 0.6f) hxWriting = false; }   // hysteresis
  else if (hxActivity > hxThresh) hxWriting = true;

  if (pomoState == POMO_NOTES && hxWriting) notesWriteMs += dt;

#if HX_DEBUG
  Serial.printf("hx raw=%ld act=%.0f thr=%.0f writing=%d notesMs=%lu\n",
                (long)raw, hxActivity, hxThresh, hxWriting ? 1 : 0, notesWriteMs);
#endif
}

// ============================================================
//  Actions
// ============================================================
void wakeBot(unsigned long now) {
  if (currentState == STATE_SLEEPING || currentState == STATE_DROOPY) {
    currentState = STATE_WAKING;
    targetEyeHeight = maxEyeHeight;
    targetTopLidCut = 0.0;
    stateTimer = now + 2200;
  } else if (currentState == STATE_ACTIVE) {
    stateTimer = now + random(120000, 180000);
  }
}

void showHint(HintType h, unsigned long now) { hintType = h; hintUntil = now + 1800; }

void startSprint(int mins, unsigned long now) {
  chosenSprintMins = mins;
  currentPomoDuration = (unsigned long)mins * 60UL * 1000UL;
  halfwayAlerted = false;
  hourglassFlipped = false;
  menuActive = false;
  transitionStart = now;
  pomoState = POMO_TRANSITION;
  soundStartSprint();
}

void cancelSprint() {
  pomoState = POMO_IDLE;
  hourglassFlipped = false;
  soundCancel();
}

// After the sprint (and the notes step): meal break, water check, or back to idle.
void applyPostSprint(unsigned long now) {
  if (pendingDone % cfgMealLimit == 0) {
    pomoState = POMO_MEAL_ALERT;
    pomoTimerStart = now;
    mealLogged = false;
    soundMealAlert();
  } else if (pendingDone % cfgWaterFreq == 0) {
    pomoState = POMO_WATER_ALERT;
    lastWaterChime = now;
    soundWaterAlert();
  } else {
    pomoState = POMO_IDLE;
  }
}

void startNotes(unsigned long now) {
  notesStart = now;
  notesWriteMs = 0;
  hxActivity = 0;                            // only writing *after* the sprint counts
  hxWriting = false;
  pomoState = POMO_NOTES;
}

void finishNotes(unsigned long now) { applyPostSprint(now); }

void skipNotes(unsigned long now) {
  addStat(&stats.skipped, 1);
  soundCancel();
  finishNotes(now);
}

void logWater(unsigned long now) {
  addStat(&stats.water, 1);
  pomoState = POMO_IDLE;
  soundPet();
  wakeBot(now);
}

void logMeal() {
  if (mealLogged) return;
  mealLogged = true;
  addStat(&stats.meals, 1);
  soundPet();
}

void onShortClick(unsigned long now) {
  wakeBot(now);
  switch (pomoState) {
    case POMO_WATER_ALERT: logWater(now); break;
    case POMO_MEAL_ALERT:  if (!mealLogged) logMeal(); else showHint(HINT_ON_BREAK, now); break;
    case POMO_NOTES:       showHint(HINT_HOLD_SKIP, now); break;
    case POMO_TRANSITION:
    case POMO_RUNNING:     showHint(HINT_HOLD_STOP, now); break;
    default:
      if (menuActive) {
        soundEncoderSelect();
        startSprint(SPRINT_PRESETS[selectedPresetIdx], now);
      } else {
        menuActive = true;
        menuTimeout = now + 6000;
        soundEncoderTick();
      }
  }
}

void onLongPress(unsigned long now) {
  if (pomoState == POMO_RUNNING || pomoState == POMO_TRANSITION) cancelSprint();
  else if (pomoState == POMO_NOTES) skipNotes(now);
  else if (menuActive) { menuActive = false; soundCancel(); }
}

void onEncoder(int diff, unsigned long now) {
  wakeBot(now);
  if (pomoState == POMO_RUNNING || pomoState == POMO_TRANSITION) { showHint(HINT_HOLD_STOP, now); return; }
  if (pomoState == POMO_NOTES) { showHint(HINT_HOLD_SKIP, now); return; }
  if (pomoState == POMO_MEAL_ALERT) { showHint(HINT_ON_BREAK, now); return; }
  selectedPresetIdx = ((selectedPresetIdx + diff) % 3 + 3) % 3;
  menuActive = true;
  menuTimeout = now + 6000;
  soundEncoderTick();
}

void onTap(unsigned long now) {
  if (pomoState == POMO_WATER_ALERT) { logWater(now); return; }
  if (pomoState == POMO_MEAL_ALERT)  { logMeal(); return; }   // logs meal, lockout continues
  addStat(&stats.pets, 1);
  isPetting = true;
  petEndTime = now + 2500;
  currentState = STATE_ACTIVE;
  targetEyeHeight = maxEyeHeight;
  targetTopLidCut = 0.0;
  stateTimer = now + random(120000, 180000);
  soundPet();
}

void calibrateTouch() {
  uint32_t sum = 0; int n = 0;
  for (int i = 0; i < 40; i++) {
    uint16_t v = touchRead(PIN_FOIL_TOUCH);
    if (v > 0) { sum += v; n++; }
    delay(5);
  }
  touchBaseline = n ? (float)sum / n : 60.0;
  touchThreshold = touchBaseline * TOUCH_RATIO;
}

void setContrast(uint8_t c) {
  if (c == curContrast) return;
  display.ssd1306_command(SSD1306_SETCONTRAST);
  display.ssd1306_command(c);
  curContrast = c;
}

// ============================================================
//  Setup
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(200);

  setenv("TZ", "UTC0", 1);   // phone sends local time; store as-is
  tzset();

  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_ENC_CLK, INPUT_PULLUP);
  pinMode(PIN_ENC_DT, INPUT_PULLUP);
  pinMode(PIN_ENC_SW, INPUT_PULLUP);
  pinMode(PIN_HX_DOUT, INPUT_PULLUP);        // pull-up: a missing HX711 reads "not ready", never garbage
  pinMode(PIN_HX_SCK, OUTPUT);
  digitalWrite(PIN_HX_SCK, LOW);
  encPrev = (digitalRead(PIN_ENC_CLK) << 1) | digitalRead(PIN_ENC_DT);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_CLK), encISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_DT), encISR, CHANGE);

  // ---- Preferences ----
  prefs.begin("botdata", false);
  currentStreak = prefs.getInt("streak", 0);
  lastStudyKey  = prefs.getInt("lastKey", 0);
  if (lastStudyKey == 0) {                       // migrate v3 keys
    int y = prefs.getInt("lYear", 0);
    if (y > 0) lastStudyKey = y * 10000 + prefs.getInt("lMonth", 1) * 100 + prefs.getInt("lDay", 1);
  }
  statsKey        = prefs.getInt("sKey", 0);
  stats.pomos     = prefs.getInt("sPomo", 0);
  stats.water     = prefs.getInt("sWater", 0);
  stats.meals     = prefs.getInt("sMeal", 0);
  stats.pets      = prefs.getInt("sPet", 0);
  stats.focusMins = prefs.getInt("sFocus", 0);
  stats.notes     = prefs.getInt("sNote", 0);
  stats.skipped   = prefs.getInt("sSkip", 0);

  // ---- Restore clock ----
  int64_t savedEpoch = prefs.getLong64("epoch", 0);
  if (savedEpoch > 1700000000LL) {
    struct timeval tv = { .tv_sec = (time_t)savedEpoch, .tv_usec = 0 };
    settimeofday(&tv, NULL);
  } else {
    applySystemTime(2026, 10, 24, 12, 0, 0);
  }

  // ---- I2C + OLED ----
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(100000);
  Wire.setTimeOut(25);

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    oledAddr = 0x3D;
    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3D)) {
      Serial.println("ERR: OLED not found at 0x3C/0x3D");
      for (;;) { soundError(); sndFlushBlocking(); delay(2000); }
    }
  }
  setContrast(CONTRAST_NORMAL);

  mpuOnline = initMPU();
  dht.begin();

  runAnimatedBootSequence();   // also calibrates touch + load cell

  requestWifiToggle = true;    // AP on at boot
  xTaskCreatePinnedToCore(WebServerTask, "WebTask", 8192, NULL, 1, NULL, 0);

  time_t n = time(NULL);
  localtime_r(&n, &currentTm);
  checkDayRollover();

  scheduleNextGaze();
  scheduleNextBlink();
  unsigned long now = millis();
  stateTimer = now + 120000;
  inputLockUntil = now + 1500;
  lastDetent = encDetent();
  lastEpochSave = now;
  lastStatsSave = now;
  hxLastSample = now;
}

// ============================================================
//  Main loop (core 1)
// ============================================================
void loop() {
  unsigned long now = millis();
  sndUpdate(now);

  // 1. Clock
  time_t nowT = time(NULL);
  localtime_r(&nowT, &currentTm);
  if (now - lastDayCheck >= 1000) { lastDayCheck = now; checkDayRollover(); }

  // 2. DHT11
  if (now - lastDhtReadTime >= DHT_READ_INTERVAL) {
    lastDhtReadTime = now;
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t) && !isnan(h)) { currentTemp = t; currentHumidity = h; dhtValid = true; }
  }

  // 3. Encoder (interrupt-driven)
  int32_t det = encDetent();
  if (det != lastDetent) {
    int diff = (int)(det - lastDetent);
    lastDetent = det;
    if ((long)(now - inputLockUntil) >= 0) onEncoder(diff, now);
  }

  // 4. Button (debounced, short click + long hold)
  bool r = digitalRead(PIN_ENC_SW);
  if (r != btnLastRead) { btnLastRead = r; btnChangeT = now; }
  if (now - btnChangeT > 25 && r != btnStable) {
    btnStable = r;
    if (r == LOW) { btnPressT = now; btnLongFired = false; }
    else if (!btnLongFired && now - btnPressT < 800 && (long)(now - inputLockUntil) >= 0) onShortClick(now);
  }
  if (btnStable == LOW && !btnLongFired && now - btnPressT >= 1500) {
    btnLongFired = true;
    if ((long)(now - inputLockUntil) >= 0) onLongPress(now);
  }

  if (menuActive && (long)(now - menuTimeout) >= 0) menuActive = false;

  // 5. Touch (sampled every 20ms, 3-hit debounce, drifting baseline)
  if (now - lastTouchSample >= 20) {
    lastTouchSample = now;
    uint16_t v = touchRead(PIN_FOIL_TOUCH);
    if (v > 0) {
      bool raw = v < touchThreshold;
      if (raw) { if (touchHits < 3) touchHits++; }
      else {
        touchHits = 0;
        if (!touchActive) {
          touchBaseline += (v - touchBaseline) * 0.01f;
          touchThreshold = touchBaseline * TOUCH_RATIO;
        }
      }
      touchState = touchHits >= 3;
    }
  }

  if (touchState) {
    if (!touchActive) {
      touchActive = true;
      touchStartTime = now;
      longHoldTriggered = false;
    } else if (!longHoldTriggered && now - touchStartTime >= LONG_HOLD_DURATION) {
      longHoldTriggered = true;
      if (wifiEnabled) soundWifiOff(); else soundWifiOn();
      requestWifiToggle = true;
    }
  } else if (touchActive) {
    touchActive = false;
    if (!longHoldTriggered && now - touchStartTime < 1500 && (long)(now - inputLockUntil) >= 0) onTap(now);
  }

  if (isPetting && (long)(now - petEndTime) >= 0) isPetting = false;

  // 5b. Load cell (notepad pen activity)
  if (hxOnline) hxUpdate(now);

  // 6. Pomodoro lifecycle
  if (pomoState == POMO_TRANSITION) {
    if (now - transitionStart >= TRANSITION_MS) {
      pomoState = POMO_RUNNING;
      pomoTimerStart = now;
      nextFlipCheck = now + random(25000, 45000);
    }
  } else if (pomoState == POMO_RUNNING) {
    unsigned long elapsed = now - pomoTimerStart;

    if (!halfwayAlerted && elapsed >= currentPomoDuration / 2) {
      halfwayAlerted = true;
      soundHalfwayChime();
    }

    if (elapsed >= currentPomoDuration) {
      hourglassFlipped = false;
      registerStudyPomo(chosenSprintMins);
      soundPomoComplete();
      int done;
      portENTER_CRITICAL(&dataMux); done = stats.pomos; portEXIT_CRITICAL(&dataMux);
      pendingDone = done;

      if (hxOnline && NOTES_ENABLED) startNotes(now);   // notes first, then water/meal alert
      else applyPostSprint(now);
      stateTimer = now + random(120000, 180000);
    } else {
      if (!hourglassFlipped && (long)(now - nextFlipCheck) >= 0) {
        hourglassFlipped = true;
        flipRestoreTime = now + 3500;
        soundHourglassFlip();
      } else if (hourglassFlipped && (long)(now - flipRestoreTime) >= 0) {
        hourglassFlipped = false;
        nextFlipCheck = now + random(25000, 50000);
        soundHourglassFlip();
      }
    }
  } else if (pomoState == POMO_NOTES) {
    if (notesWriteMs >= NOTES_WRITE_TARGET_MS) {
      addStat(&stats.notes, 1);
      soundNotesDone();
      finishNotes(now);
    } else if (now - notesStart >= NOTES_TIMEOUT_MS) {
      skipNotes(now);                                   // never trap the user forever
    }
  } else if (pomoState == POMO_WATER_ALERT) {
    if (now - lastWaterChime >= WATER_RECHIME_MS) { lastWaterChime = now; soundWaterAlert(); }
  } else if (pomoState == POMO_MEAL_ALERT) {
    if (now - pomoTimerStart >= POMO_MEAL_DURATION) {
      pomoState = POMO_IDLE;     // meal only counted if user confirmed
      soundBreakOver();
      wakeBot(now);
    }
  }

  // 7. Persistence (core 1 only touches NVS)
  if (requestTimeSave) {
    requestTimeSave = false;
    timeSynced = true;
    saveEpoch();
    lastEpochSave = now;
    nowT = time(NULL);
    localtime_r(&nowT, &currentTm);
    checkDayRollover();
  }
  if (now - lastEpochSave >= 300000UL) { lastEpochSave = now; saveEpoch(); }
  if (statsDirty && now - lastStatsSave >= 30000UL) { lastStatsSave = now; saveStats(); }

  // 8. Render (~40 FPS)
  if (now - lastFrameTime >= 25) {
    lastFrameTime = now;

    // --- MPU6050 ---
    if (!mpuOnline) {
      if (now - lastMpuRetry > 5000) { lastMpuRetry = now; mpuOnline = initMPU(); mpuFails = 0; }
    } else {
      bool ok = false;
      Wire.beginTransmission(MPU_ADDR);
      Wire.write(REG_ACCEL_XOUT_H);
      if (Wire.endTransmission(false) == 0 && Wire.requestFrom((uint8_t)MPU_ADDR, (size_t)6, true) >= 6) {
        ok = true;
        int16_t rawX = Wire.read() << 8 | Wire.read();
        int16_t rawY = Wire.read() << 8 | Wire.read();
        int16_t rawZ = Wire.read() << 8 | Wire.read();
        float ax = rawX / 8192.0, ay = rawY / 8192.0, az = rawZ / 8192.0;
        float dev = fabsf(sqrtf(ax * ax + ay * ay + az * az) - 1.0f);

        if (dev > WAKE_MOTION_THRESHOLD) wakeBot(now);

        if (dev > SHAKE_FORCE_CUTOFF) shakeEnergy += dev * 12.0;
        else shakeEnergy -= 3.5;
        shakeEnergy = constrain(shakeEnergy, 0.0, 100.0);

        if (shakeEnergy >= SHAKE_TRIGGER_SCORE && currentState != STATE_DIZZY) {
          currentState = STATE_DIZZY;
          dizzyEndTime = now + 2500;
          shakeEnergy = 0.0;
          soundDizzy();
        }
      }
      if (ok) mpuFails = 0;
      else if (++mpuFails >= 10) {
        i2cBusRecover();
        mpuOnline = initMPU();
        mpuFails = 0;
        lastMpuRetry = now;
      }
    }

    updateBotLifecycle(now);

    if (currentState == STATE_ACTIVE && !isPetting && pomoState == POMO_IDLE) {
      if ((long)(now - nextGazeShift) >= 0) scheduleNextGaze();
      currentGazeX += (targetGazeX - currentGazeX) * 0.20;
      currentGazeY += (targetGazeY - currentGazeY) * 0.20;
    } else {
      currentGazeX += (0 - currentGazeX) * 0.2;
      currentGazeY += (0 - currentGazeY) * 0.2;
    }

    currentTopLidCut += (targetTopLidCut - currentTopLidCut) * 0.08;
    handleBlinkLogic(now);

    // Dim blue OLED while asleep (burn-in / lifetime)
    bool asleep = currentState == STATE_SLEEPING && pomoState == POMO_IDLE && !menuActive;
    setContrast(asleep ? CONTRAST_DIM : CONTRAST_NORMAL);

    bool isHot  = dhtValid && currentTemp >= TEMP_HOT_THRESHOLD;
    bool isCold = dhtValid && currentTemp <= TEMP_COLD_THRESHOLD;

    int shiverX = 0, shiverY = 0;
    if (isCold && currentState != STATE_SLEEPING && currentState != STATE_DIZZY && !isPetting) {
      shiverX = (now % 80 < 40) ? 1 : -1;
      shiverY = (now % 120 < 60) ? 1 : 0;
    }

    display.clearDisplay();

    if (pomoState == POMO_TRANSITION) {
      drawTransition(now);
    } else if (menuActive) {
      drawEncoderMenu();
    } else if (pomoState == POMO_RUNNING) {
      drawHourglassBot(now);
    } else {
      drawTopHeader(now);
      int gx = (int)currentGazeX + shiverX;
      int gy = (int)currentGazeY + shiverY;
      int curH = (int)currentEyeHeight;
      int curLid = (int)currentTopLidCut;

      if (currentState == STATE_DIZZY) {
        drawDizzyEyes();
        drawDizzyMouth();
      } else if (isPetting) {
        drawHappyArcEyes(shiverX, shiverY);
        drawTemplateSmile(shiverX, shiverY);
      } else if (pomoState == POMO_WATER_ALERT) {
        drawBoxEyes(gx, gy, curH, 0);
        drawTemplateSmile(shiverX, shiverY);
        drawWaterDropIcon();
      } else if (pomoState == POMO_MEAL_ALERT) {
        drawHappyArcEyes(0, 0);
        if (mealLogged) drawTemplateSmile(0, 0);
        else drawChompMouth(now);
      } else if (pomoState == POMO_NOTES) {
        drawBoxEyes(gx, 2, curH, 0);            // looking down at the page
        drawNotesProgress(now);
      } else if (currentState == STATE_SLEEPING) {
        drawSleepingEyes(0, 0);
        drawSmallGentleSmile(0, 0);
        drawFloatingZ(now);
        if (isCold) drawSnowflakeIcon(104, 46);
      } else if (currentState == STATE_DROOPY) {
        drawBoxEyes(gx, gy, curH, curLid);
        drawSmallGentleSmile(shiverX, shiverY);
      } else {
        drawBoxEyes(gx, gy, curH, curLid);
        if (isCold) {
          drawShiveringMouth(shiverX, shiverY);
          drawSnowflakeIcon(112, 34);
        } else if (isHot) {
          drawCuteWarmSmile(shiverX, shiverY);
          drawSweatDrip(now);
        } else {
          drawTemplateSmile(shiverX, shiverY);
        }
      }
    }

    drawHint(now);
    display.display();
  }
}

// ============================================================
//  Drawing helpers
// ============================================================
void printCentered(const char* s, int y, int cx) {
  int w = strlen(s) * 6;
  display.setCursor(cx - w / 2, y);
  display.print(s);
}

void drawHint(unsigned long now) {
  if (hintType == HINT_NONE || (long)(now - hintUntil) >= 0) return;
  const char* msg = "";
  switch (hintType) {
    case HINT_HOLD_STOP: msg = "HOLD KNOB TO STOP"; break;
    case HINT_HOLD_SKIP: msg = "HOLD KNOB TO SKIP"; break;
    case HINT_ON_BREAK:  msg = "ON BREAK - EAT!";    break;
    default: return;
  }
  display.fillRect(0, 55, 128, 9, SSD1306_BLACK);
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  printCentered(msg, 56);
}

// Progress bar (cumulative writing time) + a pencil that wiggles while pen activity is detected.
void drawNotesProgress(unsigned long now) {
  float p = constrain((float)notesWriteMs / (float)NOTES_WRITE_TARGET_MS, 0.0f, 1.0f);
  display.drawRoundRect(14, 52, 100, 6, 2, SSD1306_WHITE);
  int w = (int)(p * 96.0f);
  if (w > 0) display.fillRect(16, 54, w, 2, SSD1306_WHITE);

  int wob = hxWriting ? (int)((now / 90) % 2) : 0;
  int px = 104, py = 44 + wob;
  display.drawLine(px, py, px + 8, py - 8, SSD1306_WHITE);
  display.drawLine(px + 1, py, px + 9, py - 8, SSD1306_WHITE);
  display.drawPixel(px - 1, py + 1, SSD1306_WHITE);
}

void drawEncoderMenu() {
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  printCentered("FOCUS SPRINT", 4);
  display.drawFastHLine(14, 15, 100, SSD1306_WHITE);

  const int xOffsets[3] = {14, 49, 84};
  for (int i = 0; i < 3; i++) {
    if (i == selectedPresetIdx) {
      display.fillRoundRect(xOffsets[i], 22, 30, 22, 4, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK);
    } else {
      display.drawRoundRect(xOffsets[i], 22, 30, 22, 4, SSD1306_WHITE);
      display.setTextColor(SSD1306_WHITE);
    }
    display.setCursor(xOffsets[i] + 6, 29);
    display.print(SPRINT_PRESETS[i]);
    display.print("m");
  }
  display.setTextColor(SSD1306_WHITE);
  printCentered("CLICK TO START", 52);
}

void drawTransition(unsigned long now) {
  unsigned long t = now - transitionStart;
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  if (t < 240) {
    int step = t / 60;                                   // 0..3
    for (int y = 0; y < 64; y += (4 - step)) display.drawFastHLine(0, y, 128, SSD1306_WHITE);
    display.fillRect(30, 25, 68, 13, SSD1306_BLACK);
    printCentered("GET READY", 28);
  } else {
    int pct = constrain((int)((t - 240) * 100 / 760), 0, 100);
    char buf[20];
    snprintf(buf, sizeof(buf), "PREPARING %dM", chosenSprintMins);
    printCentered(buf, 12);
    display.drawRoundRect(14, 28, 100, 12, 3, SSD1306_WHITE);
    int barW = map(pct, 0, 100, 0, 96);
    if (barW > 0) display.fillRoundRect(16, 30, barW, 8, 2, SSD1306_WHITE);
    snprintf(buf, sizeof(buf), "%d%%", pct);
    printCentered(buf, 46);
  }
}

// ---- Hourglass: logical coords, mirrored about y=28 when flipped ----
bool hgInv = false;
inline int hgY(int y) { return hgInv ? (56 - y) : y; }
void hgRect(int x, int y, int w, int h, uint16_t c) {
  if (h <= 0 || w <= 0) return;
  display.fillRect(x, hgInv ? (57 - y - h) : y, w, h, c);
}
void hgTri(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t c) {
  display.fillTriangle(x0, hgY(y0), x1, hgY(y1), x2, hgY(y2), c);
}

void drawHourglassBot(unsigned long now) {
  const int cx = 64;
  unsigned long elapsed = now - pomoTimerStart;
  float p = constrain((float)elapsed / (float)currentPomoDuration, 0.0f, 1.0f);
  hgInv = hourglassFlipped;

  // Sand: remaining in upper bulb, fallen in lower bulb
  float rem = 1.0f - p;
  int topH = (int)lroundf(rem * 10.0f);
  int botH = (int)lroundf(p * 10.0f);
  if (rem > 0.01f) {
    hgRect(cx - 19, 18 - topH, 39, topH, SSD1306_WHITE);
    if (topH > 0) hgTri(cx - 18, 18, cx + 18, 18, cx, 26, SSD1306_WHITE);
    else          hgTri(cx - 8, 22, cx + 8, 22, cx, 26, SSD1306_WHITE);
  }
  hgRect(cx - 19, 51 - botH, 39, botH, SSD1306_WHITE);
  if (p > 0.02f && p < 0.99f) hgTri(cx - 19, 50 - botH, cx + 19, 50 - botH, cx, 46 - botH, SSD1306_WHITE);

  // Falling stream
  if (p < 0.99f) {
    int phase = (now / 80) % 2;
    for (int y = 28 + phase; y < 46 - botH; y += 2) display.drawPixel(cx, hgY(y), SSD1306_WHITE);
  }

  // Glass outline (symmetric, same both ways)
  display.fillRoundRect(cx - 24, 2, 48, 4, 1, SSD1306_WHITE);
  display.fillRoundRect(cx - 24, 51, 48, 4, 1, SSD1306_WHITE);
  for (int s = -1; s <= 1; s += 2) {
    display.drawLine(cx + s * 21, 6,  cx + s * 21, 17, SSD1306_WHITE);
    display.drawLine(cx + s * 21, 17, cx + s * 3,  27, SSD1306_WHITE);
    display.drawLine(cx + s * 3,  27, cx + s * 3,  29, SSD1306_WHITE);
    display.drawLine(cx + s * 3,  29, cx + s * 21, 39, SSD1306_WHITE);
    display.drawLine(cx + s * 21, 39, cx + s * 21, 50, SSD1306_WHITE);
  }

  // Face (black halo keeps it visible over sand)
  int ey = hgY(11);
  bool blink = (now % 4300) < 130;
  for (int s = -1; s <= 1; s += 2) {
    int ex = cx + s * 9;
    display.fillCircle(ex, ey, 5, SSD1306_BLACK);
    if (blink) {
      display.drawFastHLine(ex - 3, ey, 7, SSD1306_WHITE);
    } else {
      display.drawCircle(ex, ey, 3, SSD1306_WHITE);
      display.fillCircle(ex, ey, 1, SSD1306_WHITE);
    }
  }
  hgRect(cx - 4, 14, 9, 3, SSD1306_BLACK);
  if (hgInv) {
    display.drawCircle(cx, hgY(15), 1, SSD1306_WHITE);               // surprised "o"
  } else {
    display.drawPixel(cx - 3, hgY(14), SSD1306_WHITE);
    display.drawFastHLine(cx - 2, hgY(15), 5, SSD1306_WHITE);
    display.drawPixel(cx + 3, hgY(14), SSD1306_WHITE);
  }

  // Sparkles
  display.drawPixel(cx - 30, 20, SSD1306_WHITE);
  display.drawPixel(cx + 32, 16, SSD1306_WHITE);
  display.drawPixel(cx + 30, 40, SSD1306_WHITE);
  display.drawPixel(cx - 28, 44, SSD1306_WHITE);

  // Text
  char buf[12];
  unsigned long remS = (currentPomoDuration > elapsed) ? (currentPomoDuration - elapsed + 999) / 1000 : 0;
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  snprintf(buf, sizeof(buf), "%02lu:%02lu", remS / 60, remS % 60);
  display.setCursor(2, 24); display.print(buf);
  snprintf(buf, sizeof(buf), "%dm", chosenSprintMins);
  display.setCursor(2, 35); display.print(buf);
  snprintf(buf, sizeof(buf), "%d%%", (int)(p * 100));
  display.setCursor(126 - strlen(buf) * 6, 24); display.print(buf);

  // Progress bar
  display.drawRoundRect(14, 58, 100, 5, 2, SSD1306_WHITE);
  int w = (int)(p * 96.0f);
  if (w > 0) display.fillRect(16, 59, w, 3, SSD1306_WHITE);
}

// ============================================================
//  Lifecycle / blink / gaze
// ============================================================
void updateBotLifecycle(unsigned long now) {
  if (currentState == STATE_DIZZY) {
    if ((long)(now - dizzyEndTime) >= 0) {
      currentState = STATE_ACTIVE;
      targetEyeHeight = maxEyeHeight;
      targetTopLidCut = 0.0;
      stateTimer = now + random(120000, 180000);
    }
    return;
  }

  // Stay awake during sprints, notes, alerts and menu
  if (pomoState != POMO_IDLE || menuActive) {
    currentState = STATE_ACTIVE;
    targetTopLidCut = 0.0;
    targetEyeHeight = maxEyeHeight;
    stateTimer = now + 120000;
    return;
  }

  if ((long)(now - stateTimer) >= 0) {
    switch (currentState) {
      case STATE_ACTIVE:
        currentState = STATE_DROOPY;
        targetTopLidCut = 9.0;
        stateTimer = now + random(30000, 45000);
        break;
      case STATE_DROOPY:
        currentState = STATE_SLEEPING;
        targetEyeHeight = 3.0;
        stateTimer = now + random(60000, 120000);
        break;
      case STATE_SLEEPING:
        currentState = STATE_WAKING;
        targetEyeHeight = maxEyeHeight;
        targetTopLidCut = 0.0;
        stateTimer = now + 2200;
        break;
      case STATE_WAKING:
        currentState = STATE_ACTIVE;
        stateTimer = now + random(120000, 180000);
        break;
      default: break;
    }
  }
}

void handleBlinkLogic(unsigned long now) {
  if (currentState == STATE_SLEEPING || currentState == STATE_DIZZY || isPetting) {
    blinkState = BLINK_IDLE;
    return;
  }
  if (blinkState == BLINK_IDLE && (long)(now - nextBlinkTime) >= 0) blinkState = BLINK_CLOSING;
  if (blinkState == BLINK_CLOSING) {
    currentEyeHeight -= 7.5;
    if (currentEyeHeight <= 2.5) { currentEyeHeight = 2.5; blinkState = BLINK_OPENING; }
  } else if (blinkState == BLINK_OPENING) {
    currentEyeHeight += 6.0;
    if (currentEyeHeight >= targetEyeHeight) { currentEyeHeight = targetEyeHeight; blinkState = BLINK_IDLE; scheduleNextBlink(); }
  } else {
    currentEyeHeight += (targetEyeHeight - currentEyeHeight) * 0.25;
  }
}

void scheduleNextGaze() {
  static const int8_t G[5][2] = {{0,0},{-5,0},{5,0},{-4,-3},{4,-3}};
  int c = random(0, 5);
  targetGazeX = G[c][0];
  targetGazeY = G[c][1];
  nextGazeShift = millis() + random(2200, 4200);
}

void scheduleNextBlink() { nextBlinkTime = millis() + random(3000, 7000); }

// ============================================================
//  Header (1px burn-in shift every 2 min)
// ============================================================
void drawTopHeader(unsigned long now) {
  int sh = (now / 120000UL) % 2;
  char buf[16];
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  if (dhtValid) snprintf(buf, sizeof(buf), "%dC", (int)lroundf(currentTemp));
  else strcpy(buf, "--C");
  display.setCursor(1 + sh, 1);
  display.print(buf);

  if (dhtValid) snprintf(buf, sizeof(buf), "%d%%", (int)lroundf(currentHumidity));
  else strcpy(buf, "--%");
  display.setCursor(121 + sh - strlen(buf) * 6, 1);
  display.print(buf);

  if (wifiEnabled) {
    display.drawPixel(125, 2, SSD1306_WHITE);
    display.drawPixel(125, 4, SSD1306_WHITE);
    display.drawPixel(125, 6, SSD1306_WHITE);
  }

  if (pomoState == POMO_NOTES) {
    strcpy(buf, "WRITE NOTES");
  } else if (pomoState == POMO_WATER_ALERT) {
    strcpy(buf, "!DRINK!");
  } else if (pomoState == POMO_MEAL_ALERT) {
    unsigned long el = now - pomoTimerStart;
    unsigned long rem = (POMO_MEAL_DURATION > el) ? (POMO_MEAL_DURATION - el) / 1000 : 0;
    snprintf(buf, sizeof(buf), mealLogged ? "BRK %02lu:%02lu" : "EAT %02lu:%02lu", rem / 60, rem % 60);
  } else {
    int cycle = (now / 3500) % 3;
    if (cycle == 0)      snprintf(buf, sizeof(buf), "%s", DAYS_OF_WEEK[currentTm.tm_wday]);
    else if (cycle == 1) snprintf(buf, sizeof(buf), "%02d:%02d%s", currentTm.tm_hour, currentTm.tm_min, timeSynced ? "" : "?");
    else                 snprintf(buf, sizeof(buf), "S:%02dd", (int)currentStreak);
  }
  printCentered(buf, 1, 64 + sh);
}

// ============================================================
//  Faces
// ============================================================
void drawBoxEyes(int ox, int oy, int h, int topLidCut) {
  int r = min(eyeCornerRadius, h / 2);
  int lx = leftEyeCenterX - (eyeWidth / 2) + ox;
  int ly = eyeCenterY - (h / 2) + oy;
  int rx = rightEyeCenterX - (eyeWidth / 2) + ox;
  int ry = ly;
  display.fillRoundRect(lx, ly, eyeWidth, h, r, SSD1306_WHITE);
  display.fillRoundRect(rx, ry, eyeWidth, h, r, SSD1306_WHITE);
  if (topLidCut > 0 && h > 8) {
    display.fillRect(lx - 1, ly - 1, eyeWidth + 2, topLidCut, SSD1306_BLACK);
    display.fillRect(rx - 1, ry - 1, eyeWidth + 2, topLidCut, SSD1306_BLACK);
    display.drawLine(lx, ly + topLidCut - 1, lx + eyeWidth - 1, ly + topLidCut + 2, SSD1306_WHITE);
    display.drawLine(rx, ry + topLidCut + 2, rx + eyeWidth - 1, ry + topLidCut - 1, SSD1306_WHITE);
  }
}

void drawHappyArcEyes(int ox, int oy) {          // ^ ^
  for (int t = 0; t <= 2; t++) {
    for (int cx : {leftEyeCenterX, rightEyeCenterX}) {
      display.drawLine(cx - 11 + ox, eyeCenterY + 3 + t + oy, cx + ox, eyeCenterY - 4 + t + oy, SSD1306_WHITE);
      display.drawLine(cx + ox, eyeCenterY - 4 + t + oy, cx + 11 + ox, eyeCenterY + 3 + t + oy, SSD1306_WHITE);
    }
  }
}

void drawSleepingEyes(int ox, int oy) {          // closed, curved down (distinct from happy)
  for (int t = 0; t <= 1; t++) {
    for (int cx : {leftEyeCenterX, rightEyeCenterX}) {
      display.drawLine(cx - 11 + ox, eyeCenterY - 1 + t + oy, cx + ox, eyeCenterY + 3 + t + oy, SSD1306_WHITE);
      display.drawLine(cx + ox, eyeCenterY + 3 + t + oy, cx + 11 + ox, eyeCenterY - 1 + t + oy, SSD1306_WHITE);
    }
  }
}

void drawDizzyEyes() {
  for (int t = -1; t <= 1; t++) {
    for (int cx : {leftEyeCenterX, rightEyeCenterX}) {
      display.drawLine(cx - 9 + t, eyeCenterY - 9, cx + 9 + t, eyeCenterY + 9, SSD1306_WHITE);
      display.drawLine(cx - 9 + t, eyeCenterY + 9, cx + 9 + t, eyeCenterY - 9, SSD1306_WHITE);
    }
  }
}

void drawTemplateSmile(int ox, int oy) {
  for (int t = 0; t <= 1; t++) {
    display.drawLine(56 + ox, 52 + t + oy, 59 + ox, 55 + t + oy, SSD1306_WHITE);
    display.drawLine(59 + ox, 55 + t + oy, 69 + ox, 55 + t + oy, SSD1306_WHITE);
    display.drawLine(69 + ox, 55 + t + oy, 72 + ox, 52 + t + oy, SSD1306_WHITE);
  }
}

void drawSmallGentleSmile(int ox, int oy) {
  display.drawLine(60 + ox, 54 + oy, 68 + ox, 54 + oy, SSD1306_WHITE);
  display.drawLine(60 + ox, 55 + oy, 68 + ox, 55 + oy, SSD1306_WHITE);
}

void drawCuteWarmSmile(int ox, int oy) {
  drawTemplateSmile(ox, oy);
  display.drawLine(28 + ox, 49 + oy, 33 + ox, 49 + oy, SSD1306_WHITE);   // blush
  display.drawLine(95 + ox, 49 + oy, 100 + ox, 49 + oy, SSD1306_WHITE);
}

void drawShiveringMouth(int ox, int oy) {
  for (int t = 0; t <= 1; t++) {
    for (int i = 0; i < 5; i++) {
      int x0 = 57 + i * 3;
      int y0 = (i % 2 == 0) ? 54 : 51;
      int y1 = (i % 2 == 0) ? 51 : 54;
      display.drawLine(x0 + ox, y0 + t + oy, x0 + 3 + ox, y1 + t + oy, SSD1306_WHITE);
    }
  }
}

void drawChompMouth(unsigned long now) {
  int r = ((now / 300) % 2) ? 5 : 2;
  display.fillCircle(64, 54, r, SSD1306_WHITE);
}

void drawDizzyMouth() {
  display.drawLine(56, 54, 60, 51, SSD1306_WHITE);
  display.drawLine(60, 51, 64, 56, SSD1306_WHITE);
  display.drawLine(64, 56, 68, 52, SSD1306_WHITE);
  display.drawLine(68, 52, 72, 55, SSD1306_WHITE);
}

void drawSweatDrip(unsigned long now) {
  int x = 104;
  int y = 20 + (now / 140) % 16;
  display.drawPixel(x, y - 2, SSD1306_WHITE);
  display.drawLine(x - 1, y - 1, x + 1, y - 1, SSD1306_WHITE);
  display.fillCircle(x, y + 1, 2, SSD1306_WHITE);
}

void drawSnowflakeIcon(int x, int y) {
  display.drawLine(x, y - 3, x, y + 3, SSD1306_WHITE);
  display.drawLine(x - 3, y, x + 3, y, SSD1306_WHITE);
  display.drawLine(x - 2, y - 2, x + 2, y + 2, SSD1306_WHITE);
  display.drawLine(x - 2, y + 2, x + 2, y - 2, SSD1306_WHITE);
}

void drawWaterDropIcon() {
  display.fillCircle(114, 52, 4, SSD1306_WHITE);
  display.fillTriangle(114, 45, 110, 51, 118, 51, SSD1306_WHITE);
}

void drawFloatingZ(unsigned long now) {
  int phase = (now / 700) % 3;
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(104, 25); display.print("z");
  if (phase >= 1) { display.setCursor(111, 18); display.print("z"); }
  if (phase >= 2) { display.setCursor(117, 11); display.print("Z"); }
}

// ============================================================
//  Boot sequence (blocking is fine here)
// ============================================================
void bootLine(int y, const char* s, int wait) {
  display.setCursor(2, y);
  display.print(s);
  display.display();
  delay(wait);
}

void runAnimatedBootSequence() {
  char buf[24];
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  printCentered("DESK BOT v4.1", 2);
  display.display();
  delay(180);

  snprintf(buf, sizeof(buf), "OLED    0x%02X   OK", oledAddr);
  bootLine(11, buf, 100);
  bootLine(20, mpuOnline ? "MPU6050 0x68   OK" : "MPU6050 0x68   --", 100);
  bootLine(29, "ROTARY  32/33/27", 100);
  bootLine(38, "DHT11   GP19", 100);
  bootLine(47, "HX711   HANDS OFF", 50);
  bootLine(56, "TOUCH   HANDS OFF", 50);

  calibrateTouch();
  hxCalibrate();
  display.fillRect(0, 47, 128, 17, SSD1306_BLACK);
  bootLine(47, hxOnline ? "HX711   GP16   OK" : "HX711   NOT FOUND", 200);
  bootLine(56, "TOUCH   GP04   OK", 250);

  for (int p = 0; p <= 100; p += 5) {
    display.clearDisplay();
    printCentered("SYSTEM WAKING UP", 16);
    display.drawRoundRect(14, 32, 100, 10, 3, SSD1306_WHITE);
    int barWidth = map(p, 0, 100, 0, 96);
    if (barWidth > 0) display.fillRoundRect(16, 34, barWidth, 6, 2, SSD1306_WHITE);
    snprintf(buf, sizeof(buf), "%d%%", p);
    printCentered(buf, 46);
    display.display();
    delay(16);
  }
  delay(150);

  for (int h = 2; h <= maxEyeHeight; h += 3) {
    display.clearDisplay();
    drawBoxEyes(0, 0, h, 0);
    drawTemplateSmile(0, 0);
    display.display();
    delay(20);
  }
  delay(120);
  display.clearDisplay();
  drawBoxEyes(0, 0, 2, 0);
  drawTemplateSmile(0, 0);
  display.display();
  delay(80);
  display.clearDisplay();
  drawBoxEyes(0, 0, maxEyeHeight, 0);
  drawTemplateSmile(0, 0);
  display.display();

  soundBootChime();
  sndFlushBlocking();
  delay(150);
}
