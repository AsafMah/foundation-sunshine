### ░▒▓ コア機能

- **HDR フルチェーン** — デュアルフォーマットエンコード (PQ + HLG)・フレーム単位の GPU 輝度分析・HDR10+ / HDR Vivid 動的メタデータ・完全な静的メタデータ透過
- **仮想ディスプレイ** — [ZakoVDD](https://github.com/qiin2333/zako-vdd) との深い統合・5 種類の画面モード・Named Pipe 通信・マルチクライアント GUID セッション
- **オーディオ強化** — 7.1.4 サラウンド (12ch)・Opus DRED パケットロス回復・継続的オーディオストリーム・リモートマイク・仮想スピーカーのビット深度マッチング
- **エンコード最適化** — NVENC SDK 13.0・AMF QVBR/HQVBR・エンコーダ結果キャッシュ (260x)・適応型ダウンサンプリング・Vulkan エンコーダ
- **コントロールパネル** — Tauri 2 + Vue 3 + Vite・ダークモード・QR ペアリング・リアルタイムモニタリング
- **スマートペアリング** — クライアント個別設定・デバイス能力の自動マッチング・仮想マウスドライバ (vmouse)

### ░▒▓ 技術詳細

<details>
<summary><b>HDR フルチェーン技術方式</b></summary>

#### デュアルフォーマット HDR エンコード：HDR10 (PQ) + HLG 並行サポート

従来のストリーミング方式は HDR10 (PQ) 絶対輝度マッピングのみをサポートしており、端末デバイスの能力不足や輝度パラメータの不一致が発生すると、暗部のディテール損失やハイライトのクリッピングなどの問題が生じます。

そのため、エンコード層に HLG（Hybrid Log-Gamma, ITU-R BT.2100）サポートを追加し、相対輝度マッピングを採用しています：
- **シーン参照型輝度適応**：HLG は相対輝度曲線に基づき、表示端が自身のピーク輝度に応じて自動的にトーンマッピングを行います。低輝度デバイスでは、暗部のディテール保持が PQ よりも大幅に優れています。
- **ハイライト領域のスムーズなロールオフ**：HLG の対数-ガンマ混合伝達関数は、ハイライト領域で段階的なロールオフを提供し、PQ のハードクリッピングによるハイライトの階調断裂を回避します。
- **ネイティブ SDR 後方互換性**：HLG 信号は SDR ディスプレイで標準の BT.709 画面として直接デコード可能で、追加のトーンマッピング処理は不要です。

**フレーム単位の輝度分析と適応型メタデータ生成**

GPU 側にリアルタイム輝度分析モジュールを統合し、Compute Shader を使用して各フレーム画面に対して以下を実行します：
- **MaxFALL / MaxCLL フレーム単位計算**：フレームレベルの最大コンテンツ輝度（MaxCLL）とフレーム平均輝度（MaxFALL）をリアルタイムで統計し、HEVC/AV1 SEI/OBU メタデータに動的に注入します。
- **外れ値ロバストフィルタリング**：パーセンタイル打ち切り戦略を採用し、極端な輝度ピクセル（ハイライトの鏡面反射など）を除去。孤立した高輝度点が全体の輝度基準を引き上げ、画面全体が暗くなるのを防ぎます。
- **フレーム間指数平滑化**：連続フレームの輝度統計値に EMA（指数移動平均）フィルタを適用し、シーン切り替え時のメタデータ急変による輝度フリッカを除去します。

**完全な HDR メタデータ透過**

HDR10 静的メタデータ（Mastering Display Info + Content Light Level）を完全に透過。NVENC / AMF / QSV エンコード出力のビットストリームは、CTA-861 仕様に準拠した完全なカラーボリュームと輝度情報を保持します。

**HDR10+ / HDR Vivid 動的メタデータ注入**

NVENC エンコードパイプラインにおいて、フレーム単位の輝度分析結果に基づき、以下の動的メタデータ SEI を自動生成・注入します：
- **HDR10+ (ST 2094-40)**：シーンレベルの MaxSCL / distribution percentiles / knee point などのトーンマッピング参照を保持。Samsung/Panasonic などの HDR10+ 認定テレビでの精密なトーンマッピングをサポートします。
- **HDR Vivid (CUVA T/UWA 005.3)**：ITU-T T.35 登録の中国超高清映像連盟(CUVA)標準。PQ モードでは絶対輝度トーンマッピング、HLG モードではシーン参照相対輝度トーンマッピングを提供し、国産端末エコシステムをカバーします。

</details>

<details>
<summary><b>仮想ディスプレイ統合</b> (Windows 10 22H2+ が必要)</summary>

[ZakoVDD](https://github.com/qiin2333/zako-vdd) 仮想ディスプレイドライバとの深い統合：
- カスタム解像度とリフレッシュレートのサポート、10-bit HDR 色深度
- **5 種類の画面構成モード**：仮想画面のみ、物理画面のみ、混合モード、ミラーモード、拡張モード
- Named Pipe リアルタイム通信、ストリーミング開始/終了時に仮想ディスプレイを自動生成/破棄
- 各クライアントに VDD セッション（GUID）を個別にバインド、マルチクライアントの高速切り替えをサポート
- 再起動不要のリアルタイム設定変更

</details>

<details>
<summary><b>オーディオ強化</b></summary>

- **7.1.4 サラウンド (12チャンネル)**：Dolby Atmos などの没入型オーディオレイアウトの完全なチャンネルマッピング
- **Opus DRED 深度冗長**：ニューラルネットワークベースのパケットロス回復、100ms 冗長ウィンドウでネットワークジッターをスムーズに補償
- **継続的オーディオストリーム**：中断のないオーディオストリーム、無音時に自動的に無音データを挿入し、オーディオデバイスの繰り返し初期化を回避
- **仮想スピーカーの自動マッチング**：16bit/24bit などのビット深度フォーマットの仮想オーディオデバイスを自動検出・マッチング

</details>

<details>
<summary><b>キャプチャとエンコードの最適化</b></summary>

**キャプチャパイプライン**
- **Gamma-Aware シェーダー**：DXGI ColorSpace に基づき、sRGB / リニア Gamma 色変換を自動選択
- **高品質ダウンサンプリング**：バイキュービック (Bicubic) 補間、fast / balanced / high_quality の 3 段階をサポート
- **動的解像度検出**：ディスプレイの解像度と回転変化をリアルタイムで認識、エンコーダが適応的に調整
- **GPU 輝度分析**：Compute Shader による 2 段階リダクション、P95/P99 打ち切り、フレーム間 EMA 時間領域平滑化

**NVENC**
- **SDK 13.0**：精細なビットレート制御と Look-ahead
- **HDR メタデータ API**：NVENC SDK 12.2+ ネイティブの Mastering Display / Content Light Level 書き込み
- **HDR10+ / HDR Vivid SEI**：フレーム単位で ST 2094-40 および CUVA T.35 動的メタデータを自動生成
- **SPS ビットストリーム仕様**：H.264/HEVC SPS bitstream restrictions を完全に書き込み

**AMF (AMD)**
- **QVBR / HQVBR / HQCBR**：高度なビットレート制御、品質レベルの UI 調整をサポート
- **AV1 低遅延**：AV1 エンコーダの遅延影響のない最適化オプション

**共通**
- **エンコーダ結果キャッシュ**：プローブ結果を永続化、後続接続を 26s → <100ms (260x 高速化)
- **適応型ダウンサンプリング**：バイリニア / バイキュービック / 高品質の 3 段階の解像度スケーリングをサポート、4K ホスト→1080p ストリーミングシナリオに適応
- **Vulkan エンコーダ**：実験的な Vulkan ビデオエンコードサポート
- **ロックフリー証明書チェーン**：`shared_mutex` で mutex を置き換え、TLS キューオーバーヘッドを排除

</details>

<br>

---

### ░▒▓ 推奨クライアント

以下の最適化版 Moonlight クライアントと組み合わせることで、最良の体験が得られます（セット効果発動）

- **PC** — [Moonlight-PC](https://github.com/qiin2333/moonlight-qt)（Windows · macOS · Linux）
- **Android** — [威力加強版](https://github.com/qiin2333/moonlight-vplus) · [王冠版](https://github.com/WACrown/moonlight-android)
- **iOS** — [VoidLink](https://github.com/The-Fried-Fish/VoidLink-previously-moonlight-zwm)
- **HarmonyOS** — [Moonlight V+](https://appgallery.huawei.com/app/detail?id=com.alkaidlab.sdream)

その他のリソース：[awesome-sunshine](https://github.com/LizardByte/awesome-sunshine)

<br>

<details>
<summary><b>░▒▓ システム要件</b></summary>

| コンポーネント | 最小要件 | 4K 推奨 |
|------|----------|---------|
| **GPU** | AMD VCE 1.0+ / Intel VAAPI / NVIDIA NVENC | AMD VCE 3.1+ / Intel HD 510+ / GTX 1080+ |
| **CPU** | Ryzen 3 / Core i3 | Ryzen 5 / Core i5 |
| **RAM** | 4 GB | 8 GB |
| **OS** | Windows 10 22H2+ | Windows 10 22H2+ |
| **ネットワーク** | 5GHz 802.11ac | CAT5e イーサネット |

GPU 互換性：[NVENC](https://developer.nvidia.com/video-encode-and-decode-gpu-support-matrix-new) · [AMD VCE](https://github.com/obsproject/obs-amd-encoder/wiki/Hardware-Support) · [Intel VAAPI](https://www.intel.com/content/www/us/en/developer/articles/technical/linuxmedia-vaapi.html)

</details>

---

### ░▒▓ ドキュメントとサポート

[![Docs](https://img.shields.io/badge/使用ドキュメント-ff69b4?style=flat-square)](https://docs.qq.com/aio/DSGdQc3htbFJjSFdO?p=YTpMj5JNNdB5hEKJhhqlSB) [![LizardByte](https://img.shields.io/badge/LizardByte_ドキュメント-a78bfa?style=flat-square)](https://docs.lizardbyte.dev/projects/sunshine/latest/) [![QQグループ](https://img.shields.io/badge/QQ_交流グループ-38bdf8?style=flat-square)](https://qm.qq.com/cgi-bin/qm/qr?k=5qnkzSaLIrIaU4FvumftZH_6Hg7fUuLD&jump_from=webapi)

雑魚のためにコードを書きたい？ → [![Build](https://img.shields.io/badge/ビルド手順-34d399?style=flat-square)](docs/building.md) [![Config](https://img.shields.io/badge/設定ガイド-fbbf24?style=flat-square)](docs/configuration.md) [![WebUI](https://img.shields.io/badge/WebUI_開発-fb923c?style=flat-square)](docs/WEBUI_DEVELOPMENT.md)

<br>

<div align="center">

「 ░▒▓ 」

<a href="https://github.com/qiin2333/foundation-sunshine/graphs/contributors">
  <img src="https://contrib.rocks/image?repo=qiin2333/foundation-sunshine&max=100" />
</a>

<br>

[![QQグループに参加](https://pub.idqqimg.com/wpa/images/group.png 'QQグループに参加')](https://qm.qq.com/cgi-bin/qm/qr?k=WC2PSZ3Q6Hk6j8U_DG9S7522GPtItk0m&jump_from=webapi&authKey=zVDLFrS83s/0Xg3hMbkMeAqI7xoHXaM3sxZIF/u9JW7qO/D8xd0npytVBC2lOS+z)

[![Star History Chart](https://api.star-history.com/svg?repos=qiin2333/Sunshine-Foundation&type=Date)](https://www.star-history.com/#qiin2333/Sunshine-Foundation&Date)

</div>