#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ArduinoOTA.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <Update.h>
#include <PubSubClient.h>
#include <Adafruit_NeoPixel.h>

// ── Pins ─────────────────────────────────────────────────────────────────────
// INPUT_PULLUP: HIGH = door open (reed opens), LOW = door closed (reed shorts to GND)
constexpr uint8_t PIN_REED = 3;   // reed contact — adjust if wired differently
constexpr uint8_t PIN_FSR  = 4;   // FSR pressure sensor (ADC)
constexpr uint8_t PIN_LED  = 8;   // WS2812 built-in LED (C3 SuperMini)

// ── Fixed device identity ─────────────────────────────────────────────────────
const char DEVICE_ID[] = "esp32-c3-supermini-door";
const char SHELLY_IP[] = "192.168.0.118";

// ── Static IP ────────────────────────────────────────────────────────────────
static const IPAddress STATIC_IP (192, 168, 0,  54);
static const IPAddress GATEWAY   (192, 168, 0,   1);
static const IPAddress SUBNET    (255, 255, 255, 0);
static const IPAddress DNS1      (  8,   8, 8,   8);

// ── Timings ──────────────────────────────────────────────────────────────────
constexpr uint32_t WIFI_CONNECT_TIMEOUT = 15000;
constexpr uint32_t WIFI_RETRY_DELAY     = 10000;
constexpr uint32_t MQTT_RETRY_DELAY     = 5000;
constexpr uint32_t PUBLISH_INTERVAL     = 30000;
constexpr uint32_t FSR_READ_INTERVAL    = 100;
constexpr uint8_t  FSR_AVG_WINDOW       = 10;
constexpr uint32_t DOOR_DEBOUNCE_MS     = 50;

// ── Persistent configuration (stored in NVS via Preferences) ─────────────────
struct Config {
    String wifiSsid;
    String wifiPass;
    String mqttHost = "192.168.0.40";
    uint16_t mqttPort = 1883;
    String mqttUser = "pi";
    String mqttPass = "FeelTheAlps2025!";
} cfg;

void loadConfig() {
    Preferences p;
    p.begin("wifi", /*readOnly=*/true);
    cfg.wifiSsid = p.getString("ssid", "");
    cfg.wifiPass = p.getString("pass", "");
    p.end();
    p.begin("mqtt", true);
    cfg.mqttHost = p.getString("host", cfg.mqttHost.c_str());
    cfg.mqttPort = p.getUShort("port", cfg.mqttPort);
    cfg.mqttUser = p.getString("user", cfg.mqttUser.c_str());
    cfg.mqttPass = p.getString("pass", cfg.mqttPass.c_str());
    p.end();
}

void saveWifi(const String& ssid, const String& pass) {
    Preferences p;
    p.begin("wifi", false);
    p.putString("ssid", ssid);
    p.putString("pass", pass);
    p.end();
    cfg.wifiSsid = ssid;
    cfg.wifiPass = pass;
}

void saveMqtt(const String& host, uint16_t port,
              const String& user, const String& pass) {
    Preferences p;
    p.begin("mqtt", false);
    p.putString("host", host);
    p.putUShort("port", port);
    p.putString("user", user);
    p.putString("pass", pass);
    p.end();
    cfg.mqttHost = host;
    cfg.mqttPort = port;
    cfg.mqttUser = user;
    cfg.mqttPass = pass;
}

// ── Objects ───────────────────────────────────────────────────────────────────
Adafruit_NeoPixel led(1, PIN_LED, NEO_GRB + NEO_KHZ800);
WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);
WebServer    server(80);
DNSServer    dns;

// ── State ─────────────────────────────────────────────────────────────────────
enum class NetMode { IDLE, CONNECTING, CONNECTED, AP };
enum FsrLevel { FSR_LOW, FSR_MEDIUM, FSR_HIGH };

struct {
    NetMode  net           = NetMode::IDLE;
    bool     mqttConnected = false;
    bool     otaReady      = false;
    uint32_t lastWifiTry   = 0;
    uint32_t lastMqttTry   = 0;

    bool     doorOpen      = false;
    bool     rawDoor       = false;
    uint32_t debounceAt    = 0;
    bool     debouncing    = false;

