# ESP32-CAM → GitHub Pages

Sube una foto cada 10 minutos a este repo vía la API de contenidos de
GitHub. `index.html` (servido por GitHub Pages) muestra la foto principal
+ las 4 anteriores, con auto-refresh cada 60 s.

## Cómo mantiene solo 10 fotos

El ESP32 rota entre 10 "ranuras": `fotos/foto0.jpg` … `fotos/foto9.jpg`.
Un contador que sobrevive al deep sleep decide la ranura de cada ciclo, así
que al llegar a 10 vuelve a empezar y **siempre hay exactamente 10 fotos**
en el repo. Además escribe `fotos/latest.txt` con la ranura más nueva, el
timestamp del upload (epoch UTC, vía NTP) y la telemetría del ciclo:

```
slot=3
ts=1737146400
interval=10
size=48231
rssi=-52
wifi=8
wifilist=VecinoA:-67,VTR-56:-71,Claro_2G:-73
bt=6
btnames=JBL Flip 5,MiBand
```

La web usa `slot` para el orden de la galería, `ts`+`interval` para la hora
de la última foto y la cuenta regresiva, y el resto para las tarjetas de
telemetría (WiFi, Bluetooth, tamaño). El estado conectado/desconectado se
deduce: si la última foto llegó dentro del intervalo esperado (+3 min de
margen), está conectado. `wifilist` y `btnames` alimentan los desplegables
"ver redes" / "ver dispositivos" con la lista completa. Por privacidad, la
**red propia (STA_SSID) se excluye** de `wifilist` (la página es pública);
solo aparecen las demás redes detectadas. Si NTP no sincroniza, `ts=0` y la
web cae a una estimación local.

Nota sobre el disco: aunque solo haya 10 archivos, git guarda el historial
de cada subida para siempre. La GitHub Action `prune-history` (ver más abajo)
aplana ese historial periódicamente para que el `.git` no crezca.

## 1. Token de GitHub

1. GitHub → Settings → Developer settings → Fine-grained tokens → Generate new token.
2. Repository access: **Only select repositories** → este repo.
3. Permissions → Repository permissions → **Contents: Read and write**. Nada más.
4. Genera y copia el token (empieza con `github_pat_...`).

## 2. Configurar secrets

`secrets.h` ya existe con el WiFi cargado. Solo falta completar el token y
el repo:

```
#define GH_TOKEN "github_pat_..."
#define GH_USER  "tu-usuario"
#define GH_REPO  "tu-repo"
```

Si empiezas de cero, copia `secrets.h.example` a `secrets.h` y rellena los
4 valores. `secrets.h` está en `.gitignore`, no se sube al repo.

## 3. Arduino IDE

Placa: **AI Thinker ESP32-CAM** (ESP-32S + OV2640), programada vía el
baseboard **ESP32-CAM-MB** (USB-serial, normalmente chip CH340 — instala su
driver si Windows no reconoce el puerto COM).

1. Instala el core **esp32 by Espressif Systems** (Preferencias → URLs
   adicionales de gestor de tarjetas →
   `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
   → Gestor de tarjetas → busca "esp32" → instala).
2. Abre `esp2.ino` (Archivo → Abrir, esta carpeta). `secrets.h` aparecerá
   como pestaña aparte automáticamente.
3. Herramientas → Placa → **AI Thinker ESP32-CAM**.
4. Herramientas → asegúrate de:
   - **PSRAM: Enabled**
   - **Partition Scheme: Huge APP (3MB No OTA/1MB SPIFFS)**
   - **Upload Speed: 115200** (si falla el upload, baja aquí, p.ej. a 57600)
5. Conecta el ESP32-CAM al MB (respeta la serigrafía/orientación del
   conector de 2x2 pines) y el MB al USB. Selecciona el puerto COM
   correcto. Con el MB **no hace falta puentear GPIO0 a GND a mano**: la
   placa lo hace sola al programar. Pulsa Subir.
   - Si se queda en "Connecting..." sin avanzar: mantén presionado el
     botón **IO0** del MB al iniciar la subida y suéltalo apenas empiece
     a transferir.
6. Al terminar la subida, pulsa **RST** en el MB (o desconecta/reconecta
   el USB) para que arranque el sketch normal — algunos MB quedan en modo
   programación hasta el siguiente reset.
7. Monitor Serie a 115200 baudios para ver los logs.

## 4. Activar GitHub Pages

Settings → Pages → Source: rama `main` (o la que uses), carpeta `/ (root)`.
La página quedará en `https://<usuario>.github.io/<repo>/`.

Las primeras fotos aparecen de a una: la galería se llena a medida que el
ESP32 va escribiendo las 10 ranuras (una cada 10 min). Las miniaturas de
ranuras aún vacías se ocultan solas.

## 5. Configurar la cámara desde la web (sin reflashear)

El ESP32 lee `config.txt` (raíz del repo) en cada ciclo, vía
`raw.githubusercontent.com`, y aplica esos valores antes de tomar la foto.
Formato `clave=valor`:

```
framesize=9   # resolución: 5=QVGA 8=VGA 9=SVGA 10=XGA 11=HD 12=SXGA 13=UXGA
quality=12    # JPEG: 10=mejor (grande) … 63=peor (chico)
brightness=0  # -2 … 2
contrast=0    # -2 … 2
saturation=0  # -2 … 2
hmirror=0     # 0/1 espejo horizontal
vflip=0       # 0/1 voltear vertical
interval=10   # minutos entre fotos, 1 … 1440
wbmode=0      # balance de blancos: 0=auto 1=soleado 2=nublado 3=oficina 4=hogar
keepalive=0   # 0/1 modo power bank (despertares cortos anti-apagado)
```

`keepalive=1` evita que un power bank corte la salida por bajo consumo: en
vez de dormir el intervalo entero de una, el ESP32 duerme en tramos de ~20 s
con un pulso de consumo de ~2 s entre medio (sin WiFi ni cámara). Gasta algo
más de batería; actívalo solo si tu banco se apaga solo.

Los cambios se aplican en el **próximo ciclo** (hasta ~10 min, más el caché
de `raw` de unos minutos). Si `config.txt` no existe o falla la lectura, el
firmware usa los defaults y toma la foto igual.

**Editor web:** abre `config.html` (link "⚙ configurar cámara" en la página).
Ajusta los valores, pega tu token fine-grained (Contents: RW) y guarda. El
token queda **solo en tu navegador** (localStorage), nunca se sube al repo;
el editor lo usa para hacer el commit de `config.txt` vía la API de GitHub.
Cualquiera puede abrir la página, pero sin un token válido no puede guardar.

## 6. Podar el historial (opcional pero recomendado)

`.github/workflows/prune-history.yml` aplana todo el historial de git a un
solo commit, para que el `.git` no crezca sin límite. Como la galería lee los
10 archivos (no el historial), podar no rompe nada.

Para activarlo: Settings → Actions → General → Workflow permissions →
**Read and write permissions**. Corre solo cada día (04:17 UTC) y también a
mano desde la pestaña **Actions**.

**Ojo:** borra TODA la historia de commits, incluida la del código fuente.
Si quieres conservar la del código, guárdalo en otro repo o rama. Si no te
importa el tamaño del `.git`, puedes borrar este workflow.
