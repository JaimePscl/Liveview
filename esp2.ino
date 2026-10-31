#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_camera.h>
#include <esp_sleep.h>
#include <mbedtls/base64.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include "secrets.h"

#define PWDN_GPIO_NUM 32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 0
#define SIOD_GPIO_NUM 26
#define SIOC_GPIO_NUM 27
#define Y9_GPIO_NUM 35
#define Y8_GPIO_NUM 34
#define Y7_GPIO_NUM 39
#define Y6_GPIO_NUM 36
#define Y5_GPIO_NUM 21
#define Y4_GPIO_NUM 19
#define Y3_GPIO_NUM 18
#define Y2_GPIO_NUM 5
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM 23
#define PCLK_GPIO_NUM 22

static const char *GH_HOST = "api.github.com";
static const char *RAW_HOST = "raw.githubusercontent.com"; // lectura de config (plano)
static const char *USER_AGENT = "esp32cam-uploader";
static const size_t GET_BUF_SIZE = 300 * 1024;
static const unsigned long HTTP_TIMEOUT_MS = 15000;
static const uint32_t NUM_SLOTS = 10; // fotos que se mantienen (ranuras rotativas)

// Intervalo entre fotos (min). Configurable via config.txt; sleepNow() lo usa.
static uint32_t intervalMin = 10;

// Modo powerbank (keepalive=1): duerme en tramos cortos con despertares
// breves para que el banco no corte la salida por bajo consumo.
static const uint32_t SLICE_SLEEP_S = 20;  // sueno por tramo
static const uint32_t PULSE_MS = 2000;     // duracion del pulso de consumo
static const uint32_t SLICE_PERIOD_S = 22; // tramo efectivo (sueno + boot + pulso)
RTC_DATA_ATTR uint8_t keepAlive = 0;
RTC_DATA_ATTR uint32_t slicesLeft = 0;

// Parametros de camara configurables desde la web (config.txt en la raiz del repo).
struct CamCfg {
  int framesize;   // enum framesize_t (SVGA=9). Ver README.
  int quality;     // 10 (mejor) .. 63 (peor)
  int brightness;  // -2 .. 2
  int contrast;    // -2 .. 2
  int saturation;  // -2 .. 2
  int hmirror;     // 0/1
  int vflip;       // 0/1
  int interval;    // minutos entre fotos, 1 .. 1440
  int wbmode;      // 0=auto 1=soleado 2=nublado 3=oficina 4=hogar
  int keepalive;   // 0/1 modo powerbank (despertares cortos anti-apagado)
};

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Sobrevive al deep sleep (no a un corte de energia). Marca la ranura actual.
RTC_DATA_ATTR uint32_t bootCount = 0;

void sleepNow() {
  if (keepAlive) {
    uint32_t total = (intervalMin * 60) / SLICE_PERIOD_S;
    if (total < 1) total = 1;
    slicesLeft = total;
    Serial.printf("Deep sleep %lu min en %lu tramos (keep-alive powerbank)\n",
                  (unsigned long)intervalMin, (unsigned long)total);
    Serial.flush();
    esp_sleep_enable_timer_wakeup((uint64_t)SLICE_SLEEP_S * 1000000ULL);
  } else {
    Serial.printf("Deep sleep %lu min...\n", (unsigned long)intervalMin);
    Serial.flush();
    esp_sleep_enable_timer_wakeup((uint64_t)intervalMin * 60ULL * 1000000ULL);
  }
  esp_deep_sleep_start();
}

void fail(const char *msg) {
  Serial.println(msg);
  sleepNow();
}

bool wifiConnect() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(STA_SSID, STA_PASS);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(250);
  }
  return WiFi.status() == WL_CONNECTED;
}

// Escaneo WiFi. Devuelve cantidad total y arma la lista "SSID:rssi,..."
// EXCLUYENDO la red propia (STA_SSID) por privacidad; la pagina es publica.
int scanWifi(String &list) {
  int n = WiFi.scanNetworks();
  if (n < 0) n = 0;
  for (int i = 0; i < n && list.length() < 300; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid == STA_SSID || ssid.length() == 0) continue; // ocultar la propia
    ssid.replace("\n", " "); ssid.replace("=", " ");
    ssid.replace(",", " "); ssid.replace(":", " ");
    if (list.length()) list += ",";
    list += ssid + ":" + String(WiFi.RSSI(i));
  }
  WiFi.scanDelete();
  return n;
}