    float    fsrV          = 0.0f;
    uint32_t lastFsrRead   = 0;
    uint32_t fsrSamples[FSR_AVG_WINDOW] = {};
    uint8_t  fsrIdx        = 0;
    FsrLevel fsrLevel      = FSR_LOW;

    uint32_t ledOffAt      = 0;
    uint32_t lastPublish   = 0;
} S;

// ── LED ───────────────────────────────────────────────────────────────────────
void ledSet(uint8_t r, uint8_t g, uint8_t b, uint32_t dur = 0) {
    led.setPixelColor(0, led.Color(r, g, b));
    led.show();
    S.ledOffAt = dur ? millis() + dur : 0;
}
void ledOff() { led.setPixelColor(0, 0); led.show(); S.ledOffAt = 0; }
void tickLed() { if (S.ledOffAt && millis() >= S.ledOffAt) ledOff(); }

// ── MQTT topics ───────────────────────────────────────────────────────────────
static String tSensor(const char* n) { return String(DEVICE_ID) + "/sensor/" + n + "/state"; }
static String tBinary(const char* n) { return String(DEVICE_ID) + "/binary_sensor/" + n + "/state"; }

void publishAll() {
    if (!mqtt.connected()) return;
    char buf[32];
    mqtt.publish(tBinary("door_contact__reed_").c_str(), S.doorOpen ? "ON" : "OFF", true);
    snprintf(buf, sizeof(buf), "%.3f", S.fsrV);
    mqtt.publish(tSensor("fsr_pressure_sensor").c_str(), buf);
    snprintf(buf, sizeof(buf), "%d", WiFi.RSSI());
    mqtt.publish(tSensor("wifi_signal_strength").c_str(), buf);
    int q = WiFi.RSSI() <= -100 ? 0 : WiFi.RSSI() >= -50 ? 100 : 2*(WiFi.RSSI()+100);
    snprintf(buf, sizeof(buf), "%d", q);
    mqtt.publish(tSensor("wifi_signal_quality").c_str(), buf);
    mqtt.publish(tSensor("device_ip_address").c_str(), WiFi.localIP().toString().c_str());
    snprintf(buf, sizeof(buf), "%lu", millis() / 60000UL);
    mqtt.publish(tSensor("device_uptime").c_str(), buf);
}

// ── WiFi / AP ─────────────────────────────────────────────────────────────────
void startAP() {
    WiFi.mode(WIFI_AP);
    WiFi.softAP("DoorSensor-Setup", "doorsensor");
    dns.start(53, "*", WiFi.softAPIP());
    S.net = NetMode::AP;
    Serial.printf("[WiFi] AP mode — connect to DoorSensor-Setup and open 192.168.4.1\n");
    ledSet(80, 50, 0);  // amber = AP setup mode
}

void beginConnect() {
    Serial.printf("[WiFi] Connecting to '%s'…\n", cfg.wifiSsid.c_str());
    WiFi.disconnect(false);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.config(STATIC_IP, GATEWAY, SUBNET, DNS1);
    WiFi.begin(cfg.wifiSsid.c_str(), cfg.wifiPass.c_str());
    S.net        = NetMode::CONNECTING;
    S.lastWifiTry = millis();
    ledSet(0, 0, 30);  // dim blue = connecting
}

void tickWifi() {
    uint32_t now = millis();

    if (S.net == NetMode::AP) {
        dns.processNextRequest();
        return;
    }

    bool up = (WiFi.status() == WL_CONNECTED);

    if (up && S.net != NetMode::CONNECTED) {
        S.net = NetMode::CONNECTED;
        Serial.printf("[WiFi] Connected — %s\n", WiFi.localIP().toString().c_str());
        ledSet(0, 80, 0, 2000);
        if (!S.otaReady) { ArduinoOTA.begin(); S.otaReady = true; }
        return;
    }

    if (!up && S.net == NetMode::CONNECTED) {
        S.net = NetMode::IDLE;
        S.mqttConnected = false;
        Serial.println("[WiFi] Lost connection.");
        ledSet(40, 0, 0);  // dim red = no WiFi
    }

    if (!up) {
        bool timedOut = (now - S.lastWifiTry >= WIFI_CONNECT_TIMEOUT);
        bool readyRetry = (S.net == NetMode::IDLE && now - S.lastWifiTry >= WIFI_RETRY_DELAY);
        if (timedOut || readyRetry) beginConnect();
    }
}

