"""Gate host protocol feature bits against the pinned common-c allocation registry."""
import argparse
import re
import sys
import unittest
from pathlib import Path

REGISTRY = Path("third-party/moonlight-common-c/docs/protocol-extension-registry.md")
CAPS_HEADER = Path("src/platform/common.h")
ROW = re.compile(r"^\|\s*`(?P<name>(?:LI|ML)_FF_[A-Z0-9_]+)`\s*\|\s*`(?P<value>0x[0-9A-Fa-f]+)`\s*\|", re.M)
DEFINITION = re.compile(
    r"(?:#define\s+(?P<d_name>(?:LI|ML)_FF_[A-Z0-9_]+)\s+(?P<d_value>0x[0-9A-Fa-f]+))"
    r"|(?:constexpr\s+[\w:]+\s+(?P<c_name>(?:LI|ML)_FF_[A-Z0-9_]+)\s*=\s*(?P<c_value>0x[0-9A-Fa-f]+))")
CAPS_BLOCK = re.compile(r"namespace platform_caps\s*\{(?P<body>.*?)\};", re.S)
CAP = re.compile(r"constexpr\s+caps_t\s+(?P<name>\w+)\s*=\s*(?P<value>0x[0-9A-Fa-f]+)")


def registry_flags(text):
    """Return {(namespace, value): identifier} from the registry tables."""
    flags = {}
    for match in ROW.finditer(text):
        name = match["name"]
        flags[(name[:2], int(match["value"], 16))] = name
    return flags


def fork_definitions(sources):
    """Yield (path, identifier, value) for every LI_FF_/ML_FF_ definition in host sources."""
    for path, text in sources:
        for match in DEFINITION.finditer(text):
            name = match["d_name"] or match["c_name"]
            yield path, name, int(match["d_value"] or match["c_value"], 16)


def host_caps(text):
    block = CAPS_BLOCK.search(text)
    return [(m["name"], int(m["value"], 16)) for m in CAP.finditer(block["body"])] if block else []


def collisions(registry_text, sources, caps_text):
    registry = registry_flags(registry_text)
    problems = []
    definitions = list(fork_definitions(sources))
    for path, name, value in definitions:
        owner = registry.get((name[:2], value))
        if owner and owner != name:
            problems.append(f"{path}: {name}=0x{value:X} collides with registry {owner}")
    # Advertised host bits must be distinct across platform caps and fork LI_FF_ constants.
    host_bits = {}
    for name, value in host_caps(caps_text) + [(n, v) for _, n, v in definitions if n.startswith("LI_")]:
        if value in host_bits and host_bits[value] != name:
            problems.append(f"host bit 0x{value:X} is shared by {host_bits[value]} and {name}")
        host_bits.setdefault(value, name)
    return problems


class CollisionDetectorTests(unittest.TestCase):
    REGISTRY_TEXT = "| `LI_FF_ENDPOINT_AUTO_REFRESH` | `0x8000` | x |\n| `ML_FF_ENDPOINT_AUTO_REFRESH` | `0x8000` | x |\n"
    CAPS_TEXT = "namespace platform_caps {\n constexpr caps_t endpoint_auto_refresh = 0x8000;\n};"

    def test_reused_registry_value_is_reported(self):
        source = [("restart_intent.h", "inline constexpr std::uint32_t ML_FF_HOST_RESTART = 0x8000;")]
        self.assertEqual(len(collisions(self.REGISTRY_TEXT, source, "")), 1)

    def test_shared_host_bit_is_reported(self):
        source = [("restart_intent.h", "inline constexpr std::uint32_t LI_FF_HOST_RESTART = 0x8000;")]
        problems = collisions("", source, self.CAPS_TEXT)
        self.assertTrue(any("shared" in p for p in problems), problems)

    def test_same_identifier_and_free_bits_pass(self):
        source = [("stream.cpp", "#define ML_FF_ENDPOINT_AUTO_REFRESH 0x8000"),
                  ("restart_intent.h", "inline constexpr std::uint32_t LI_FF_HOST_RESTART = 0x4000;")]
        self.assertEqual(collisions(self.REGISTRY_TEXT, source, self.CAPS_TEXT), [])


def check_tree(root):
    registry = root / REGISTRY
    if not registry.is_file():
        print(f"missing {registry}; initialise the moonlight-common-c submodule", file=sys.stderr)
        return 1
    sources = [(str(p.relative_to(root)), p.read_text(encoding="utf-8", errors="replace"))
               for p in sorted((root / "src").rglob("*")) if p.suffix in {".h", ".hpp", ".cpp", ".c"}]
    problems = collisions(registry.read_text(encoding="utf-8"), sources,
                          (root / CAPS_HEADER).read_text(encoding="utf-8"))
    for problem in problems:
        print(problem, file=sys.stderr)
    print(f"checked {len(registry_flags(registry.read_text(encoding='utf-8')))} registry flags, "
          f"{len(list(fork_definitions(sources)))} host definitions: {len(problems)} collision(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path(__file__).resolve().parents[2])
    args, rest = parser.parse_known_args()
    result = unittest.main(argv=[sys.argv[0], *rest], exit=False).result
    sys.exit(1 if not result.wasSuccessful() else check_tree(args.source.resolve()))