// Sincroniza el reloj por NTP (UTC). Devuelve true si logro la hora.
bool syncTime() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov"); // offset 0 = UTC
  struct tm ti;
  return getLocalTime(&ti, 5000); // espera hasta 5 s a que sincronice
}

// Escaneo BLE de 3 s. Devuelve cantidad y junta los pocos nombres publicados.
// Se hace ANTES de conectar WiFi y se libera la RAM del stack BT enseguida
// (TLS necesita esa memoria despues).
int scanBle(String &names) {
  BLEDevice::init("");
  BLEScan *scan = BLEDevice::getScan();
  scan->setActiveScan(true); // pide scan response (ahi suelen venir los nombres)
  BLEScanResults *res = scan->start(3, false);
  int count = res ? res->getCount() : 0;
  for (int i = 0; i < count && names.length() < 300; i++) {
    BLEAdvertisedDevice d = res->getDevice(i);
    if (d.haveName()) {
      String n = d.getName().c_str();
      // limpiar caracteres que rompen el formato clave=valor,coma
      n.replace("\n", " "); n.replace("=", " "); n.replace(",", " ");
      if (names.length()) names += ",";
      names += n;
    }
  }
  scan->clearResults();
  BLEDevice::deinit(true);
  return count;
}

// GET plano (no la API): devuelve el body+headers en out. Para archivos chicos.
bool httpGetText(const char *host, const char *path, char *out, size_t outSize) {
  WiFiClientSecure client;
  client.setInsecure(); // TODO v2: pinear el certificado real
  client.setTimeout(HTTP_TIMEOUT_MS);
  if (!client.connect(host, 443)) {
    return false;
  }
  client.print(String("GET ") + path + " HTTP/1.1\r\n" +
               "Host: " + host + "\r\n" +
               "User-Agent: " + USER_AGENT + "\r\n" +
               "Connection: close\r\n\r\n");
  size_t len = 0;
  unsigned long start = millis();
  while ((client.connected() || client.available()) && millis() - start < HTTP_TIMEOUT_MS) {
    while (client.available() && len < outSize - 1) {
      out[len++] = client.read();
      start = millis();
    }
    if (len >= outSize - 1) break;
    delay(1);
  }
  out[len] = '\0';
  client.stop();
  char *sl = strstr(out, "HTTP/1.1 ");
  return sl && atoi(sl + 9) == 200;
}

// Busca "clave=" en el texto y devuelve el entero que le sigue, o def.
static int cfgInt(const char *buf, const char *key, int def) {
  const char *p = strstr(buf, key);
  if (!p) return def;
  return atoi(p + strlen(key));
}

// Tras un reset fisico (boton RST) el contador RTC vuelve a 0, lo que
// desordenaria la galeria. Retomamos la rotacion desde el slot publicado.
void resumeRotation() {
  static char buf[1024];
  char path[128];
  snprintf(path, sizeof(path), "/%s/%s/main/fotos/latest.txt", GH_USER, GH_REPO);
  if (httpGetText(RAW_HOST, path, buf, sizeof(buf))) {
    int last = cfgInt(buf, "slot=", -1);
    if (last >= 0) {
      bootCount = (uint32_t)last + 1;
      Serial.printf("Reset detectado: rotacion retoma en ranura %lu\n",
                    (unsigned long)(bootCount % NUM_SLOTS));
    }
  }
}