// ── MQTT ──────────────────────────────────────────────────────────────────────
void mqttConnect() {
    mqtt.setServer(cfg.mqttHost.c_str(), cfg.mqttPort);
    String lwt = String(DEVICE_ID) + "/online";
    Serial.printf("[MQTT] Connecting to %s:%d…\n", cfg.mqttHost.c_str(), cfg.mqttPort);
    if (mqtt.connect(DEVICE_ID, cfg.mqttUser.c_str(), cfg.mqttPass.c_str(),
                     lwt.c_str(), 1, true, "false")) {
        mqtt.publish(lwt.c_str(), "true", true);
        S.mqttConnected = true;
        Serial.println("[MQTT] Connected.");
        ledSet(0, 40, 40, 800);  // cyan flash = MQTT up
        publishAll();
    } else {
        Serial.printf("[MQTT] Failed rc=%d\n", mqtt.state());
    }
    S.lastMqttTry = millis();
}

void tickMqtt() {
    if (S.net != NetMode::CONNECTED) return;
    if (mqtt.connected()) { mqtt.loop(); S.mqttConnected = true; return; }
    if (S.mqttConnected) { S.mqttConnected = false; Serial.println("[MQTT] Disconnected."); }
    if (millis() - S.lastMqttTry >= MQTT_RETRY_DELAY) mqttConnect();
}

// ── Shelly ────────────────────────────────────────────────────────────────────
void shellySet(bool on) {
    if (S.net != NetMode::CONNECTED) return;
    HTTPClient http;
    String url = String("http://") + SHELLY_IP + "/relay/0?turn=" + (on ? "on" : "off");
    http.begin(url);
    http.setTimeout(3000);
    int rc = http.GET();
    http.end();
    Serial.printf("[Shelly] %s → HTTP %d\n", on ? "ON" : "OFF", rc);
}

// ── Door reed ─────────────────────────────────────────────────────────────────
void tickDoor() {
    bool raw = (digitalRead(PIN_REED) == HIGH);
    uint32_t now = millis();
    if (raw != S.rawDoor) { S.rawDoor = raw; S.debounceAt = now; S.debouncing = true; }
    if (S.debouncing && now - S.debounceAt >= DOOR_DEBOUNCE_MS) {
        S.debouncing = false;
        if (S.rawDoor != S.doorOpen) {
            S.doorOpen = S.rawDoor;
            Serial.printf("[Door] %s\n", S.doorOpen ? "OPEN" : "CLOSED");
            if (mqtt.connected())
                mqtt.publish(tBinary("door_contact__reed_").c_str(),
                             S.doorOpen ? "ON" : "OFF", true);
            ledSet(S.doorOpen ? 80 : 0, S.doorOpen ? 0 : 80, 0, 500);
        }
    }
}

// ── FSR ───────────────────────────────────────────────────────────────────────
void tickFsr() {
    if (millis() - S.lastFsrRead < FSR_READ_INTERVAL) return;
    S.lastFsrRead = millis();
    S.fsrSamples[S.fsrIdx++ % FSR_AVG_WINDOW] = analogRead(PIN_FSR);
    uint32_t sum = 0;
    for (uint8_t i = 0; i < FSR_AVG_WINDOW; i++) sum += S.fsrSamples[i];
    S.fsrV = (sum / (float)FSR_AVG_WINDOW / 4095.0f) * 3.3f;

    FsrLevel lvl = S.fsrV > 1.0f ? FSR_HIGH : S.fsrV > 0.5f ? FSR_MEDIUM : FSR_LOW;
    if (lvl == S.fsrLevel) return;
    if (lvl == FSR_HIGH) {
        Serial.printf("[FSR] ALARM %.3fV\n", S.fsrV);
        if (mqtt.connected()) mqtt.publish("door/alarm", "Lamp on");
        shellySet(true);
        ledSet(80, 0, 0, 20000);
    } else if (lvl == FSR_MEDIUM && S.fsrLevel == FSR_LOW) {
        Serial.printf("[FSR] MEDIUM %.3fV\n", S.fsrV);
        if (mqtt.connected()) mqtt.publish("door/alarm", "Lamp off");
        shellySet(false);
        ledSet(0, 80, 0, 3000);
    }
    S.fsrLevel = lvl;
}

