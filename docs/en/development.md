# Development

[Project home](../readme/README.en.md) · [Documentation](README.md) · [简体中文](../zh-CN/development.md)

Choose an entry point by the module you need to change. The host service, browser WebUI, and Windows desktop GUI have separate runtime environments. Optional device components also have their own installation and session lifecycles.

## Code structure

| Module | Entry point | Responsibility |
| --- | --- | --- |
| Host service | `src/` | Capture, encoding, audio, input, pairing, and sessions |
| Client extensions | `src/nvhttp/`, `src/rtsp.cpp`, `src/stream.cpp` | Negotiation, control channel, dynamic parameters, displays, and feedback |
| Video and HDR | `src/video*`, `src/hdr/`, `src/platform/windows/display*` | Color, metadata, display targets, and capture |
| Network adaptation | `src/streaming/` | Bitrate and video loss protection |
| Browser WebUI | `src_assets/common/assets/web/` | Configuration, application lists, and browser diagnostics |
| Desktop GUI | `src_assets/common/sunshine-control-panel/` | Device center, local scanning, components, and system operations; a separate repository submodule |
| Device components | `src/platform/windows/ds5/`, `src/remote_usb/`, and others | Optional devices, transports, and process lifecycles |
| Verification | `tests/`, WebUI `tests/`, `.github/workflows/` | Unit tests, integration checks, and builds |

## Get the source

```bash
git clone --recurse-submodules https://github.com/AlkaidLab/foundation-sunshine.git
cd foundation-sunshine
```

For an existing checkout, use `git submodule update --init --recursive` to obtain recorded submodule versions. For coordinated host and GUI changes, record both commits and the packages used.

## Build the host

See the [build instructions](https://github.com/AlkaidLab/foundation-sunshine/blob/master/docs/building.md) for platform dependencies and the [Build and Release workflow](https://github.com/AlkaidLab/foundation-sunshine/blob/master/.github/workflows/main.yml) for official Windows packaging. Generic build examples do not exactly reproduce the official installer pipeline.

For optional components, check package origin, version, capability, and fallback behavior:

- [DualSense component lifecycle (Chinese)](https://github.com/AlkaidLab/foundation-sunshine/blob/master/docs/windows_dualsense_component_lifecycle.md)
- [NVIDIA HDR build](https://github.com/AlkaidLab/foundation-sunshine/blob/master/docs/rtx_hdr_build.md)
- [Paired host and GUI builds](https://github.com/AlkaidLab/foundation-sunshine/blob/master/docs/paired-gui-build.md)

## Experimental Windows Core restart intent

The service distinguishes a console-session migration from a genuine stop using
private inherited manual-reset events. Core validates SYSTEM identity and the
event handles before enabling RTSP restart support. Portable startup has no
restart bridge. A transient absent console session is rechecked, not treated as
a migration.

These v1 protocol allocations match the reviewed common-c definitions in
`AsafMah/moonlight-common-c` commit
`080e04dd3c26bede976a8434ddc3defe8fc4b6e8`:
`SS_TERMINATION_HOST_RESTART_V1=0x53535201`,
`LI_FF_HOST_RESTART=0x800`, `ML_FF_HOST_RESTART=0x100`, and
`LI_HOST_RESTART_PROTOCOL_VERSION=1`. Host and client must both negotiate their
feature bit and `x-ss-restart.protocolVersion:1` /
`x-ml-restart.protocolVersion:1`, with encrypted control. Only an accepted
session receives restart reason bytes `53 53 52 01`; genuine stops and legacy
sessions retain `80 03 00 23`. No packet layout or existing encryption changes.

The owning control thread services ENet for up to 250 ms before session/peer
teardown. This improves delivery but adds no application acknowledgement:
forced termination or packet loss can still appear as generic connection loss.
There is no recovery tombstone or managed-game persistence. Automatic recovery
also requires a restart-aware client and authenticated host-state refresh;
the initial client scope is a fresh launch of the original commandless Desktop.
The host deliberately keeps its constants in `src/streaming/restart_intent.h`;
the client-only `ML_ERROR_HOST_RESTART` is not defined by the host. Ship the
common-c/client support before enabling host emission; the compatible client
commit is `AsafMah/moonlight-qt` `6425ac90b38e52ef5ef7e5f73b0f6610f94ec3c7`
and pins the common-c commit above.

The dedicated `restart_intent_unit_tests` target uses fake service events, a
device-free fake Core child, and loopback ENet. It does not run Sunshine or alter
Windows sessions. Live service/WTS/RDP interoperability requires separate
validation and cannot be inferred from these tests.

## Develop the WebUI

Use the Node.js version in `.node-version` and npm requirements in `package.json`. From the host repository root:

```bash
npm ci
npm run dev-server
```

The development server uses `https://localhost:3000` and needs a reachable Sunshine instance for APIs. Run checks relevant to the interface change:

```bash
npm run lint:webui
npm run test:webui
npm run build
```

For locale changes, also run `npm run i18n:validate`. See the [WebUI development guide (Chinese)](https://github.com/AlkaidLab/foundation-sunshine/blob/master/docs/WEBUI_DEVELOPMENT.md).

## Develop the desktop GUI

The GUI submodule is [sunshine-control-panel](https://github.com/qiin2333/sunshine-control-panel). Install its Node.js, Rust, and Windows native build requirements, then run from that directory:

```bash
npm ci
npm run dev
```

`dev` builds native plugins before starting Tauri. Use `npm run dev:renderer` for renderer development. For host WebUI integration, start `npm run dev-server` in the host repository, then `npm run dev-webui` in the GUI directory.

Regular Windows builds download an available GUI release bundle. For testing an exact host and recorded GUI commit combination, follow [paired build instructions](https://github.com/AlkaidLab/foundation-sunshine/blob/master/docs/paired-gui-build.md). The checkout's GUI submodule version does not automatically identify the GUI inside a release package.

## Verify the change

| Change | What to confirm |
| --- | --- |
| Capture or encoding | Actual capture method, encoder, format, and fallback during a real connection; frame rate and latency when relevant |
| HDR and color | Negotiation, host metadata, client decoding, and display presentation as separate checks |
| Audio or input | Real-device recording, playback, and input feedback, with cleanup after disconnect |
| Components | Installation, unavailable state, repair, removal, and session cleanup |
| WebUI or GUI | Actual flows, browser and desktop differences, and both languages |
| Documentation | Relative links, anchors, language navigation, and usable instructions |

Explain any checks not run. Unit-test success does not establish client, driver, or hardware behavior. See [contributing](../contributing.en.md) for PR contents.
