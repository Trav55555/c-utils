"""Fetch pinned Gitea source archives and verify their complete Git trees.

Private CI settings select an existing API login and source repository.
No credential copies or SSH fallback; extraction rejects unsafe archives.
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
    if (not isinstance(config, dict) or set(config) != {"schema", *NAMES}
            or config["schema"] != "c-utils.ci-dependencies/1"):
        raise ValueError("wrong dependency schema or membership")
    for name in NAMES:
        item = config[name]
        if not isinstance(item, dict) or set(item) != {"revision", "tree"}:
            raise ValueError("dependency entries must contain only revision and tree")
        if any(not isinstance(item[key], str) or not re.fullmatch("[0-9a-f]{40}", item[key])
               for key in ["revision", "tree"]):
            raise ValueError("dependency pins must be exact commit/tree hashes")
    return config


def private_source(environ):
    login = environ.get("CUTILS_CI_API_LOGIN", "")
    repository = environ.get("CUTILS_CI_TOOLCHAIN_REPOSITORY", "")
    if not re.fullmatch(r"[A-Za-z0-9_][A-Za-z0-9_.-]*", login):
        raise ValueError("CUTILS_CI_API_LOGIN must name an explicit configured API login")
    if not re.fullmatch(r"[A-Za-z0-9_][A-Za-z0-9_.-]*/[A-Za-z0-9_][A-Za-z0-9_.-]*", repository):
        raise ValueError("CUTILS_CI_TOOLCHAIN_REPOSITORY must be an owner/repository pair")
    return login, repository


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
    login, repository = private_source(os.environ)
    for name in NAMES:
        item = config[name]
        archive = args.work / (name + ".tar.gz")
        subprocess.run(["tea", "api", "--login", login, f"/repos/{repository}/archive/{item['revision']}.tar.gz",
                        "-o", str(archive)], check=True, timeout=90)
        extract(archive, args.work / name, item["tree"])
        print("PASS Gitea pinned dependency:", name, item["revision"], item["tree"], flush=True)


if __name__ == "__main__":
    main()
