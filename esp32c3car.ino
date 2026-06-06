#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>

// ===================== MOTOR PINS
#define M1_H 20
#define M1_L 6
#define M2_H 7
#define M2_L 8

// ===================== WIFI AP
const char* ssid = "ESP32-CAR";
const char* password = "12345678";

// ===================== SERVER
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// ===================== CONTROL
volatile int joyX = 0;
volatile int joyY = 0;
volatile unsigned long lastPacket = 0;

// ===================== PWM
const int PWM_FREQ = 5000;
const int PWM_RES  = 8;

// ===================== MOTOR STATE
int lastM1 = 0;
int lastM2 = 0;

unsigned long m1_dead_until = 0;
unsigned long m2_dead_until = 0;

// ===================== MOTOR DRIVER (WiFi SAFE)
void motorWrite(int pinH, int pinL, int val, int &lastState, unsigned long &deadUntil)
{
  val = constrain(val, -255, 255);
  unsigned long now = micros();

  // non-blocking dead time
  if (now < deadUntil)
  {
    ledcWrite(pinH, 0);
    ledcWrite(pinL, 0);
    return;
  }

  // direction change → apply dead-time
  if ((val > 0 && lastState < 0) ||
      (val < 0 && lastState > 0))
  {
    ledcWrite(pinH, 0);
    ledcWrite(pinL, 0);
    deadUntil = now + 80;
    lastState = 0;
    return;
  }

  if (val > 0)
  {
    ledcWrite(pinH, val);
    ledcWrite(pinL, 0);
  }
  else if (val < 0)
  {
    ledcWrite(pinH, 0);
    ledcWrite(pinL, -val);
  }
  else
  {
    ledcWrite(pinH, 0);
    ledcWrite(pinL, 0);
  }

  lastState = val;
}

void drive(int left, int right)
{
  motorWrite(M1_H, M1_L, left, lastM1, m1_dead_until);
  motorWrite(M2_H, M2_L, right, lastM2, m2_dead_until);
}

// ===================== WEBSOCKET
void onWsEvent(
  AsyncWebSocket *server,
  AsyncWebSocketClient *client,
  AwsEventType type,
  void *arg,
  uint8_t *data,
  size_t len)
{
  if (type == WS_EVT_CONNECT)
  {
    Serial.println("[WS] Client connected");
  }

  if (type == WS_EVT_DISCONNECT)
  {
    Serial.println("[WS] Client disconnected");
  }

  if (type == WS_EVT_DATA)
  {
    String msg;
    for (size_t i = 0; i < len; i++) msg += (char)data[i];

    int c = msg.indexOf(',');

    if (c > 0)
    {
      joyX = msg.substring(0, c).toInt();
      joyY = msg.substring(c + 1).toInt();
      lastPacket = millis();

      Serial.print("[WS] X=");
      Serial.print(joyX);
      Serial.print(" Y=");
      Serial.println(joyY);
    }
  }
}

// ===================== SIMPLE UI
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
body{margin:0;background:#0b1220;color:white;font-family:Arial;overflow:hidden}
#pad{width:320px;height:320px;margin:auto;margin-top:40px;border-radius:50%;background:#1e293b;position:relative}
#stick{width:90px;height:90px;border-radius:50%;position:absolute;left:115px;top:115px;background:#00ffaa}
</style>
</head>
<body>

<div id="pad"><div id="stick"></div></div>

<script>
let ws;
function connect(){
ws=new WebSocket("ws://"+location.host+"/ws");
ws.onopen=()=>console.log("WS ON");
ws.onclose=()=>setTimeout(connect,1000);
}
connect();

let pad=document.getElementById("pad");
let stick=document.getElementById("stick");

let x=0,y=0,r=120;

function send(){
if(ws&&ws.readyState==1)ws.send(x+","+y);
}
setInterval(send,40);

function move(e){
let b=pad.getBoundingClientRect();
let dx=e.clientX-b.left-160;
let dy=e.clientY-b.top-160;

let d=Math.sqrt(dx*dx+dy*dy);
if(d>r){dx/=d;dy/=d;dx*=r;dy*=r;}

stick.style.left=(dx+160-45)+"px";
stick.style.top=(dy+160-45)+"px";

x=Math.round(dx/r*100);
y=Math.round(-dy/r*100);
}

pad.onpointerdown=move;
pad.onpointermove=e=>{if(e.buttons)move(e)};
pad.onpointerup=()=>{x=0;y=0;stick.style.left="115px";stick.style.top="115px";}
</script>

</body>
</html>
)rawliteral";

// ===================== START SOFTAP (FIXED ORDER)
void startSoftAP()
{
  Serial.println("\n[WiFi] Starting SoftAP...");

  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP);

  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);

  delay(500); // IMPORTANT for ESP32-C3

  bool ok = WiFi.softAP(ssid, password);

  delay(300);

  if (ok)
    Serial.println("[WiFi] SoftAP started OK");
  else
    Serial.println("[WiFi] SoftAP FAILED");

  Serial.print("[WiFi] SSID: ");
  Serial.println(ssid);

  Serial.print("[WiFi] IP: ");
  Serial.println(WiFi.softAPIP());
}

// ===================== SETUP
void setup()
{
  Serial.begin(115200);
  delay(1000);

  // 1. WiFi FIRST (CRITICAL FIX)
  startSoftAP();

  // 2. WebSocket
  ws.onEvent(onWsEvent);
  server.addHandler(&ws);

  // 3. HTTP server
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send_P(200, "text/html", index_html);
  });

  server.begin();

  // 4. PWM LAST
  ledcAttach(M1_H, PWM_FREQ, PWM_RES);
  ledcAttach(M1_L, PWM_FREQ, PWM_RES);
  ledcAttach(M2_H, PWM_FREQ, PWM_RES);
  ledcAttach(M2_L, PWM_FREQ, PWM_RES);

  Serial.println("[SYSTEM] READY");
}

// ===================== LOOP
void loop()
{
  if (millis() - lastPacket > 250)
  {
    drive(0, 0);
    return;
  }

  static unsigned long lastDrive = 0;
  if (millis() - lastDrive < 20) return;
  lastDrive = millis();

  float throttle = joyY / 100.0f;
  float steer    = joyX / 100.0f;

  float left  = throttle + steer;
  float right = throttle - steer;

  float maxV = max(abs(left), abs(right));

  if (maxV > 1.0f)
  {
    left /= maxV;
    right /= maxV;
  }

  drive(left * 255, right * 255);
}