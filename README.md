# 🌿 Serre Arrosage Automatique — ESP8266 + Home Assistant

Système d'irrigation autonome pour serre 6×3m, piloté par un ESP8266 et supervisé via Home Assistant (MQTT). Fonctionne sans réseau grâce au DS3231, se synchronise via NTP quand le WiFi est disponible.

---

## 📦 Matériel

| Composant | Référence | Rôle |
|-----------|-----------|------|
| Microcontrôleur | ESP8266 NodeMCU / Wemos D1 Mini | Cerveau du système |
| Relais | HW-803 1 canal | Commutation 24VAC |
| Horloge temps réel | DS3231 | Heure autonome (optionnel) |
| Électrovanne | 24VAC 1/2" ou 3/4" | Ouverture/fermeture eau |
| Capteur température | Zigbee (ex: SNZB-02) | Mesure ambiante serre |
| Alimentation ESP | 5V USB ou adaptateur | — |
| Alimentation valve | Transformateur 24VAC | — |

---

## 🔌 Schéma de câblage

```
                    ESP8266 (Wemos D1 Mini)
                   ┌─────────────────────┐
              3.3V │●                   ●│ 5V (VIN)
               GND │●                   ●│ GND
      DS3231 SCL ──│ D3 (GPIO0)  (GPIO5) │── D1 ── Relais IN
      DS3231 SDA ──│ D2 (GPIO4)         │
                   └─────────────────────┘

  DS3231 RTC                    Relais HW-803
  ┌──────────┐                  ┌───────────────┐
  │ VCC ─────┼── 3.3V ESP       │ VCC ──────────┼── 5V ESP (VIN)
  │ GND ─────┼── GND ESP        │ GND ──────────┼── GND ESP
  │ SDA ─────┼── D2 ESP         │ IN  ──────────┼── D1 ESP
  │ SCL ─────┼── D3 ESP         │               │
  └──────────┘                  │ COM ──────────┼── Fil 1 transfo 24VAC
                                │ NO  ──────────┼── Fil 1 électrovanne
                                └───────────────┘

  Circuit 24VAC (ISOLÉ du circuit ESP)
  ┌─────────────────────────────────────────────┐
  │  Transformateur 24VAC                        │
  │  ┌──────────┐                               │
  │  │ Sortie 1 │──── COM (relais)              │
  │  │ Sortie 2 │──── Fil 2 électrovanne        │
  │  └──────────┘                               │
  │                                             │
  │  Électrovanne 24VAC                         │
  │  ┌──────────┐                               │
  │  │ Fil 1    │──── NO (relais)               │
  │  │ Fil 2    │──── Sortie 2 transfo          │
  │  └──────────┘                               │
  └─────────────────────────────────────────────┘

⚠️  NO = Normally Open : relais alimenté UNIQUEMENT pendant l'arrosage
⚠️  Le 24VAC ne doit JAMAIS être connecté au circuit ESP (3.3V/5V)
```

---

## 🧠 Logique d'arrosage

| Déclencheur | Durée | Condition |
|-------------|-------|-----------|
| 09h00 (RTC/NTP) | 15 min | 1× par jour |
| Température > 30°C | 2 min | Cooldown 30 min entre déclenchements |
| Commande manuelle HA | 2 ou 15 min | À tout moment |

**Source d'heure (priorité) :**
1. NTP (`pool.ntp.org`) quand WiFi disponible
2. DS3231 en fallback hors WiFi
3. Si aucun des deux → arrosage température fonctionne, arrosage matin désactivé

---

## 📡 Topics MQTT

| Topic | Direction | Description |
|-------|-----------|-------------|
| `serre/arrosage/status` | ESP → HA | Status JSON complet (retain) |
| `serre/arrosage/relay` | ESP → HA | `ON` / `OFF` (retain) |
| `serre/arrosage/cmd` | HA → ESP | Commandes manuelles |
| `zigbee2mqtt-edge/capteur écran` | HA → ESP | Température Zigbee |

### Payloads MQTT réels

**`serre/arrosage/status`** — publié toutes les 30 secondes :
```json
{
  "status": "online",
  "time": "09:00:12",
  "date": "2026-06-06",
  "time_source": "NTP",
  "temp_c": 28.5,
  "relay": "OFF",
  "relay_reason": "morning_9h",
  "morning_done": true,
  "temp_cooldown": false,
  "ip": "192.168.1.127"
}
```

**`serre/arrosage/relay`** :
```
ON
OFF
```

**`zigbee2mqtt-edge/capteur écran`** — reçu du capteur Zigbee :
```json
{
  "battery": 87,
  "humidity": 62.5,
  "temperature": 31.2,
  "voltage": 2900
}
```

**`serre/arrosage/cmd`** — commandes disponibles :
```
ON_TEMP        → arrosage court 2 min
ON_FULL        → arrosage long 15 min
OFF            → arrêt immédiat
RESET_MORNING  → reset flag arrosage matin
```

**LWT (Last Will Testament)** — publié automatiquement si l'ESP se déconnecte :
```json
{ "status": "offline" }
```