// Lee fotos/config.txt del repo (raw). Si falla, deja los defaults.
CamCfg loadConfig() {
  CamCfg c = {9, 12, 0, 0, 0, 0, 0, 10, 0, 0}; // SVGA, cal 12, neutro, 10 min, WB auto, sin keepalive
  static char buf[3072];
  char path[128];
  snprintf(path, sizeof(path), "/%s/%s/main/config.txt", GH_USER, GH_REPO);

  if (httpGetText(RAW_HOST, path, buf, sizeof(buf))) {
    c.framesize  = clampi(cfgInt(buf, "framesize=",  c.framesize),  0, 13);
    c.quality    = clampi(cfgInt(buf, "quality=",    c.quality),   10, 63);
    c.brightness = clampi(cfgInt(buf, "brightness=", c.brightness), -2, 2);
    c.contrast   = clampi(cfgInt(buf, "contrast=",   c.contrast),   -2, 2);
    c.saturation = clampi(cfgInt(buf, "saturation=", c.saturation), -2, 2);
    c.hmirror    = clampi(cfgInt(buf, "hmirror=",    c.hmirror),     0, 1);
    c.vflip      = clampi(cfgInt(buf, "vflip=",      c.vflip),       0, 1);
    c.interval   = clampi(cfgInt(buf, "interval=",   c.interval),    1, 1440);
    c.wbmode     = clampi(cfgInt(buf, "wbmode=",     c.wbmode),      0, 4);
    c.keepalive  = clampi(cfgInt(buf, "keepalive=",  c.keepalive),   0, 1);
    Serial.printf("Config: fs=%d q=%d br=%d ct=%d sat=%d hm=%d vf=%d int=%d\n",
                  c.framesize, c.quality, c.brightness, c.contrast,
                  c.saturation, c.hmirror, c.vflip, c.interval);
  } else {
    Serial.println("config.txt no leido, usando defaults");
  }
  return c;
}

camera_fb_t *capturePhoto(const CamCfg &cfg) {
  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  // 10 MHz en vez de 20: fix conocido para tinte verde/purpura en clones
  // del OV2640 (integridad de señal). Mas lento por frame, no importa aca.
  config.xclk_freq_hz = 10000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = (framesize_t)cfg.framesize;
  config.jpeg_quality = cfg.quality;
  config.fb_count = 2;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.grab_mode = CAMERA_GRAB_LATEST;

  if (esp_camera_init(&config) != ESP_OK) {
    return nullptr;
  }

  // Ajustes de imagen que van al sensor (no al buffer).
  sensor_t *s = esp_camera_sensor_get();
  if (s) {
    s->set_brightness(s, cfg.brightness);
    s->set_contrast(s, cfg.contrast);
    s->set_saturation(s, cfg.saturation);
    s->set_hmirror(s, cfg.hmirror);
    s->set_vflip(s, cfg.vflip);
    s->set_whitebal(s, 1);   // AWB on
    s->set_awb_gain(s, 1);   // ganancia AWB on (necesaria para wb_mode)
    s->set_wb_mode(s, cfg.wbmode); // 0=auto, >0 fuerza un preset
    s->set_lenc(s, 1);       // correccion de lente
    s->set_raw_gma(s, 1);    // gamma raw
    s->set_bpc(s, 1);        // correccion pixeles muertos
    s->set_wpc(s, 1);        // correccion pixeles blancos
    s->set_gain_ctrl(s, 1);  // AGC on
    s->set_exposure_ctrl(s, 1); // AEC on
  }

  // AWB y exposicion necesitan ~2 s de frames para converger tras el init;
  // si no, salen fotos con tinte verde/magenta. Descartamos 12 frames.
  for (int i = 0; i < 12; i++) {
    camera_fb_t *warmup = esp_camera_fb_get();
    if (warmup) esp_camera_fb_return(warmup);
    delay(100);
  }

  return esp_camera_fb_get();
}

