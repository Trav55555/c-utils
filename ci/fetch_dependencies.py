"""Fetch pinned Gitea source archives and verify their complete Git trees.

The trusted host runner uses its existing tea login. No credential copies or
SSH fallback; archive extraction rejects links, traversal, and Git metadata.
"""
import argparse
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import tarfile

NAMES = ["dependable-c-agent-toolchain"]


def configuration(path):
    config = json.loads(path.read_text())
    if set(config) != {"schema", *NAMES} or config["schema"] != "c-utils.ci-dependencies/0":
        raise ValueError("wrong dependency schema or membership")
    for name in NAMES:
        item = config[name]
        if item["url"] != f"https://git.traversannan.com/trav/{name}.git":
            raise ValueError("unexpected dependency origin")
        if any(not re.fullmatch("[0-9a-f]{40}", item[key]) for key in ["revision", "tree"]):
            raise ValueError("dependency pins must be exact commit/tree hashes")
    return config


def extract(archive, target, expected_tree):
    if archive.stat().st_size > 64 * 1024 * 1024:
        raise ValueError("source archive exceeds compressed bound")
    with tarfile.open(archive, "r:gz") as stream:
        members = []
        total = 0
        for member in stream:
            total += member.size
            if len(members) == 10000 or total > 128 * 1024 * 1024:
                raise ValueError("source archive exceeds extraction bound")
            members.append(member)
        roots = set()
        paths = set()
        for member in members:
            path = PurePosixPath(member.name)
            if path.is_absolute() or ".." in path.parts or ".git" in path.parts or not path.parts:
                raise ValueError("unsafe source path")
            if not member.isdir() and not member.isfile():
                raise ValueError("source links/devices are not allowed")
            roots.add(path.parts[0])
            if path in paths or (len(path.parts) == 1 and not member.isdir()):
                raise ValueError("duplicate path or missing archive root")
            paths.add(path)
        if len(roots) != 1:
            raise ValueError("source archive must have one root")
        target.mkdir()
        for member in members:
            path = PurePosixPath(member.name)
            if len(path.parts) == 1:
                continue
            output = target.joinpath(*path.parts[1:])
            if member.isdir():
                output.mkdir(parents=True, exist_ok=True)
            else:
                output.parent.mkdir(parents=True, exist_ok=True)
                with stream.extractfile(member) as source, output.open("xb") as sink:
                    shutil.copyfileobj(source, sink)
                output.chmod(0o755 if member.mode & 0o111 else 0o644)
    env = dict(os.environ, GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull)
    subprocess.run(["git", "init", "-q", str(target)], env=env, check=True)
    subprocess.run(["git", "-C", str(target), "add", "-f", "--", "."], env=env, check=True)
    tree = subprocess.check_output(["git", "-C", str(target), "write-tree"], env=env, text=True).strip()
    if tree != expected_tree:
        raise ValueError("pinned source tree mismatch: " + target.name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("config", type=Path)
    parser.add_argument("work", type=Path)
    args = parser.parse_args()
    config = configuration(args.config)
    for name in NAMES:
        item = config[name]
        archive = args.work / (name + ".tar.gz")
        subprocess.run(["tea", "api", "--login", "trav", f"/repos/trav/{name}/archive/{item['revision']}.tar.gz",
                        "-o", str(archive)], check=True, timeout=90)
        extract(archive, args.work / name, item["tree"])
        print("PASS Gitea pinned dependency:", name, item["revision"], item["tree"], flush=True)


if __name__ == "__main__":
    main()
