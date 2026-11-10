#!/usr/bin/env python3
"""Select a reachable fork release lineage and reject non-increasing versions."""
import argparse
import json
import re
import subprocess
import sys
from pathlib import Path


VERSION = re.compile(r"^v(\d+)\.(\d+)\.(\d+)(?:\.(\d+))?(?:\.杂鱼)?$")


def version(tag):
    match = VERSION.fullmatch(tag)
    if not match:
        raise ValueError(f"Unrecognized release version: {tag!r}")
    return tuple(int(part) for part in match.groups() if part is not None)


def require_newer(candidate, latest):
    if len(version(candidate)) != 4 or len(version(latest)) != 4:
        raise ValueError("Fork candidate and Latest must both have four numeric components")
    if version(candidate) <= version(latest):
        raise ValueError(f"Candidate {candidate} must be strictly greater than Latest {latest}")


def select(upstream, fork, latest, run_number, is_ancestor):
    if not isinstance(run_number, int) or run_number <= 0:
        raise ValueError("Run number must be positive")
    bases = []
    for releases, components, allow_prerelease in ((upstream, 3, True), (fork, 4, False)):
        for release in releases:
            # Upstream publishes its builds as prereleases; fork prereleases are retracted labels.
            if release.get("draft") or (release.get("prerelease") and not allow_prerelease):
                continue
            tag = release.get("tag_name", "")
            if not VERSION.fullmatch(tag):
                continue
            parsed = version(tag)
            sha = release.get("target_commitish", "")
            if len(parsed) == components and re.fullmatch(r"[0-9a-f]{40}", sha) and is_ancestor(sha):
                bases.append(parsed[:3])
    if not bases:
        raise ValueError("No reachable release base")
    candidate = f"v{'.'.join(map(str, max(bases)))}.{run_number}"
    require_newer(candidate, latest)
    return candidate


def read_releases(path):
    data = json.loads(Path(path).read_text(encoding="utf-8-sig"))
    return [release for page in data for release in page] if data and isinstance(data[0], list) else data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--upstream")
    parser.add_argument("--fork")
    parser.add_argument("--latest", required=True)
    parser.add_argument("--run-number", type=int)
    parser.add_argument("--candidate")
    args = parser.parse_args()
    latest = json.loads(Path(args.latest).read_text(encoding="utf-8-sig"))["tag_name"]
    if args.candidate:
        require_newer(args.candidate, latest)
        print(args.candidate)
        return

    def ancestor(sha):
        result = subprocess.run(
            ["git", "merge-base", "--is-ancestor", sha, "HEAD"],
            check=False, capture_output=True,
        )
        if result.returncode not in (0, 1):
            print(f"Skipping unavailable release commit {sha}: "
                  f"{result.stderr.decode(errors='replace').strip()}", file=sys.stderr)
        return result.returncode == 0

    print(select(read_releases(args.upstream), read_releases(args.fork),
                 latest, args.run_number, ancestor))


if __name__ == "__main__":
    main()
