# Control de Relé con ESP32-C2 y Blynk via MQTT

Proyecto IoT para controlar un relé desde cualquier lugar usando la app Blynk.
Desarrollado sobre ESP-IDF 5.5 sin Arduino.

---

## Hardware usado

- ESPC2-12 DevKit CozyLife (ESP32-C2, cristal 26MHz, flash 2MB)
- Relé 5V DC / 10A 120-220VAC (5 pines)
- PC817 optoacoplador
- Transistor 2N2222
- Diodo 1N4007
- 2x LED (estado relé y conexión WiFi/Blynk)
- 1x Resistencia 200Ω
- 3x Resistencia 1kΩ

---

## Esquema de conexión

### ESP32-C2 → PC817 (aislamiento)
```
GPIO5 → R200Ω → Pin 1 PC817 (ánodo)
GND   →         Pin 2 PC817 (cátodo)
```

### PC817 → 2N2222 (amplificación)
```
Pin 3 PC817 (emisor)   → GND
Pin 4 PC817 (colector) → R1kΩ → 5V   (pull-up)
Pin 4 PC817 (colector) → Base 2N2222  (mismo punto)
```

### 2N2222 → Relé
```
Emisor 2N2222   → GND
Colector 2N2222 → Pin IN del relé
```

### Relé
```
VCC → 5V
GND → GND
IN  → Colector 2N2222
COM → cable común de la carga
NO  → otro cable de la carga
```

### Diodo 1N4007 (protección bobina)
```
Cátodo (banda) → VCC relé (5V)
Ánodo          → GND relé
```

### LEDs
```
GPIO10 → R1kΩ → LED estado relé → GND
GPIO18 → R1kΩ → LED conexión WiFi/Blynk → GND
```

### GND común
```
GND ESP32 + Pin2 PC817 + Pin3 PC817 + Emisor 2N2222 + GND relé
```

---

## Configuración del entorno

### Requisitos
- Windows 10/11
- ESP-IDF 5.5.3 instalado (incluye Python 3.11 y Git)
- Cuenta en Blynk (plan gratuito funciona)

### Instalación ESP-IDF
Descarga el instalador oficial desde:
https://dl.espressif.com/dl/esp-idf/

Durante la instalación se crea acceso directo a **ESP-IDF 5.5 PowerShell** en el menú inicio. Usar siempre ese entorno para compilar y flashear.

---

## Configuración del proyecto

### 1. Clonar el repositorio
```powershell
git clone https://github.com/henrycifuentesserrano/blynk-rele-esp32c2.git
cd blynk-rele-esp32c2
```

### 2. Configurar el target
El ESPC2-12 de CozyLife usa cristal de 26MHz. Esto es crítico — sin este paso el WiFi no funciona.
```powershell
idf.py set-target esp32c2
idf.py menuconfig
```

Dentro del menuconfig:
- **Component config → Hardware Settings → Main XTAL frequency → 26 MHz**
- **Blynk Rele Configuration → WiFi SSID** → nombre de tu red 2.4GHz
- **Blynk Rele Configuration → WiFi Password** → contraseña
- **Blynk Rele Configuration → Blynk Auth Token** → token de tu dispositivo Blynk
- **Blynk Rele Configuration → Blynk Template ID** → ID de tu plantilla Blynk

Guardar con `S`, salir con `Q`.

### 3. Compilar y flashear
Identificar el puerto COM en el Administrador de dispositivos (Puertos COM y LPT).
```powershell
idf.py build flash monitor -p COM5 --monitor-baud 74880
```

El `74880` es la velocidad correcta para el cristal de 26MHz del ESPC2-12.

---

## Configuración en Blynk

1. Crear cuenta en https://blynk.io
2. Crear nuevo template con nombre "Alarma"
3. Agregar datastream: Virtual Pin V0, nombre "Rele", tipo Integer, min 0, max 1
4. Crear dashboard con un widget Button, asociarlo al datastream "Rele", modo Switch
5. Crear dispositivo desde el template y copiar el Auth Token

---

## Funcionamiento

| Evento | LED GPIO10 | LED GPIO18 | Relé |
|--------|-----------|-----------|------|
| Arranca | OFF | OFF | OFF |
| Conectado a WiFi/Blynk | OFF | ON | OFF |
| Botón ON en app | ON | ON | ON |
| Botón OFF en app | OFF | ON | OFF |
| Sin conexión | OFF | OFF | último estado |

---

## Notas importantes

- La red WiFi debe ser **2.4GHz**. El ESP32-C2 no soporta 5GHz.
- El cristal de **26MHz** es específico del ESPC2-12 CozyLife. Otros módulos ESP32-C2 pueden usar 40MHz.
- El relé siempre **inicia en OFF** al encender el dispositivo.
- Las credenciales WiFi y Blynk se configuran via `menuconfig` y no se suben al repositorio.
- El PC817 aísla eléctricamente el ESP32 del circuito del relé, protegiendo el microcontrolador.

---

## Estructura del proyecto
```
blynk-rele-esp32c2/
├── main/
│   ├── blynk_rele_main.c     # código principal
│   ├── CMakeLists.txt
│   ├── Kconfig.projbuild     # menú de configuración
│   └── idf_component.yml
├── CMakeLists.txt
├── README.md
├── sdkconfig.defaults
└── .gitignore
```

---

## Autor

Henry Cifuentes Serrano  
[github.com/henrycifuentesserrano](https://github.com/henrycifuentesserrano)
