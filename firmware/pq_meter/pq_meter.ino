#include <WiFi.h>
#include <PubSubClient.h>

// ---------- Network settings ----------
const char* WIFI_SSID  = "YOUR_WIFI_NAME";         // must be a 2.4 GHz network
const char* WIFI_PASS  = "PUT_YOUR_WIFI_PASSWORD_HERE";
const char* MQTT_HOST  = "192.168.1.100";          // IP of the computer running Mosquitto
const int   MQTT_PORT  = 1883;
const char* MQTT_TOPIC = "power_meter/esp32_1/data";
const char* CLIENT_ID  = "esp32-power-meter";

// ---------- Measurement settings ----------
const int VPIN = 34;            // voltage channel (ADC1, works with WiFi on)
const int IPIN = 35;            // current channel (ADC1, works with WiFi on)
const int N = 250;              // 250 samples x 200 us = 50 ms = exactly 3 cycles at 60 Hz
const int DT_US = 200;
const int WINDOWS = 5;          // average 5 captures per result (15 cycles)
const int CYCLES = 3;           // fundamental = DFT bin 3
const int H_MAX = 15;           // highest harmonic to measure

const float DIVIDER_RATIO = 12.0;    // voltage divider (110k + 10k) / 10k
const float ADAPTER_RATIO = 11.49;   // calibrated vs Kill A Watt
const float ICAL = 19.47;            // calibrated vs Kill A Watt (fan, 3 turns, Nov 11)
const float I_NOISE = 0.19;          // noise floor in amps with 1 turn (measured Nov 11)
const float LOOP_TURNS = 3.0;        // number of times the wire passes through the CT
const float CT_DIRECTION = 1.0;      // flip to -1.0 if P reads negative
const float PHASE_SHIFT = 0.75;      // voltage shift in samples (1 sample = 4.32 deg), calibrated Nov 11
const float I_MIN = 0.15 / LOOP_TURNS;   // no-load threshold: 0.15 A at 1 turn, 0.05 A at 3 turns

const float V_SCALE = DIVIDER_RATIO * ADAPTER_RATIO / 1000.0;  // mains volts per ADC mV
const float I_SCALE = ICAL / 1000.0 / LOOP_TURNS;              // amps per ADC mV

int vS[N], iS[N];
float cosT[N], sinT[N];

WiFiClient espClient;
PubSubClient mqtt(espClient);

// ---------- WiFi + MQTT helpers ----------
void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.print("Connecting to WiFi");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("\nWiFi connected. ESP32 IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nWiFi FAILED - check name/password and 2.4 GHz");
  }
}

