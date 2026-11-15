"""Gate the host's protocol feature bits by value against one allocation table.

Every LI (host-advertised, x-ss-general.featureFlags) and ML (client opt-in, x-ml-general.featureFlags) bit defined in the
host sources or the pinned moonlight-common-c headers must match ALLOCATIONS by value: a value that the table gives to another
meaning is a collision, whatever the constant is called (LI_FF_*, ML_FF_*, platform_caps::*, *_feature). The pinned registry
(docs/protocol-extension-registry.md) must agree with the table too, so an upstream reassignment fails here first.
"""
import argparse
import re
import sys
import unittest
from pathlib import Path

COMMON_C = Path("third-party/moonlight-common-c")
REGISTRY = COMMON_C / "docs/protocol-extension-registry.md"

# The fork's view of both registries: upstream rows, upstream reservations not yet in the pinned registry, and fork allocations.
ALLOCATIONS = {
    "LI": {
        0x1: "PEN_TOUCH", 0x2: "CONTROLLER_TOUCH", 0x4: "CLIPBOARD_TEXT", 0x8: "CLIPBOARD_IMAGE", 0x10: "TOUCHPAD",
        0x20: "TOUCHPAD_FRAME", 0x40: "CURSOR_SHAPE", 0x80: "DS5_HAPTICS_PCM", 0x100: "DYNAMIC_SDR_WHITE",
        0x200: "REMOTE_TEXT_CONTEXT",
        0x400: "CONTROLLER_HAPTICS",  # upstream (qiin), claimed first
        0x800: "HOST_SESSION_STATE",
        0x1000: "HOST_RESTART",  # fork: restart intent v1
        0x2000: "CLIENT_RESOLUTION_CHANGE",  # fork: #729 client-requested resolution changes
        0x4000: "BITRATE_CALLBACK",  # upstream: host-to-client runtime bitrate (0x5506 subtype 2)
        0x8000: "ENDPOINT_AUTO_REFRESH",
        0x10000: "DISPLAY_GROUP",  # fork: reserved for the unreleased multi-screen prototype
    },
    "ML": {
        0x1: "FEC_STATUS", 0x2: "SESSION_ID_V1", 0x4: "DS5_HAPTICS_PCM", 0x8: "DS5_HAPTICS_IR_V2", 0x10: "REMOTE_TEXT_CONTEXT",
        0x20: "PYROWAVE", 0x40: "DS5_HAPTICS_STATE", 0x80: "DS5_HAPTICS_IR_EXTENDED", 0x100: "HOST_SESSION_STATE",
        0x1000: "HOST_RESTART",  # fork: restart intent v1
        0x4000: "BITRATE_CALLBACK",  # upstream
        0x8000: "ENDPOINT_AUTO_REFRESH",
    },
}
# Values the fork release depends on; a change here is a protocol change and must be made on purpose.
PINNED = {("LI", "HOST_RESTART"): 0x1000, ("ML", "HOST_RESTART"): 0x1000, ("LI", "CLIENT_RESOLUTION_CHANGE"): 0x2000,
          ("LI", "BITRATE_CALLBACK"): 0x4000, ("ML", "BITRATE_CALLBACK"): 0x4000,
          ("LI", "ENDPOINT_AUTO_REFRESH"): 0x8000, ("ML", "ENDPOINT_AUTO_REFRESH"): 0x8000}

ROW = re.compile(r"^\|\s*`(?P<ns>LI|ML)_FF_(?P<name>[A-Z0-9_]+)`\s*\|\s*`(?P<value>0x[0-9A-Fa-f]+)`\s*\|", re.M)
PREFIXED = re.compile(
    r"(?:#define\s+(?P<ns1>LI|ML)_FF_(?P<n1>[A-Z0-9_]+)\s+\(?(?P<v1>0x[0-9A-Fa-f]+))"
    r"|(?:constexpr\s+[\w:]+\s+(?P<ns2>LI|ML)_FF_(?P<n2>[A-Z0-9_]+)\s*=\s*(?P<v2>0x[0-9A-Fa-f]+))")
CAPS_BLOCK = re.compile(r"namespace platform_caps\s*\{(?P<body>.*?)\};", re.S)
CAP = re.compile(r"constexpr\s+caps_t\s+(?P<name>\w+)\s*=\s*(?P<value>0x[0-9A-Fa-f]+)")
FEATURE = re.compile(r"constexpr\s+std::uint32_t\s+(?P<name>\w+)_feature\s*=\s*(?P<value>0x[0-9A-Fa-f]+)")


def meaning(name):
    name = name.upper()
    for suffix in ("_EVENTS", "_FEATURE"):
        if name.endswith(suffix):
            name = name[: -len(suffix)]
    return name


def definitions(sources):
    """Yield (where, namespace, meaning, value) for every feature bit definition, whatever its spelling."""
    for path, text in sources:
        for m in PREFIXED.finditer(text):
            ns, name, value = (m["ns1"], m["n1"], m["v1"]) if m["ns1"] else (m["ns2"], m["n2"], m["v2"])
            yield f"{path}: {ns}_FF_{name}", ns, meaning(name), int(value, 16)
        block = CAPS_BLOCK.search(text)
        if block:
            for m in CAP.finditer(block["body"]):
                yield f"{path}: platform_caps::{m['name']}", "LI", meaning(m["name"]), int(m["value"], 16)
        if path.replace("\\", "/").endswith("dynamic_param_protocol.h"):
            for m in FEATURE.finditer(text):
                yield f"{path}: {m['name']}_feature", "ML", meaning(m["name"]), int(m["value"], 16)


