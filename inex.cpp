#include <WiFi.h>
#include <WebServer.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <ArduinoOTA.h>
#include "AudioTools.h"
#include "AudioTools/AudioLibs/MaximilianDSP.h"


maxiClock myClock;
maxiFilter filter;
maxiOsc osc[4]; 

I2SStream out;
Maximilian maximilian(out);

const int TOUCH_PINS[] = {14, 33, 32, 27};
const int NUM_SENSORS = 4;

// --- VARIABLES COMPARTIDAS ENTRE AUDIO Y SENSORES ---
volatile float currentVolumes[4] = {0.0, 0.0, 0.0, 0.0};
volatile double targetFreqs[4] = {261.63, 329.63, 392.00, 493.88};

// Frecuencias actuales suavizadas
double currentFreqs[4] = {261.63, 329.63, 392.00, 493.88};

// Estado anterior de volumen para detectar cambios (trigger de nota)
float previousVolumes[4] = {0.0, 0.0, 0.0, 0.0};

// --- BANCOS DE NOTAS ARMÓNICAS (Pentatónica de Do Mayor por octavas) ---
double bank0[] = {130.81, 146.83, 164.81}; // C3, D3, E3
double bank1[] = {196.00, 220.00, 261.63}; // G3, A3, C4
double bank2[] = {293.66, 329.63, 392.00}; // D4, E4, G4
double bank3[] = {440.00, 523.25, 587.33}; // A4, C5, D5
double* noteBanks[] = {bank0, bank1, bank2, bank3};

// Índices de nota actual para cada oscilador
int currentNoteIndex[4] = {0, 0, 0, 0};

// --- CALIBRACIÓN ---
struct Calibration { int idle; int touch; int barefoot; };
Calibration calibData[NUM_SENSORS] = {
  {70, 40, 15}, {70, 40, 15}, {70, 40, 15}, {70, 40, 15}
};

int currentReadings[NUM_SENSORS];
int currentStates[NUM_SENSORS]; 

// --- GESTIÓN WIFI ---
WebServer server(80);
Preferences preferences;
bool wifiActive = true;
unsigned long bootTime = 0;

// --- HTML ---
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<style>body{font-family:sans-serif;background:#111;color:#eee;text-align:center;padding:10px}.box{background:#222;margin:10px;padding:10px;border-radius:8px}input{width:50px;background:#333;color:#fff;border:1px solid #555}.s{float:right;padding:2px 8px;border-radius:4px}</style></head>
<body><h3>Synth Calibration</h3><div id="status" style="color:orange">WiFi se apagará en 60s</div>
<form action="/save" method="POST"><div id="c"></div><br><button type="submit">GUARDAR</button></form>
<script>
fetch('/calib').then(r=>r.json()).then(d=>{
  let h="";d.forEach((s,i)=>{h+=`<div class="box"><b>Sensor ${i+1}</b> <span id="v${i}">-</span> <span id="st${i}" class="s">.</span><br>
  I:<input name="i${i}" value="${s.i}"> T:<input name="t${i}" value="${s.t}"> B:<input name="b${i}" value="${s.b}"></div>`});
  document.getElementById('c').innerHTML=h; setInterval(u,200);
});
function u(){fetch('/read').then(r=>r.json()).then(d=>{d.forEach((x,i)=>{
  document.getElementById('v'+i).innerText=x.v;
  let b=document.getElementById('st'+i);b.innerText=x.s==0?"OFF":(x.s==1?"TOC":"PIE");b.style.background=x.s==0?"#333":(x.s==1?"#d60":"#d04");
})})};
</script></body></html>
)rawliteral";

int determineState(int val, int idx) {
  int dI = abs(val - calibData[idx].idle);
  int dT = abs(val - calibData[idx].touch);
  int dB = abs(val - calibData[idx].barefoot);
  if (dI <= dT && dI <= dB) return 0;
  if (dT < dI && dT <= dB) return 1;
  return 2;
}

void handleRoot() { server.send(200, "text/html", index_html); }
void handleGetCalib() {
  String j="["; for(int i=0;i<4;i++){ j+=String("{\"i\":")+calibData[i].idle+",\"t\":"+calibData[i].touch+",\"b\":"+calibData[i].barefoot+"}"; if(i<3)j+=",";} j+="]";
  server.send(200, "application/json", j);
}
void handleSave() {
  for(int i=0;i<4;i++){
    String sI=server.arg("i"+String(i)); String sT=server.arg("t"+String(i)); String sB=server.arg("b"+String(i));
    if(sI!="")calibData[i].idle=sI.toInt(); if(sT!="")calibData[i].touch=sT.toInt(); if(sB!="")calibData[i].barefoot=sB.toInt();
  }
  preferences.begin("calib",false); preferences.putBytes("d",calibData,sizeof(calibData)); preferences.end();
  server.sendHeader("Location","/"); server.send(303);
}
void handleRead() {
  String j="["; for(int i=0;i<4;i++){ j+=String("{\"v\":")+currentReadings[i]+",\"s\":"+currentStates[i]+"}"; if(i<3)j+=",";} j+="]";
  server.send(200, "application/json", j);
}