// ── Shared CSS ────────────────────────────────────────────────────────────────
static const char CSS[] PROGMEM = R"(
<style>
:root{--bg:#0d1117;--card:#161b22;--bo:#30363d;--tx:#e6edf3;--mu:#8b949e;
      --gr:#3fb950;--re:#f85149;--bl:#58a6ff;--ye:#e3b341}
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;
     background:var(--bg);color:var(--tx);min-height:100vh;padding:1.5rem;font-size:14px}
h1{font-size:1.1rem;margin-bottom:.8rem}
nav{display:flex;gap:.5rem;flex-wrap:wrap;margin-bottom:1.1rem}
nav a{background:#21262d;border:1px solid var(--bo);color:var(--mu);
      padding:.3rem .75rem;border-radius:6px;text-decoration:none;font-size:.78rem}
nav a:hover,nav a.active{color:var(--tx);border-color:var(--bl)}
.card{background:var(--card);border:1px solid var(--bo);border-radius:10px;
      padding:1rem 1.2rem;margin-bottom:1rem}
.card h2{font-size:.75rem;color:var(--mu);text-transform:uppercase;
         letter-spacing:.05em;margin-bottom:.7rem}
.row{display:flex;justify-content:space-between;align-items:center;
     padding:.32rem 0;border-bottom:1px solid #21262d}
.row:last-child{border-bottom:none}
.lbl{color:var(--mu);font-size:.82rem}.val{font-weight:600;font-size:.82rem}
.open{color:var(--re)}.closed{color:var(--gr)}.ok{color:var(--gr)}.err{color:var(--re)}
label{display:block;font-size:.78rem;color:var(--mu);margin:.45rem 0 .15rem}
input{width:100%;background:#21262d;border:1px solid var(--bo);color:var(--tx);
      padding:.38rem .6rem;border-radius:6px;font-size:.82rem}
input:focus{outline:none;border-color:var(--bl)}
.btn{display:inline-block;background:#21262d;border:1px solid var(--bo);color:var(--tx);
     padding:.4rem 1rem;border-radius:6px;cursor:pointer;font-size:.82rem;
     text-decoration:none;margin-top:.5rem}
.btn:hover{background:#30363d}
.btn-p{background:var(--bl);border-color:var(--bl);color:#000;font-weight:600}
.btn-p:hover{opacity:.85}
.msg{padding:.5rem .75rem;border-radius:6px;margin-bottom:.8rem;font-size:.82rem}
.msg-ok{background:rgba(63,185,80,.12);color:var(--gr)}
.msg-warn{background:rgba(227,179,65,.12);color:var(--ye)}
progress{width:100%;height:6px;border-radius:3px;accent-color:var(--bl);margin-top:.4rem}
code{font-family:monospace;font-size:.85em;color:var(--bl)}
</style>)";

static String pageHead(const char* title, const char* active) {
    String h = "<!DOCTYPE html><html lang='en'><head>"
               "<meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
               "<title>";
    h += title;
    h += " — Door Sensor</title>";
    h += FPSTR(CSS);
    h += "</head><body>";
    h += "<h1>🚪 Door Sensor</h1><nav>";
    h += String("<a href='/' ") + (strcmp(active,"/")     == 0 ? "class='active'" : "") + ">Status</a>";
    h += String("<a href='/config' ") + (strcmp(active,"/config") == 0 ? "class='active'" : "") + ">Config</a>";
    h += String("<a href='/update' ") + (strcmp(active,"/update") == 0 ? "class='active'" : "") + ">OTA Update</a>";
    h += "</nav>";
    return h;
}

// ── GET / ─────────────────────────────────────────────────────────────────────
void handleRoot() {
    if (S.net == NetMode::AP) {
        server.sendHeader("Location", "/config");
        server.send(302);
        return;
    }
    String h = pageHead("Status", "/");
    h += "<div class='card'><h2>🚪 Türsensor</h2>"
         "<div class='row'><span class='lbl'>Reed contact</span>";
    h += S.doorOpen ? "<span class='val open'>🔓 OPEN</span>"
                    : "<span class='val closed'>🔒 CLOSED</span>";
    h += "</div><div class='row'><span class='lbl'>FSR voltage</span>"
         "<span class='val'>" + String(S.fsrV, 3) + " V</span></div></div>";

    h += "<div class='card'><h2>📶 Network</h2>"
         "<div class='row'><span class='lbl'>IP address</span><span class='val'>"
         + WiFi.localIP().toString() + "</span></div>"
         "<div class='row'><span class='lbl'>SSID</span><span class='val'>"
         + WiFi.SSID() + "</span></div>"
         "<div class='row'><span class='lbl'>RSSI</span><span class='val'>"
         + String(WiFi.RSSI()) + " dBm</span></div>"
         "<div class='row'><span class='lbl'>MQTT broker</span><span class='val'>"
         + cfg.mqttHost + ":" + String(cfg.mqttPort) + "</span></div>"
         "<div class='row'><span class='lbl'>MQTT</span><span class='val "
         + String(mqtt.connected() ? "ok'>Connected" : "err'>Disconnected")
         + "</span></div></div>";

    h += "<div class='card'><h2>⚙️ System</h2>"
         "<div class='row'><span class='lbl'>Uptime</span><span class='val'>"
         + String(millis() / 60000UL) + " min</span></div>"
         "<div class='row'><span class='lbl'>Free heap</span><span class='val'>"
         + String(ESP.getFreeHeap() / 1024) + " kB</span></div>"
         "<div class='row'><span class='lbl'>Device ID</span><span class='val'>"
         + String(DEVICE_ID) + "</span></div>"
         "<form action='/restart' method='POST'>"
         "<button class='btn' type='submit'>🔄 Restart</button></form></div>";

    h += "</body></html>";
    server.send(200, "text/html", h);
}

// ── GET /data ─────────────────────────────────────────────────────────────────
void handleData() {
    int32_t rssi = WiFi.RSSI();
    int q = rssi <= -100 ? 0 : rssi >= -50 ? 100 : 2*(rssi+100);
    String j = "{";
    j += "\"door_open\":"    + String(S.doorOpen ? "true" : "false") + ",";
    j += "\"fsr_voltage\":"  + String(S.fsrV, 3) + ",";
    j += "\"wifi_rssi\":"    + String(rssi) + ",";
    j += "\"wifi_quality\":" + String(q) + ",";
    j += "\"ip\":\""         + WiFi.localIP().toString() + "\",";
    j += "\"mqtt\":"         + String(mqtt.connected() ? "true" : "false") + ",";
    j += "\"uptime_s\":"     + String(millis() / 1000UL) + ",";
    j += "\"free_heap\":"    + String(ESP.getFreeHeap());
    j += "}";
    server.sendHeader("Cache-Control", "no-cache");
    server.send(200, "application/json", j);
}

// ── GET /config ───────────────────────────────────────────────────────────────
void handleConfigGet() {
    String h = pageHead("Config", "/config");
    bool inAP = (S.net == NetMode::AP);
    if (inAP) {
        h += "<div class='msg msg-warn'>⚠️ No WiFi — device is in setup AP mode.<br>"
             "Connect your phone/PC to <strong>DoorSensor-Setup</strong> (password: doorsensor), "
             "then fill in your WiFi credentials below.</div>";
    }
    if (server.hasArg("saved"))
        h += "<div class='msg msg-ok'>✓ Saved — device is restarting…</div>";

    h += "<form method='POST' action='/config'>"
         "<div class='card'><h2>📶 WiFi</h2>"
         "<label>SSID</label>"
         "<input name='w_ssid' value='" + cfg.wifiSsid + "' placeholder='Network name' required>"
         "<label>Password</label>"
         "<input name='w_pass' type='password' placeholder='Leave blank to keep current'>"
         "</div>"
         "<div class='card'><h2>🔗 MQTT Broker</h2>"
         "<label>Host / IP</label>"
         "<input name='m_host' value='" + cfg.mqttHost + "' placeholder='192.168.0.40'>"
         "<label>Port</label>"
         "<input name='m_port' type='number' value='" + String(cfg.mqttPort) + "'>"
         "<label>Username</label>"
         "<input name='m_user' value='" + cfg.mqttUser + "'>"
         "<label>Password</label>"
         "<input name='m_pass' type='password' placeholder='Leave blank to keep current'>"
         "</div>"
         "<button class='btn btn-p' type='submit'>💾 Save &amp; Restart</button>"
         "</form></body></html>";

    server.send(200, "text/html", h);
}

// ── POST /config ──────────────────────────────────────────────────────────────
void handleConfigPost() {
    String wSsid = server.arg("w_ssid");
    String wPass = server.arg("w_pass");
    if (!wSsid.isEmpty()) {
        if (wPass.isEmpty()) wPass = cfg.wifiPass;
        saveWifi(wSsid, wPass);
    }
    String mHost = server.arg("m_host");
    String mPort = server.arg("m_port");
    String mUser = server.arg("m_user");
    String mPass = server.arg("m_pass");
    if (!mHost.isEmpty()) {
        if (mPass.isEmpty()) mPass = cfg.mqttPass;
        if (mUser.isEmpty()) mUser = cfg.mqttUser;
        uint16_t port = mPort.toInt() > 0 ? (uint16_t)mPort.toInt() : (uint16_t)1883;
        saveMqtt(mHost, port, mUser, mPass);
    }
    server.sendHeader("Location", "/config?saved=1");
    server.send(302);
    delay(400);
    ESP.restart();
}

// ── GET /update ───────────────────────────────────────────────────────────────
void handleUpdateGet() {
    String h = pageHead("OTA Update", "/update");
    h += "<div class='card'><h2>⬆️ Web firmware upload</h2>"
         "<p style='font-size:.82rem;color:var(--mu);margin-bottom:.8rem'>"
         "Build in PlatformIO (<strong>Build</strong> or <code>pio run</code>), "
         "then select the <code>.pio/build/doorsensor/firmware.bin</code> file below.</p>"
         "<form id='uf' method='POST' action='/update' enctype='multipart/form-data'>"
         "<input type='file' name='firmware' accept='.bin' required>"
         "<progress id='pr' value='0' max='100' style='display:none'></progress>"
         "<div id='st' style='font-size:.82rem;margin-top:.4rem;min-height:1.2rem'></div>"
         "<button class='btn btn-p' type='submit'>⚡ Flash firmware</button>"
         "</form></div>"
         "<div class='card'><h2>📡 ArduinoOTA (IDE / CLI)</h2>"
         "<p style='font-size:.82rem;color:var(--mu);line-height:1.5'>"
         "Hostname: <code>door-sensor</code><br>"
         "Use PlatformIO OTA env or Arduino IDE → Tools → Port → door-sensor</p>"
         "</div>"
         R"(<script>
document.getElementById('uf').addEventListener('submit',function(e){
  e.preventDefault();
  var btn=this.querySelector('button'),pr=document.getElementById('pr'),st=document.getElementById('st');
  btn.disabled=true; pr.style.display='block'; st.textContent='Uploading…';
  var xhr=new XMLHttpRequest();
  xhr.open('POST','/update');
  xhr.upload.onprogress=function(ev){
    if(ev.lengthComputable){var p=Math.round(ev.loaded/ev.total*100);pr.value=p;st.textContent='Uploading… '+p+'%';}
  };
  xhr.onload=function(){
    if(xhr.status===200){st.innerHTML='<span class="ok">✓ Flash OK — rebooting in 5 s…</span>';}
    else{st.innerHTML='<span class="err">✗ Failed (HTTP '+xhr.status+')</span>';btn.disabled=false;}
  };
  xhr.onerror=function(){st.innerHTML='<span class="err">✗ Network error</span>';btn.disabled=false;};
  xhr.send(new FormData(this));
});
</script>)"
         "</body></html>";
    server.send(200, "text/html", h);
}

// ── POST /update (result) ─────────────────────────────────────────────────────
void handleUpdateResult() {
    bool ok = !Update.hasError();
    server.send(ok ? 200 : 500, "text/plain", ok ? "OK" : "FAIL");
    if (ok) { delay(500); ESP.restart(); }
}

// ── POST /update (upload chunks) ─────────────────────────────────────────────
void handleUpdateChunk() {
    HTTPUpload& up = server.upload();
    if (up.status == UPLOAD_FILE_START) {
        Serial.printf("[OTA-Web] Start: %s\n", up.filename.c_str());
        ledSet(0, 0, 80);  // blue = flashing
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (Update.write(up.buf, up.currentSize) != up.currentSize)
            Update.printError(Serial);
    } else if (up.status == UPLOAD_FILE_END) {
        if (Update.end(true))
            Serial.printf("[OTA-Web] Done: %u bytes\n", up.totalSize);
        else
            Update.printError(Serial);
    }
}

// ── POST /restart ─────────────────────────────────────────────────────────────
void handleRestart() {
    server.send(200, "text/html",
        "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
        "<meta http-equiv='refresh' content='8;url=/'></head>"
        "<body style='font-family:sans-serif;background:#0d1117;color:#e6edf3;padding:2rem'>"
        "Restarting…</body></html>");
    delay(200);
    ESP.restart();
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("\n=== Door Sensor starting ===");

    led.begin();
    led.setBrightness(80);
    ledSet(20, 20, 20, 800);  // brief white on boot

    loadConfig();
    Serial.printf("[Config] WiFi='%s'  MQTT=%s:%d user=%s\n",
                  cfg.wifiSsid.c_str(), cfg.mqttHost.c_str(),
                  cfg.mqttPort, cfg.mqttUser.c_str());

    pinMode(PIN_REED, INPUT_PULLUP);
    S.rawDoor = S.doorOpen = (digitalRead(PIN_REED) == HIGH);
    Serial.printf("[Door] Initial: %s\n", S.doorOpen ? "OPEN" : "CLOSED");

    analogReadResolution(12);
    analogSetPinAttenuation(PIN_FSR, ADC_11db);

    WiFi.setAutoReconnect(false);  // we manage reconnection manually
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);

    if (cfg.wifiSsid.isEmpty()) {
        Serial.println("[WiFi] No SSID saved — entering AP setup mode");
        startAP();
    } else {
        beginConnect();
    }

    // ArduinoOTA — begin() is called from tickWifi() after first connect
    ArduinoOTA.setHostname("door-sensor");
    ArduinoOTA.onStart([]() {
        Serial.println("[OTA] ArduinoOTA start");
        ledSet(0, 0, 80);
    });
    ArduinoOTA.onEnd([]()         { Serial.println("[OTA] Done"); ledOff(); });
    ArduinoOTA.onError([](ota_error_t e) {
        Serial.printf("[OTA] Error %u\n", e);
        ledSet(80, 0, 0, 3000);
    });

    mqtt.setServer(cfg.mqttHost.c_str(), cfg.mqttPort);
    mqtt.setKeepAlive(60);
    mqtt.setBufferSize(512);

    server.on("/",        HTTP_GET,  handleRoot);
    server.on("/data",    HTTP_GET,  handleData);
    server.on("/config",  HTTP_GET,  handleConfigGet);
    server.on("/config",  HTTP_POST, handleConfigPost);
    server.on("/update",  HTTP_GET,  handleUpdateGet);
    server.on("/update",  HTTP_POST, handleUpdateResult, handleUpdateChunk);
    server.on("/restart", HTTP_POST, handleRestart);
    // Captive portal: redirect all unknown paths to config page
    server.onNotFound([]() {
        server.sendHeader("Location", "http://192.168.4.1/config");
        server.send(302);
    });
    server.begin();
    Serial.println("[Web] Server on port 80");
    Serial.println("=== Setup done ===");
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
    tickLed();
    tickWifi();
    tickMqtt();
    tickDoor();
    tickFsr();
    server.handleClient();
    if (S.otaReady) ArduinoOTA.handle();

    if (millis() - S.lastPublish >= PUBLISH_INTERVAL) {
        S.lastPublish = millis();
        publishAll();
        Serial.printf("[Pub] door=%s fsr=%.3fV rssi=%ddBm mqtt=%s\n",
                      S.doorOpen ? "OPEN" : "CLOSED", S.fsrV,
                      WiFi.RSSI(), mqtt.connected() ? "ok" : "off");
    }
    delay(1);
}