---

## ⚡ Sécurités électriques — Électrovanne 24VAC

### Isolation des circuits
Le circuit 24VAC (transformateur + électrovanne) est **totalement isolé** du circuit basse tension (ESP 3.3V/5V). Le relais HW-803 assure cette séparation galvanique.

```
Réseau 230VAC → Transformateur 24VAC → Relais (côté charge) → Électrovanne
ESP 5V        → Relais (côté commande) → D1 (GPIO5)

Les deux circuits ne se touchent JAMAIS.
```

### Règles de câblage 24VAC

- ✅ Utiliser du câble **2 fils section 0.75mm²** minimum pour le 24VAC
- ✅ Brancher l'électrovanne sur **NO (Normally Open)** — la valve est fermée au repos, le relais n'est alimenté que pendant l'arrosage
- ✅ Le transformateur 24VAC doit être **dimensionné** pour la consommation de l'électrovanne (généralement 25VA)
- ✅ Protéger le transformateur avec un **fusible 1A** côté 230VAC
- ❌ Ne jamais connecter le 24VAC sur **NC (Normally Closed)** — l'électrovanne serait en permanence alimentée
- ❌ Ne jamais alimenter l'ESP depuis le transformateur 24VAC

### Sécurité logicielle
- Au démarrage, le GPIO D1 est forcé **LOW** → relais ouvert → électrovanne fermée
- En cas de perte WiFi/MQTT, l'ESP continue de fonctionner de manière autonome via RTC/NTP
- La durée maximale d'arrosage est codée en dur (15 min) — impossible de rester ouvert indéfiniment par commande MQTT

---

## 🔴 Scénarios de panne

### 1. Perte WiFi
| Symptôme | Comportement |
|----------|-------------|
| WiFi coupé | ESP continue en mode autonome |
| Heure | DS3231 prend le relais (si branché) |
| MQTT | Plus de supervision HA, commandes manuelles impossibles |
| Arrosage matin | ✅ Fonctionne (DS3231 requis) |
| Arrosage température | ❌ Désactivé (plus de données temp.) |

**Résolution** : vérifier le routeur, l'ESP se reconnecte automatiquement au retour du WiFi.

---

### 2. Perte MQTT (broker HA éteint)
| Symptôme | Comportement |
|----------|-------------|
| MQTT déconnecté | ESP tente reconnexion en boucle (sans bloquer) |
| Arrosage matin | ✅ Fonctionne (RTC/NTP indépendant) |
| Arrosage température | ❌ Désactivé (données temp. viennent du MQTT) |
| LWT | HA affiche `offline` après timeout broker |

**Résolution** : redémarrer le broker Mosquitto dans HA.

---

### 3. DS3231 absent ou pile morte
| Symptôme | Comportement |
|----------|-------------|
| RTC hors service | NTP prend le relais si WiFi OK |
| WiFi + RTC absents | Arrosage matin désactivé, temp. fonctionne |
| Date aberrante au boot | Ignorée si > 2100 ou < 2020 |

**Résolution** : remplacer la pile CR2032 du DS3231, ou s'assurer que le WiFi est disponible.

---

### 4. Capteur Zigbee hors ligne
| Symptôme | Comportement |
|----------|-------------|
| Plus de données temp. | `temp_c` reste à `-99.0` |
| Arrosage température | ❌ Désactivé (seuil jamais atteint) |
| Arrosage matin | ✅ Non affecté |

**Résolution** : vérifier la batterie du capteur Zigbee, le rapprocher du coordinateur.

---

### 5. Électrovanne bloquée ouverte (panne mécanique)
| Symptôme | Comportement logiciel |
|----------|----------------------|
| Valve mécaniquement coincée | L'ESP coupe l'alimentation normalement |
| L'eau continue de couler | Problème mécanique, pas électrique |

**Résolution** : fermeture manuelle du robinet d'arrêt en amont, remplacement de l'électrovanne.

---

### 6. ESP ne démarre pas après flash
- Vérifier que **D3 (GPIO0)** n'est pas maintenu LOW au boot (I2C pull-up requis)
- Déconnecter le DS3231 pendant le flash si problème persistant
- Vérifier l'alimentation 5V stable

---

## 📚 Librairies Arduino

```
PubSubClient    → Nick O'Leary       (MQTT)
RTClib          → Adafruit           (DS3231)
ESP8266WiFi     → incluse board ESP8266
ArduinoOTA      → incluse board ESP8266
time.h          → incluse board ESP8266 (NTP)
```

---

## 🗂️ Structure du projet

```
serre-arrosage-esp8266/
├── README.md
├── arduino/
│   └── serre_arrosage.ino          # Code ESP8266 complet
└── homeassistant/
    ├── packages/
    │   └── serre.yaml              # Entités MQTT + automations HA
    └── dashboard/
        └── dashboard_serre.yaml    # Dashboard Lovelace
```

---

## 📜 Licence

MIT — libre d'utilisation, modification et distribution.
