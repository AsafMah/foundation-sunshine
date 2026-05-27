### ░▒▓ Core Features

- **Full HDR Pipeline** — Dual format encoding (PQ + HLG)・Per-frame GPU luminance analysis・HDR10+ / HDR Vivid dynamic metadata・Complete static metadata passthrough
- **Virtual Display** — Deep integration with [ZakoVDD](https://github.com/qiin2333/zako-vdd)・5 screen modes・Named Pipe communication・Multi-client GUID sessions
- **Audio Enhancement** — 7.1.4 surround sound (12ch)・Opus DRED packet loss recovery・Continuous audio stream・Remote microphone・Virtual speaker bit depth matching
- **Encoding Optimization** — NVENC SDK 13.0・AMF QVBR/HQVBR・Encoder result caching (260x)・Adaptive downscaling・Vulkan encoder
- **Control Panel** — Tauri 2 + Vue 3 + Vite・Dark mode・QR pairing・Real-time monitoring
- **Smart Pairing** — Client-specific configuration・Automatic device capability matching・Virtual mouse driver (vmouse)

### ░▒▓ Technical Details

<details>
<summary><b>Full HDR Pipeline Technical Solution</b></summary>

#### Dual Format HDR Encoding: HDR10 (PQ) + HLG Parallel Support

Traditional streaming solutions only support HDR10 (PQ) absolute luminance mapping. When the terminal device's capabilities are insufficient or luminance parameters don't match, issues like loss of shadow detail and highlight clipping can occur.

Therefore, HLG (Hybrid Log-Gamma, ITU-R BT.2100) support has been added at the encoding layer, using relative luminance mapping:
- **Scene-referenced Luminance Adaptation**: HLG is based on a relative luminance curve. The display side automatically performs tone mapping according to its own peak luminance. Shadow detail retention is significantly better than PQ on low-luminance devices.
- **Smooth Highlight Roll-off**: The logarithmic-gamma hybrid transfer function of HLG provides a gradual roll-off in highlight areas, avoiding the highlight color banding caused by PQ hard clipping.
- **Native SDR Backward Compatibility**: HLG signals can be directly decoded by SDR displays as standard BT.709 images, requiring no additional tone mapping processing.

**Per-frame Luminance Analysis and Adaptive Metadata Generation**

A real-time luminance analysis module is integrated on the GPU side, executing the following on each frame via Compute Shader:
- **MaxFALL / MaxCLL Per-frame Calculation**: Real-time statistics of frame-level Maximum Content Light Level (MaxCLL) and Maximum Frame Average Light Level (MaxFALL), dynamically injecting HEVC/AV1 SEI/OBU metadata.
- **Robust Outlier Filtering**: Uses a percentile truncation strategy to filter out extreme luminance pixels (e.g., specular highlights), preventing isolated bright pixels from raising the global luminance reference and causing the overall image to appear darker.
- **Inter-frame Exponential Smoothing**: Applies EMA (Exponential Moving Average) filtering to the luminance statistics of consecutive frames, eliminating luminance flicker caused by abrupt metadata changes during scene transitions.

**Complete HDR Metadata Passthrough**

HDR10 static metadata (Mastering Display Info + Content Light Level) is fully passed through. The bitstream output by NVENC / AMF / QSV encoders carries complete color volume and luminance information conforming to the CTA-861 specification.

**HDR10+ / HDR Vivid Dynamic Metadata Injection**

Within the NVENC encoding pipeline, based on the per-frame luminance analysis results, the following dynamic metadata SEI is automatically generated and injected:
- **HDR10+ (ST 2094-40)**: Carries scene-level MaxSCL / distribution percentiles / knee point and other tone mapping references, supporting precise tone mapping on HDR10+ certified TVs from Samsung, Panasonic, etc.
- **HDR Vivid (CUVA T/UWA 005.3)**: A China Ultra HD Video Alliance (CUVA) standard registered under ITU-T T.35. Provides absolute luminance tone mapping in PQ mode and scene-referenced relative luminance tone mapping in HLG mode, covering the domestic terminal ecosystem.

</details>

<details>
<summary><b>Virtual Display Integration</b> (Requires Windows 10 22H2+)</summary>

Deep integration with the [ZakoVDD](https://github.com/qiin2333/zako-vdd) virtual display driver:
- Custom resolution and refresh rate support, 10-bit HDR color depth
- **5 Screen Combination Modes**: Virtual only, Physical only, Hybrid, Mirror, Extend
- Named Pipe real-time communication, automatically creates/destroys virtual display at stream start/end
- Each client independently binds a VDD session (GUID), supporting fast multi-client switching
- Real-time configuration changes without reboot

</details>

<details>
<summary><b>Audio Enhancement</b></summary>

- **7.1.4 Surround Sound (12 channels)**: Complete channel mapping for immersive audio layouts like Dolby Atmos
- **Opus DRED Deep Redundancy**: Neural network-based packet loss recovery, 100ms redundancy window smoothly compensates during network jitter
- **Continuous Audio Stream**: Uninterrupted audio stream, automatically fills silence data when no audio is present, preventing repeated audio device initialization
- **Virtual Speaker Auto-matching**: Automatically detects and matches virtual audio devices with bit depths like 16bit/24bit

</details>

<details>
<summary><b>Capture and Encoding Optimization</b></summary>

**Capture Pipeline**
- **Gamma-Aware Shaders**: Automatically selects sRGB / Linear Gamma color conversion based on DXGI ColorSpace
- **High-Quality Downscaling**: Bicubic interpolation, supports fast / balanced / high_quality three levels
- **Dynamic Resolution Detection**: Real-time awareness of monitor resolution and rotation changes, encoder adapts automatically
- **GPU Luminance Analysis**: Compute Shader two-stage reduction, P95/P99 truncation, inter-frame EMA temporal smoothing

**NVENC**
- **SDK 13.0**: Fine-grained rate control and Look-ahead
- **HDR Metadata API**: Native Mastering Display / Content Light Level writing via NVENC SDK 12.2+
- **HDR10+ / HDR Vivid SEI**: Per-frame automatic generation of ST 2094-40 and CUVA T.35 dynamic metadata
- **SPS Bitstream Compliance**: Complete writing of H.264/HEVC SPS bitstream restrictions

**AMF (AMD)**
- **QVBR / HQVBR / HQCBR**: Advanced rate control, supports quality level UI adjustment
- **AV1 Low Latency**: Optimization options for AV1 encoder without latency impact

**General**
- **Encoder Result Caching**: Probe results are persisted, subsequent connections 26s → <100ms (260x speedup)
- **Adaptive Downscaling**: Supports Bilinear / Bicubic / High Quality three levels of resolution scaling, suitable for 4K host → 1080p streaming scenarios
- **Vulkan Encoder**: Experimental Vulkan video encoding support
- **Lock-free Certificate Chain**: `shared_mutex` replaces mutex, eliminating TLS queue overhead

</details>

<br>

---

### ░▒▓ Recommended Clients

Pair with the following optimized Moonlight clients for the best experience (activate the set bonus)

- **PC** — [Moonlight-PC](https://github.com/qiin2333/moonlight-qt) (Windows · macOS · Linux)
- **Android** — [Power Plus Edition](https://github.com/qiin2333/moonlight-vplus) · [Crown Edition](https://github.com/WACrown/moonlight-android)
- **iOS** — [VoidLink](https://github.com/The-Fried-Fish/VoidLink-previously-moonlight-zwm)
- **HarmonyOS** — [Moonlight V+](https://appgallery.huawei.com/app/detail?id=com.alkaidlab.sdream)

More resources: [awesome-sunshine](https://github.com/LizardByte/awesome-sunshine)

<br>

<details>
<summary><b>░▒▓ System Requirements</b></summary>

| Component | Minimum | 4K Recommended |
|------|----------|---------|
| **GPU** | AMD VCE 1.0+ / Intel VAAPI / NVIDIA NVENC | AMD VCE 3.1+ / Intel HD 510+ / GTX 1080+ |
| **CPU** | Ryzen 3 / Core i3 | Ryzen 5 / Core i5 |
| **RAM** | 4 GB | 8 GB |
| **OS** | Windows 10 22H2+ | Windows 10 22H2+ |
| **Network** | 5GHz 802.11ac | CAT5e Ethernet |

GPU Compatibility: [NVENC](https://developer.nvidia.com/video-encode-and-decode-gpu-support-matrix-new) · [AMD VCE](https://github.com/obsproject/obs-amd-encoder/wiki/Hardware-Support) · [Intel VAAPI](https://www.intel.com/content/www/us/en/developer/articles/technical/linuxmedia-vaapi.html)

</details>

---

### ░▒▓ Documentation & Support

[![Docs](https://img.shields.io/badge/Usage_Docs-ff69b4?style=flat-square)](https://docs.qq.com/aio/DSGdQc3htbFJjSFdO?p=YTpMj5JNNdB5hEKJhhqlSB) [![LizardByte](https://img.shields.io/badge/LizardByte_Docs-a78bfa?style=flat-square)](https://docs.lizardbyte.dev/projects/sunshine/latest/) [![QQ Group](https://img.shields.io/badge/QQ_Group-38bdf8?style=flat-square)](https://qm.qq.com/cgi-bin/qm/qr?k=5qnkzSaLIrIaU4FvumftZH_6Hg7fUuLD&jump_from=webapi)

Want to help a noob write code? → [![Build](https://img.shields.io/badge/Build_Guide-34d399?style=flat-square)](docs/building.md) [![Config](https://img.shields.io/badge/Config_Guide-fbbf24?style=flat-square)](docs/configuration.md) [![WebUI](https://img.shields.io/badge/WebUI_Dev-fb923c?style=flat-square)](docs/WEBUI_DEVELOPMENT.md)

<br>

<div align="center">

「 ░▒▓ 」

<a href="https://github.com/qiin2333/foundation-sunshine/graphs/contributors">
  <img src="https://contrib.rocks/image?repo=qiin2333/foundation-sunshine&max=100" />
</a>

<br>

[![Join QQ Group](https://pub.idqqimg.com/wpa/images/group.png 'Join QQ Group')](https://qm.qq.com/cgi-bin/qm/qr?k=WC2PSZ3Q6Hk6j8U_DG9S7522GPtItk0m&jump_from=webapi&authKey=zVDLFrS83s/0Xg3hMbkMeAqI7xoHXaM3sxZIF/u9JW7qO/D8xd0npytVBC2lOS+z)

[![Star History Chart](https://api.star-history.com/svg?repos=qiin2333/Sunshine-Foundation&type=Date)](https://www.star-history.com/#qiin2333/Sunshine-Foundation&Date)

</div>