// GET al path dado. sha40 se rellena si se encuentra "sha":"..." en el body;
// found queda false si el archivo no existe (404 -> primera subida).
// Devuelve true si la peticion se pudo interpretar (200 con sha, o 404).
bool httpGetSha(const char *path, char *sha40, size_t sha40Size, bool *found) {
  *found = false;
  sha40[0] = '\0';

  char *buf = (char *)ps_malloc(GET_BUF_SIZE);
  if (!buf) {
    Serial.println("ps_malloc GET buffer failed");
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure(); // TODO v2: pinear el certificado real de api.github.com
  client.setTimeout(HTTP_TIMEOUT_MS);

  if (!client.connect(GH_HOST, 443)) {
    Serial.println("GET connect failed");
    free(buf);
    return false;
  }

  client.print(String("GET ") + path + " HTTP/1.1\r\n" +
               "Host: " + GH_HOST + "\r\n" +
               "User-Agent: " + USER_AGENT + "\r\n" +
               "Authorization: Bearer " + GH_TOKEN + "\r\n" +
               "Accept: application/vnd.github+json\r\n" +
               "Connection: close\r\n\r\n");

  size_t len = 0;
  unsigned long start = millis();
  while ((client.connected() || client.available()) && millis() - start < HTTP_TIMEOUT_MS) {
    while (client.available() && len < GET_BUF_SIZE - 1) {
      buf[len++] = client.read();
      start = millis();
    }
    if (len >= GET_BUF_SIZE - 1) break;
    delay(1);
  }
  buf[len] = '\0';
  client.stop();

  int status = 0;
  char *statusLine = strstr(buf, "HTTP/1.1 ");
  if (statusLine) status = atoi(statusLine + 9);

  if (status == 200) {
    char *shaKey = strstr(buf, "\"sha\":\"");
    if (shaKey) {
      char *shaStart = shaKey + 7;
      char *shaEnd = strchr(shaStart, '"');
      if (shaEnd && (size_t)(shaEnd - shaStart) < sha40Size) {
        memcpy(sha40, shaStart, shaEnd - shaStart);
        sha40[shaEnd - shaStart] = '\0';
        *found = true;
      }
    }
    free(buf);
    return *found;
  }

  free(buf);
  if (status == 404) {
    return true; // primera subida de este archivo, sin sha
  }
  Serial.printf("GET %s status inesperado: %d\n", path, status);
  return false;
}

// PUT de data (JPEG o texto) al path dado, base64 en PSRAM. Content-Length exacto.
bool httpPut(const char *path, const char *sha40, bool hasSha,
             const uint8_t *data, size_t dataLen) {
  size_t b64Len = 0;
  mbedtls_base64_encode(nullptr, 0, &b64Len, data, dataLen);

  const char *prefix = "{\"message\":\"upload\",\"content\":\"";
  const char *shaTag = "\",\"sha\":\"";
  size_t bodyCap = strlen(prefix) + b64Len + strlen(shaTag) + 40 + 4;

  char *body = (char *)ps_malloc(bodyCap);
  if (!body) {
    Serial.println("ps_malloc PUT buffer failed");
    return false;
  }

  size_t pos = 0;
  memcpy(body + pos, prefix, strlen(prefix));
  pos += strlen(prefix);

  size_t encodedLen = 0;
  int rc = mbedtls_base64_encode((unsigned char *)(body + pos), b64Len, &encodedLen, data, dataLen);
  if (rc != 0) {
    Serial.println("base64 encode failed");
    free(body);
    return false;
  }
  pos += encodedLen;

  if (hasSha) {
    memcpy(body + pos, shaTag, strlen(shaTag));
    pos += strlen(shaTag);
    memcpy(body + pos, sha40, strlen(sha40));
    pos += strlen(sha40);
    body[pos++] = '"';
    body[pos++] = '}';
  } else {
    body[pos++] = '"';
    body[pos++] = '}';
  }

  WiFiClientSecure client;
  client.setInsecure(); // TODO v2: pinear el certificado real de api.github.com
  client.setTimeout(HTTP_TIMEOUT_MS);

  if (!client.connect(GH_HOST, 443)) {
    Serial.println("PUT connect failed");
    free(body);
    return false;
  }

  client.print(String("PUT ") + path + " HTTP/1.1\r\n" +
               "Host: " + GH_HOST + "\r\n" +
               "User-Agent: " + USER_AGENT + "\r\n" +
               "Authorization: Bearer " + GH_TOKEN + "\r\n" +
               "Accept: application/vnd.github+json\r\n" +
               "Content-Type: application/json\r\n" +
               "Content-Length: " + String(pos) + "\r\n" +
               "Connection: close\r\n\r\n");
  client.write((const uint8_t *)body, pos);
  free(body);

  char statusBuf[64] = {0};
  size_t sl = 0;
  unsigned long start = millis();
  while ((client.connected() || client.available()) && millis() - start < HTTP_TIMEOUT_MS && sl < sizeof(statusBuf) - 1) {
    if (client.available()) {
      statusBuf[sl++] = client.read();
      start = millis();
      if (strchr(statusBuf, '\n')) break;
    } else {
      delay(1);
    }
  }
  client.stop();

  int status = 0;
  char *statusLine = strstr(statusBuf, "HTTP/1.1 ");
  if (statusLine) status = atoi(statusLine + 9);

  if (status == 200 || status == 201) {
    return true;
  }
  Serial.printf("PUT %s status inesperado: %d\n", path, status);
  return false;
}

void setup() {
  Serial.begin(115200);
  delay(200);

  // Despertar corto del modo powerbank: generar consumo ~2 s y volver a
  // dormir. Sin WiFi ni camara. Un reset fisico (RST) salta esto porque
  // la RTC RAM se borra -> ciclo completo con foto inmediata.
  if (keepAlive && slicesLeft > 1 &&
      esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER) {
    slicesLeft--;
    unsigned long until = millis() + PULSE_MS;
    volatile uint32_t burn = 1;
    while (millis() < until) { burn = burn * 1664525u + 1013904223u; }
    esp_sleep_enable_timer_wakeup((uint64_t)SLICE_SLEEP_S * 1000000ULL);
    esp_deep_sleep_start();
  }

  // Telemetria: escaneos antes de conectar (el BLE se libera antes del TLS).
  WiFi.mode(WIFI_STA);
  String wifiList;
  int wifiCount = scanWifi(wifiList);
  String btNames;
  int btCount = scanBle(btNames);
  Serial.printf("Telemetria: %d redes WiFi, %d dispositivos BLE\n", wifiCount, btCount);

  if (!wifiConnect()) {
    fail("WiFi connect failed");
    return;
  }
  int rssi = WiFi.RSSI();

  // Despertar por timer = ciclo normal. Cualquier otra causa (boton RST,
  // primer encendido) resetea el contador RTC: retomamos desde latest.txt.
  if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_TIMER) {
    resumeRotation();
  }
  uint32_t slot = bootCount % NUM_SLOTS;
  Serial.printf("Ciclo %u -> ranura foto%u.jpg\n", (unsigned)bootCount, (unsigned)slot);

  bool timeOk = syncTime(); // para el timestamp del upload (no es fatal si falla)

  CamCfg cfg = loadConfig();
  intervalMin = cfg.interval;
  keepAlive = cfg.keepalive; // persiste en RTC para los despertares cortos

  camera_fb_t *fb = capturePhoto(cfg);
  if (!fb) {
    fail("Camera capture failed");
    return;
  }

  // 1) Subir la foto a su ranura foto{slot}.jpg
  char photoPath[160];
  snprintf(photoPath, sizeof(photoPath),
           "/repos/%s/%s/contents/fotos/foto%lu.jpg",
           GH_USER, GH_REPO, (unsigned long)slot);

  char sha[41];
  bool hasSha = false;
  if (!httpGetSha(photoPath, sha, sizeof(sha), &hasSha)) {
    esp_camera_fb_return(fb);
    fail("GET sha foto failed");
    return;
  }

  size_t jpgLen = fb->len;
  bool ok = httpPut(photoPath, sha, hasSha, fb->buf, fb->len);
  esp_camera_fb_return(fb);
  if (!ok) {
    fail("PUT foto failed");
    return;
  }
  Serial.println("Foto subida OK");

  // 2) Actualizar latest.txt: ranura + timestamp + telemetria del ciclo.
  //    ts=0 si NTP no sincronizo; la web lo trata como "hora desconocida".
  char txtPath[160];
  snprintf(txtPath, sizeof(txtPath),
           "/repos/%s/%s/contents/fotos/latest.txt", GH_USER, GH_REPO);

  time_t ts = timeOk ? time(nullptr) : 0;
  char meta[1024];
  int metaLen = snprintf(meta, sizeof(meta),
                         "slot=%lu\nts=%ld\ninterval=%lu\nsize=%u\nrssi=%d\nwifi=%d\nwifilist=%s\nbt=%d\nbtnames=%s\n",
                         (unsigned long)slot, (long)ts, (unsigned long)intervalMin,
                         (unsigned)jpgLen, rssi, wifiCount, wifiList.c_str(),
                         btCount, btNames.c_str());
  if (metaLen > (int)sizeof(meta)) metaLen = sizeof(meta); // por si truncara

  char tsha[41];
  bool tHasSha = false;
  if (httpGetSha(txtPath, tsha, sizeof(tsha), &tHasSha)) {
    if (httpPut(txtPath, tsha, tHasSha, (const uint8_t *)meta, metaLen)) {
      Serial.println("latest.txt actualizado OK");
    }
  }
  // Si latest.txt falla no es fatal: la foto ya esta subida; el proximo
  // ciclo vuelve a intentar mover el puntero.

  bootCount++;
  sleepNow();
}

void loop() {
  // No se usa: setup() siempre termina en deep sleep.
}
