# 🌿 Serre Arrosage — ESP8266 + Home Assistant

Système d'arrosage automatique pour serre 6×3m basé sur ESP8266, intégré à Home Assistant via MQTT.

## Matériel
- ESP8266 (NodeMCU / Wemos D1 Mini)
- Relais HW-803 1 canal
- RTC DS3231 (optionnel — fallback NTP)
- Électrovanne 24VAC
- Capteur température Zigbee (via zigbee2mqtt → HA → MQTT)

## Brochage
| Pin | Fonction |
|-----|----------|
| D1 (GPIO5) | Relais IN |
| D2 (GPIO4) | DS3231 SDA |
| D3 (GPIO0) | DS3231 SCL |

## Logique d'arrosage
- **09h00** : arrosage de 15 min (tous les jours)
- **Temp > 30°C** : arrosage de 2 min (cooldown 30 min)
- Mode autonome si WiFi absent (nécessite DS3231)

## Heure
- **Priorité 1** : NTP (`pool.ntp.org`)
- **Priorité 2** : DS3231 (fallback hors WiFi)

## Topics MQTT
| Topic | Direction | Description |
|-------|-----------|-------------|
| `serre/arrosage/status` | ESP → HA | Status JSON complet |
| `serre/arrosage/relay` | ESP → HA | ON / OFF |
| `serre/arrosage/cmd` | HA → ESP | Commandes manuelles |
| `zigbee2mqtt-edge/capteur écran` | HA → ESP | Température Zigbee |

## Commandes MQTT
| Payload | Action |
|---------|--------|
| `ON_TEMP` | Arrosage court 2 min |
| `ON_FULL` | Arrosage long 15 min |
| `OFF` | Arrêt immédiat |
| `RESET_MORNING` | Reset flag arrosage matin |

## Librairies Arduino
- `PubSubClient` — Nick O'Leary
- `RTClib` — Adafruit
- `ESP8266WiFi` — incluse board ESP8266

## Structure du projet
```
serre-arrosage-esp8266/
├── arduino/
│   └── serre_arrosage.ino       # Code ESP8266
└── homeassistant/
    ├── packages/
    │   └── serre.yaml           # Package HA (entités + automations)
    └── dashboard/
        └── dashboard_serre.yaml # Dashboard Lovelace
```