// ======================================================
// === AUDIO CALLBACK ===
// ======================================================
void play(float *output) {
  // NO usamos el reloj para cambios aleatorios
  // Las notas cambiarán solo cuando se toquen los sensores
  
  double mixedSignal = 0.0;

  float volumen= 0.0f;
  
  for (int i = 0; i < 4; i++) {
    // Suavizado de frecuencia (glide/portamento)
    currentFreqs[i] += (targetFreqs[i] - currentFreqs[i]) * 0.005; // Más lento = más suave
    
    // Generamos el oscilador
    double oscSound = osc[i].triangle(currentFreqs[i]) ;
 
    // En lugar de: double oscSound = osc[i].sawn(currentFreqs[i]);
    
    // Aplicamos el volumen
    mixedSignal += oscSound * currentVolumes[i];
    volumen += currentVolumes[i];
  }

  // Filtrado global
  double filtered = filter.lores(mixedSignal, 2500 * volumen, 0.5);

  // Salida estéreo
  output[0] = filtered * 0.25; 
  output[1] = output[0];
}

void setup() {
  Serial.begin(115200);
  bootTime = millis();



  preferences.begin("calib", true);
  if(preferences.getBytesLength("d") == sizeof(calibData)) {
    preferences.getBytes("d", calibData, sizeof(calibData));
  }
  preferences.end();
  
  WiFiManager wm;
  wm.setConfigPortalTimeout(60);
  if(!wm.autoConnect("ESP32_SYNTH")) {
    Serial.println("No WiFi, modo offline");
    wifiActive = false;
  }
  
  if(wifiActive) {
    server.on("/", handleRoot);
    server.on("/calib", handleGetCalib);
    server.on("/save", handleSave);
    server.on("/read", handleRead);
    server.begin();
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
  }
  
  auto cfg = out.defaultConfig(TX_MODE);
  cfg.is_master = true;
  cfg.pin_bck = 26; 
  cfg.pin_ws = 25; 
  cfg.pin_data = 22;
  cfg.sample_rate = 32000; 
  cfg.buffer_size = 512;

  myClock.setTicksPerBeat(4);
  myClock.setTempo(90); // Tempo más lento
  
  out.begin(cfg);
  maximilian.begin(cfg);
  
  Serial.println("=== SYNTH INICIADO ===");
  Serial.println("Formato: S1[raw vol%] S2[raw vol%] S3[raw vol%] S4[raw vol%]");

  ArduinoOTA.setHostname("inex-esp32-ota");


  ArduinoOTA.begin();
}

unsigned long lastSensorRead = 0;
unsigned long lastWebCheck = 0;
unsigned long lastDebug = 0;

void loop() {
  maximilian.copy();
  
  unsigned long now = millis();
  
  // APAGAR WIFI
  if(wifiActive && (now - bootTime > 60000)) {
    Serial.println("WiFi OFF");
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    wifiActive = false;
  }

  // LECTURA DE SENSORES
  if(now - lastSensorRead >= 10) {
    lastSensorRead = now;
    
    for(int i = 0; i < NUM_SENSORS; i++) {
      int raw = touchRead(TOUCH_PINS[i]);
      currentReadings[i] = raw;
      
      // CÁLCULO DE VOLUMEN
      // Valores BAJOS = más contacto = más volumen
      float volume = 0.0;
      
      if(raw <= calibData[i].barefoot) {
        volume = 1.0;
      } else if(raw >= calibData[i].idle) {
        volume = 0.0;
      } else {
        // Interpolación: cuando raw baja (más contacto), volumen sube
        volume = (float)(calibData[i].idle - raw) / (float)(calibData[i].idle - calibData[i].barefoot);
        volume = constrain(volume, 0.0, 1.0);
      }
      
      // Umbral de ruido
      if(volume < 0.08) volume = 0.0;
      
      // DETECCIÓN DE NOTA NUEVA: Solo cuando el volumen cruza un umbral subiendo
      // (cuando empiezas a tocar un sensor)
      if(volume > 0.2 && previousVolumes[i] <= 0.2) {
        // ¡TRIGGER! Cambiamos a una nueva nota aleatoria
        currentNoteIndex[i] = random(0, 3);
        targetFreqs[i] = noteBanks[i][currentNoteIndex[i]];
        
        // Debug del trigger
        Serial.printf("→ S%d NOTA %d (%.0fHz)\n", i+1, currentNoteIndex[i]+1, targetFreqs[i]);
      }
      
      previousVolumes[i] = volume;
      
      // Suavizado del volumen
      //currentVolumes[i] += (volume - currentVolumes[i]) * 0.2;
      if (volume > currentVolumes[i]) {
          // ATAQUE RÁPIDO (0.1)
          currentVolumes[i] += (volume - currentVolumes[i]) * 0.1;
      } else {
          // RELEASE LENTO (0.01) - Deja una "cola" de sonido
          currentVolumes[i] += (volume - currentVolumes[i]) * 0.01;
      }
    }
  }
  
  // DEBUG PERIÓDICO: Todos los sensores en una línea
  if(now - lastDebug >= 500) { // Cada 500ms
    lastDebug = now;
    
    Serial.print("│ ");
    for(int i = 0; i < NUM_SENSORS; i++) {
      int volPercent = (int)(currentVolumes[i] * 100);
      
      // Formato: S1[raw=XX vol=XX%]
      Serial.printf("S%d[%d %d%%] ", i+1, currentReadings[i], volPercent);
    }
    Serial.println("│");
  }
  
  // SERVIDOR WEB
  if(wifiActive && (now - lastWebCheck >= 100)) {
    lastWebCheck = now;
    for(int i = 0; i < NUM_SENSORS; i++) {
      int state = determineState(currentReadings[i], i);
      currentStates[i] = state;
    }
    server.handleClient();
    ArduinoOTA.handle();  // Handles OTA
  }
}