def check(registry_text, sources, allocations=ALLOCATIONS, pinned=PINNED):
    problems = []
    by_name = {ns: {v: k for k, v in table.items()} for ns, table in allocations.items()}
    for m in ROW.finditer(registry_text):
        ns, name, value = m["ns"], meaning(m["name"]), int(m["value"], 16)
        if allocations[ns].get(value) != name or by_name[ns].get(name) != value:
            problems.append(f"registry {ns}_FF_{m['name']}=0x{value:X} disagrees with the allocation table "
                            f"(table: 0x{value:X} is {allocations[ns].get(value)}, {name} is {by_name[ns].get(name)})")
    seen = {}
    for where, ns, name, value in definitions(sources):
        owner = allocations[ns].get(value)
        if owner is None:
            problems.append(f"{where}=0x{value:X}: unallocated {ns} bit; reserve it in ALLOCATIONS first")
        elif owner != name:
            problems.append(f"{where}=0x{value:X} collides by value with {ns} {owner}")
        expected = by_name[ns].get(name)
        if expected is not None and expected != value:
            problems.append(f"{where}=0x{value:X} but the table allocates {ns} {name} = 0x{expected:X}")
        seen.setdefault((ns, name), set()).add(value)
    for key, value in pinned.items():
        if seen.get(key) != {value}:
            problems.append(f"{key[0]} {key[1]} must be defined as exactly 0x{value:X} (found {sorted(seen.get(key, []))})")
    return problems


class GateTests(unittest.TestCase):
    ALLOC = {"LI": {0x4000: "BITRATE_CALLBACK", 0x1000: "HOST_RESTART"}, "ML": {0x4000: "BITRATE_CALLBACK", 0x1000: "HOST_RESTART"}}
    PIN = {("LI", "HOST_RESTART"): 0x1000, ("ML", "HOST_RESTART"): 0x1000}

    def test_ml_collision_by_value_under_a_different_spelling(self):
        # the case the name-based gate missed: upstream's bitrate_callback_feature against our restart bit
        sources = [("restart_intent.h", "constexpr std::uint32_t LI_FF_HOST_RESTART = 0x1000;\n"
                                        "constexpr std::uint32_t ML_FF_HOST_RESTART = 0x4000;"),
                   ("src/streaming/dynamic_param_protocol.h", "constexpr std::uint32_t bitrate_callback_feature = 0x4000;")]
        problems = check("", sources, self.ALLOC, self.PIN)
        self.assertTrue(any("ML_FF_HOST_RESTART=0x4000 collides by value with ML BITRATE_CALLBACK" in p for p in problems), problems)

    def test_platform_cap_collision(self):
        sources = [("common.h", "namespace platform_caps {\n constexpr caps_t bitrate_callback = 0x1000;\n};")]
        self.assertTrue(check("", sources, self.ALLOC, {}))

    def test_registry_reassignment_fails(self):
        self.assertTrue(check("| `LI_FF_SOMETHING_ELSE` | `0x1000` | x |\n", [], self.ALLOC, {}))

    def test_unallocated_bit_fails(self):
        self.assertTrue(check("", [("x.h", "#define LI_FF_NEW 0x20000")], self.ALLOC, {}))

    def test_consistent_tree_passes(self):
        sources = [("restart_intent.h", "constexpr std::uint32_t LI_FF_HOST_RESTART = 0x1000;\n"
                                        "constexpr std::uint32_t ML_FF_HOST_RESTART = 0x1000;"),
                   ("src/streaming/dynamic_param_protocol.h", "constexpr std::uint32_t bitrate_callback_feature = 0x4000;"),
                   ("common.h", "namespace platform_caps {\n constexpr caps_t bitrate_callback = 0x4000;\n};")]
        self.assertEqual(check("| `ML_FF_BITRATE_CALLBACK` | `0x4000` | x |", sources, self.ALLOC, self.PIN), [])


def check_tree(root):
    registry = root / REGISTRY
    if not registry.is_file():
        print(f"missing {registry}; initialise the moonlight-common-c submodule", file=sys.stderr)
        return 1
    files = sorted((root / "src").rglob("*")) + sorted((root / COMMON_C / "src").glob("*.h"))
    sources = [(str(p.relative_to(root)), p.read_text(encoding="utf-8", errors="replace"))
               for p in files if p.suffix in {".h", ".hpp", ".cpp", ".c"}]
    problems = check(registry.read_text(encoding="utf-8"), sources)
    for problem in problems:
        print(problem, file=sys.stderr)
    print(f"checked {sum(1 for _ in definitions(sources))} feature-bit definitions: {len(problems)} problem(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path(__file__).resolve().parents[2])
    args, rest = parser.parse_known_args()
    result = unittest.main(argv=[sys.argv[0], *rest], exit=False).result
    sys.exit(1 if not result.wasSuccessful() else check_tree(args.source.resolve()))
