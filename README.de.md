### ░▒▓ Kernfunktionen

- **HDR Vollkette** — Dual-Format-Codierung (PQ + HLG)・Frame-basierte GPU-Helligkeitsanalyse・HDR10+ / HDR Vivid dynamische Metadaten・Vollständige statische Metadaten-Durchleitung
- **Virtueller Monitor** — Tiefe Integration von [ZakoVDD](https://github.com/qiin2333/zako-vdd)・5 Bildschirmmodi・Named Pipe Kommunikation・Multi-Client GUID-Sitzungen
- **Audio-Verbesserungen** — 7.1.4 Surround Sound (12ch)・Opus DRED Paketverlustwiederherstellung・Kontinuierlicher Audiostream・Remote-Mikrofon・Virtuelle Lautsprecher-Bittiefenanpassung
- **Codierungsoptimierungen** — NVENC SDK 13.0・AMF QVBR/HQVBR・Encoder-Ergebnis-Cache (260x)・Adaptives Downsampling・Vulkan Encoder
- **Steuerungszentrale** — Tauri 2 + Vue 3 + Vite・Dark Mode・QR-Pairing・Echtzeit-Überwachung
- **Intelligentes Pairing** — Client-unabhängige Konfiguration・Automatische Anpassung an Gerätefähigkeiten・Virtueller Maus-Treiber (vmouse)

### ░▒▓ Technische Details

<details>
<summary><b>HDR Vollkette Technisches Konzept</b></summary>

#### Dual-Format HDR-Codierung: Parallele Unterstützung für HDR10 (PQ) + HLG

Traditionelle Streaming-Lösungen unterstützen nur HDR10 (PQ) absolute Helligkeitszuordnung. Wenn die Fähigkeiten des Endgeräts unzureichend sind oder die Helligkeitsparameter nicht übereinstimmen, treten Probleme wie Detailverlust in Schattenbereichen und abgeschnittene Lichter auf.

Daher wurde auf der Codierungsebene Unterstützung für HLG (Hybrid Log-Gamma, ITU-R BT.2100) hinzugefügt, das eine relative Helligkeitszuordnung verwendet:
- **Szenenbezogene Helligkeitsanpassung**: HLG basiert auf einer relativen Helligkeitskurve. Das Anzeigegerät führt automatisch eine Tonwertzuordnung basierend auf seiner eigenen Spitzenhelligkeit durch. Auf Geräten mit geringer Helligkeit ist die Detailerhaltung in Schattenbereichen deutlich besser als bei PQ.
- **Sanftes Ausrollen in hellen Bereichen**: Die logarithmisch-gamma-gemischte Übertragungsfunktion von HLG bietet ein progressives Ausrollen in hellen Bereichen und vermeidet die harten Farbabrisse in hellen Bereichen, die durch das harte Abschneiden von PQ verursacht werden.
- **Natürliche SDR-Rückwärtskompatibilität**: HLG-Signale können direkt von SDR-Displays als Standard-BT.709-Bild decodiert werden, ohne dass eine zusätzliche Tonwertzuordnung erforderlich ist.

**Frame-basierte Helligkeitsanalyse und adaptive Metadatengenerierung**

Auf der GPU-Seite ist ein Echtzeit-Helligkeitsanalysemodul integriert, das über Compute Shader für jedes Bild Folgendes ausführt:
- **MaxFALL / MaxCLL Frame-basierte Berechnung**: Echtzeit-Statistik der maximalen Inhaltshelligkeit (MaxCLL) und der durchschnittlichen Frame-Helligkeit (MaxFALL) pro Frame, dynamische Injektion in HEVC/AV1 SEI/OBU Metadaten
- **Robuste Ausreißerfilterung**: Verwendung einer Perzentil-Abschneidestrategie, um extreme Helligkeitspixel (z. B. Spiegelungen in hellen Bereichen) zu entfernen und zu verhindern, dass isolierte helle Pixel die globale Helligkeitsreferenz anheben und das Gesamtbild abdunkeln.
- **Exponentielle Glättung zwischen Frames**: Anwendung eines EMA-Filters (Exponential Moving Average) auf die Helligkeitsstatistiken aufeinanderfolgender Frames, um Helligkeitsflimmern zu vermeiden, das durch plötzliche Metadatenänderungen bei Szenenwechseln verursacht wird.

**Vollständige HDR-Metadaten-Durchleitung**

HDR10 statische Metadaten (Mastering Display Info + Content Light Level) werden vollständig durchgeleitet. Die von NVENC / AMF / QSV codierten Bitströme tragen vollständige Farbvolumen- und Helligkeitsinformationen gemäß der CTA-861-Spezifikation.

**HDR10+ / HDR Vivid Dynamische Metadaten-Injektion**

In der NVENC-Codierungspipeline werden basierend auf den Ergebnissen der Frame-basierten Helligkeitsanalyse automatisch die folgenden dynamischen Metadaten-SEIs generiert und injiziert:
- **HDR10+ (ST 2094-40)**: Trägt szenenbezogene MaxSCL / Verteilungsperzentile / Knee-Point usw. als Tonwertzuordnungsreferenz und unterstützt präzise Tonwertzuordnung auf HDR10+-zertifizierten Fernsehern von Samsung/Panasonic usw.
- **HDR Vivid (CUVA T/UWA 005.3)**: ITU-T T.35 registrierter Standard der China Ultra HD Video Alliance (CUVA). Bietet absolute Helligkeitstonwertzuordnung im PQ-Modus und szenenbezogene relative Helligkeitstonwertzuordnung im HLG-Modus und deckt das inländische Terminal-Ökosystem ab.

</details>

<details>
<summary><b>Integration des virtuellen Monitors</b> (erfordert Windows 10 22H2+)</summary>

Tiefe Integration des [ZakoVDD](https://github.com/qiin2333/zako-vdd) virtuellen Monitortreibers:
- Unterstützung für benutzerdefinierte Auflösungen und Bildwiederholraten, 10-Bit HDR-Farbtiefe
- **5 Bildschirmkombinationsmodi**: Nur virtuell, Nur physisch, Hybrid, Spiegelung, Erweitert
- Named Pipe Echtzeitkommunikation, automatische Erstellung/Zerstörung des virtuellen Monitors bei Start/Ende des Streamings
- Jeder Client ist unabhängig an eine VDD-Sitzung (GUID) gebunden, unterstützt schnelles Umschalten zwischen mehreren Clients
- Echtzeit-Konfigurationsänderungen ohne Neustart

</details>

<details>
<summary><b>Audio-Verbesserungen</b></summary>

- **7.1.4 Surround Sound (12 Kanäle)**: Vollständige Kanalzuordnung für immersive Audio-Layouts wie Dolby Atmos
- **Opus DRED Tiefenredundanz**: Neuronale Netzwerk-basierte Paketverlustwiederherstellung, 100ms Redundanzfenster gleicht Netzwerkschwankungen aus
- **Kontinuierlicher Audiostream**: Unterbrechungsfreier Audiostream, füllt Stille bei Stille automatisch auf, um wiederholte Initialisierung von Audiogeräten zu vermeiden
- **Automatische Anpassung des virtuellen Lautsprechers**: Automatische Erkennung und Anpassung virtueller Audiogeräte an Bittiefenformate wie 16bit/24bit

</details>

<details>
<summary><b>Erfassungs- und Codierungsoptimierungen</b></summary>

**Erfassungspipeline**
- **Gamma-bewusste Shader**: Automatische Auswahl der sRGB / linearen Gamma-Farbkonvertierung basierend auf dem DXGI ColorSpace
- **Hochwertiges Downsampling**: Bikubische Interpolation, unterstützt drei Stufen: fast / balanced / high_quality
- **Dynamische Auflösungserkennung**: Echtzeiterkennung von Monitorauflösung und -rotation, adaptive Anpassung des Encoders
- **GPU-Helligkeitsanalyse**: Compute Shader zweistufige Reduktion, P95/P99-Abschneidung, EMA-Zeitglättung zwischen Frames

**NVENC**
- **SDK 13.0**: Feinkörnige Bitratensteuerung und Look-ahead
- **HDR-Metadaten API**: Native Mastering Display / Content Light Level Schreibunterstützung ab NVENC SDK 12.2+
- **HDR10+ / HDR Vivid SEI**: Automatische Frame-basierte Generierung von ST 2094-40 und CUVA T.35 dynamischen Metadaten
- **SPS-Bitstrom-Spezifikation**: Vollständige Schreibunterstützung für H.264/HEVC SPS Bitstream Restrictions

**AMF (AMD)**
- **QVBR / HQVBR / HQCBR**: Erweiterte Bitratensteuerung, unterstützt UI-Anpassung der Qualitätsstufe
- **AV1 Geringe Latenz**: Optimierte Optionen für den AV1-Encoder ohne Latenzeinfluss

**Allgemein**
- **Encoder-Ergebnis-Cache**: Dauerhafte Speicherung der Erkennungsergebnisse, nachfolgende Verbindungen 26s → <100ms (260x Beschleunigung)
- **Adaptives Downsampling**: Unterstützt drei Stufen der Auflösungsskalierung (bilinear / bikubisch / hochwertig), geeignet für Szenarien wie 4K-Quelle → 1080p-Streaming
- **Vulkan Encoder**: Experimentelle Unterstützung für Vulkan Video-Codierung
- **Sperrfreie Zertifikatskette**: `shared_mutex` ersetzt mutex, eliminiert TLS-Warteschlangen-Overhead

</details>

<br>

---

### ░▒▓ Empfohlene Clients

Kombinieren Sie die folgenden optimierten Moonlight-Clients für das beste Erlebnis (Aktivierung des Set-Bonus)

- **PC** — [Moonlight-PC](https://github.com/qiin2333/moonlight-qt) (Windows · macOS · Linux)
- **Android** — [Power Enhanced Edition](https://github.com/qiin2333/moonlight-vplus) · [Crown Edition](https://github.com/WACrown/moonlight-android)
- **iOS** — [VoidLink](https://github.com/The-Fried-Fish/VoidLink-previously-moonlight-zwm)
- **HarmonyOS** — [Moonlight V+](https://appgallery.huawei.com/app/detail?id=com.alkaidlab.sdream)

Weitere Ressourcen: [awesome-sunshine](https://github.com/LizardByte/awesome-sunshine)

<br>

<details>
<summary><b>░▒▓ Systemanforderungen</b></summary>

| Komponente | Minimum | 4K Empfohlen |
|------------|---------|--------------|
| **GPU** | AMD VCE 1.0+ / Intel VAAPI / NVIDIA NVENC | AMD VCE 3.1+ / Intel HD 510+ / GTX 1080+ |
| **CPU** | Ryzen 3 / Core i3 | Ryzen 5 / Core i5 |
| **RAM** | 4 GB | 8 GB |
| **System** | Windows 10 22H2+ | Windows 10 22H2+ |
| **Netzwerk** | 5GHz 802.11ac | CAT5e Ethernet |

GPU-Kompatibilität: [NVENC](https://developer.nvidia.com/video-encode-and-decode-gpu-support-matrix-new) · [AMD VCE](https://github.com/obsproject/obs-amd-encoder/wiki/Hardware-Support) · [Intel VAAPI](https://www.intel.com/content/www/us/en/developer/articles/technical/linuxmedia-vaapi.html)

</details>

---

### ░▒▓ Dokumentation & Support

[![Docs](https://img.shields.io/badge/Benutzerdokumentation-ff69b4?style=flat-square)](https://docs.qq.com/aio/DSGdQc3htbFJjSFdO?p=YTpMj5JNNdB5hEKJhhqlSB) [![LizardByte](https://img.shields.io/badge/LizardByte_Dokumentation-a78bfa?style=flat-square)](https://docs.lizardbyte.dev/projects/sunshine/latest/) [![QQ-Gruppe](https://img.shields.io/badge/QQ_Community-38bdf8?style=flat-square)](https://qm.qq.com/cgi-bin/qm/qr?k=5qnkzSaLIrIaU4FvumftZH_6Hg7fUuLD&jump_from=webapi)

Möchten Sie beim Code mithelfen? → [![Build](https://img.shields.io/badge/Build-Anleitung-34d399?style=flat-square)](docs/building.md) [![Config](https://img.shields.io/badge/Konfigurationsleitfaden-fbbf24?style=flat-square)](docs/configuration.md) [![WebUI](https://img.shields.io/badge/WebUI_Entwicklung-fb923c?style=flat-square)](docs/WEBUI_DEVELOPMENT.md)

<br>

<div align="center">

「 ░▒▓ 」

<a href="https://github.com/qiin2333/foundation-sunshine/graphs/contributors">
  <img src="https://contrib.rocks/image?repo=qiin2333/foundation-sunshine&max=100" />
</a>

<br>

[![QQ-Gruppe beitreten](https://pub.idqqimg.com/wpa/images/group.png 'QQ-Gruppe beitreten')](https://qm.qq.com/cgi-bin/qm/qr?k=WC2PSZ3Q6Hk6j8U_DG9S7522GPtItk0m&jump_from=webapi&authKey=zVDLFrS83s/0Xg3hMbkMeAqI7xoHXaM3sxZIF/u9JW7qO/D8xd0npytVBC2lOS+z)

[![Star History Chart](https://api.star-history.com/svg?repos=qiin2333/Sunshine-Foundation&type=Date)](https://www.star-history.com/#qiin2333/Sunshine-Foundation&Date)

</div>
```