void connectMQTT() {
  if (mqtt.connected() || WiFi.status() != WL_CONNECTED) return;
  Serial.print("Connecting to MQTT broker... ");
  if (mqtt.connect(CLIENT_ID)) {
    Serial.println("connected");
  } else {
    Serial.print("failed, state = ");
    Serial.println(mqtt.state());
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  for (int k = 0; k < N; k++) {
    cosT[k] = cos(2.0 * PI * k / N);
    sinT[k] = sin(2.0 * PI * k / N);
  }
  connectWiFi();
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(512);   // room for the JSON message
}

void loop() {
  connectWiFi();
  connectMQTT();
  mqtt.loop();

  double vMsSum = 0, iMsSum = 0, pSum = 0;
  double p1Sum = 0, q1Sum = 0;
  double iHarmMs[H_MAX + 1], vHarmMs[H_MAX + 1];
  for (int h = 0; h <= H_MAX; h++) { iHarmMs[h] = 0; vHarmMs[h] = 0; }

  for (int w = 0; w < WINDOWS; w++) {
    // 1. Capture voltage and current together
    unsigned long t0 = micros();
    for (int k = 0; k < N; k++) {
      while (micros() - t0 < (unsigned long)k * DT_US) { }
      vS[k] = analogReadMilliVolts(VPIN);
      iS[k] = analogReadMilliVolts(IPIN);
    }

    // 2. Bias (average) of each channel
    long vSum = 0, iSum = 0;
    for (int k = 0; k < N; k++) { vSum += vS[k]; iSum += iS[k]; }
    float vMid = (float)vSum / N;
    float iMid = (float)iSum / N;

    // 3. Mean-square of V and I
    double vSq = 0, iSq = 0;
    for (int k = 0; k < N; k++) {
      float dv = vS[k] - vMid;
      float di = iS[k] - iMid;
      vSq += dv * dv;
      iSq += di * di;
    }
    vMsSum += vSq / N;
    iMsSum += iSq / N;

    // 4. Real power with phase-corrected voltage
    double vi = 0;
    for (int k = 1; k < N - 1; k++) {
      float dvPrev = vS[k - 1] - vMid;
      float dv     = vS[k]     - vMid;
      float dvNext = vS[k + 1] - vMid;
      float di     = iS[k]     - iMid;
      float vAdj = (PHASE_SHIFT >= 0) ? dv + PHASE_SHIFT * (dvNext - dv)
                                      : dv + PHASE_SHIFT * (dv - dvPrev);
      vi += vAdj * di;
    }
    pSum += vi / (N - 2);

    // 5. Harmonics (DFT at each multiple of 60 Hz)
    for (int h = 1; h <= H_MAX; h++) {
      int m = CYCLES * h;
      double reV = 0, imV = 0, reI = 0, imI = 0;
      for (int k = 0; k < N; k++) {
        int idx = (m * k) % N;
        float dv = vS[k] - vMid;
        float di = iS[k] - iMid;
        reV += dv * cosT[idx];  imV -= dv * sinT[idx];
        reI += di * cosT[idx];  imI -= di * sinT[idx];
      }
      reV *= 2.0 / N;  imV *= 2.0 / N;
      reI *= 2.0 / N;  imI *= 2.0 / N;
      vHarmMs[h] += (reV * reV + imV * imV) / 2.0;
      iHarmMs[h] += (reI * reI + imI * imI) / 2.0;

      if (h == 1) {
        float a = 2.0 * PI * m * PHASE_SHIFT / N;
        float ca = cos(a), sa = sin(a);
        double reVa = reV * ca - imV * sa;
        double imVa = reV * sa + imV * ca;
        p1Sum += (reVa * reI + imVa * imI) / 2.0;
        q1Sum += (imVa * reI - reVa * imI) / 2.0;
      }
    }
  }

  // 6. Results in real units
  float vrms = sqrt(vMsSum / WINDOWS) * V_SCALE;
  float irmsRaw = sqrt(iMsSum / WINDOWS) / 1000.0 * ICAL;
  float irms = sqrt(max(irmsRaw * irmsRaw - I_NOISE * I_NOISE, 0.0f)) / LOOP_TURNS;

  float P  = (pSum / WINDOWS) * V_SCALE * I_SCALE * CT_DIRECTION;
  float S  = vrms * irms;
  float PF = (S > 0.01) ? min(P / S, 1.0f) : 0.0;

  float v1  = sqrt(vHarmMs[1] / WINDOWS) * V_SCALE;
  float i1  = sqrt(iHarmMs[1] / WINDOWS) * I_SCALE;
  float P1  = (p1Sum / WINDOWS) * V_SCALE * I_SCALE * CT_DIRECTION;
  float Q1  = (q1Sum / WINDOWS) * V_SCALE * I_SCALE * CT_DIRECTION;
  float S1  = v1 * i1;
  float DPF = (S1 > 0.01) ? P1 / S1 : 0.0;

  double iHsq = 0, vHsq = 0;
  for (int h = 2; h <= H_MAX; h++) { iHsq += iHarmMs[h]; vHsq += vHarmMs[h]; }
  float thdI = (iHarmMs[1] > 0) ? 100.0 * sqrt(iHsq / iHarmMs[1]) : 0;
  float thdV = (vHarmMs[1] > 0) ? 100.0 * sqrt(vHsq / vHarmMs[1]) : 0;

  float hPct[10];   // harmonics 2..9 as % of fundamental
  for (int h = 2; h <= 9; h++) {
    hPct[h] = (iHarmMs[1] > 0) ? 100.0 * sqrt(iHarmMs[h] / iHarmMs[1]) : 0;
  }

  // 7. No-load rule: below I_MIN, current-based results are just noise
  if (irms < I_MIN) {
    irms = 0; P = 0; S = 0; PF = 0;
    P1 = 0; Q1 = 0; DPF = 0; thdI = 0;
    for (int h = 2; h <= 9; h++) hPct[h] = 0;
  }

  // 8. Build JSON message
  char msg[512];
  snprintf(msg, sizeof(msg),
    "{\"vrms\":%.2f,\"irms\":%.3f,\"p\":%.2f,\"s\":%.2f,\"pf\":%.3f,"
    "\"p1\":%.2f,\"q1\":%.2f,\"dpf\":%.3f,\"thd_i\":%.1f,\"thd_v\":%.1f,"
    "\"h2\":%.1f,\"h3\":%.1f,\"h4\":%.1f,\"h5\":%.1f,\"h6\":%.1f,\"h7\":%.1f,\"h8\":%.1f,\"h9\":%.1f}",
    vrms, irms, P, S, PF, P1, Q1, DPF, thdI, thdV,
    hPct[2], hPct[3], hPct[4], hPct[5], hPct[6], hPct[7], hPct[8], hPct[9]);

  // 9. Publish and print
  bool sent = mqtt.connected() && mqtt.publish(MQTT_TOPIC, msg);
  Serial.print(sent ? "[MQTT sent] " : "[MQTT not sent] ");
  Serial.println(msg);

  delay(1000);
}
