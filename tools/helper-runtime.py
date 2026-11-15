#!/usr/bin/env python3
"""Prepare a relocatable helper runtime and dependencies for CMake install."""
from argparse import ArgumentParser
from hashlib import sha256
import json
from pathlib import Path
import platform
import shutil
import subprocess
import tarfile
from urllib.request import Request, urlopen

ROOT = Path(__file__).resolve().parents[1]
RELEASE = "20261110"
PYTHON = "3.13.18"


def runtime_triple():
    machine = platform.machine().lower()
    arch = "aarch64" if machine in ("arm64", "aarch64") else "x86_64"
    return arch + {
        "Darwin": "-apple-darwin",
        "Windows": "-pc-windows-msvc",
        "Linux": "-unknown-linux-gnu",
    }[platform.system()]


def fetch(url):
    return urlopen(Request(url, headers={"User-Agent": "bilive-build"}), timeout=60)


def download_runtime(work):
    filename = f"cpython-{PYTHON}+{RELEASE}-{runtime_triple()}-install_only.tar.gz"
    release_url = f"https://api.github.com/repos/astral-sh/python-build-standalone/releases/tags/{RELEASE}"
    with fetch(release_url) as response:
        release = json.load(response)
    asset = next(asset for asset in release["assets"] if asset["name"] == filename)
    digest = asset["digest"].removeprefix("sha256:")
    archive = work / filename
    with fetch(asset["browser_download_url"]) as src, archive.open("wb") as output:
        shutil.copyfileobj(src, output)
    with archive.open("rb") as stream:
        if sha256(stream.read()).hexdigest() != digest:
            raise RuntimeError("Downloaded CPython runtime has the wrong checksum")
    with tarfile.open(archive) as tar:
        tar.extractall(work, filter="data")
    return work / "python"


def prepare(work, source=None, wheelhouse=None):
    work.mkdir(parents=True, exist_ok=True)
    source = source or download_runtime(work)
    runtime = work / "runtime"
    if runtime.exists():
        shutil.rmtree(runtime)
    site = work / "site"
    if site.exists():
        shutil.rmtree(site)
    shutil.copytree(source, runtime, symlinks=True)
    python = runtime / ("python.exe" if platform.system() == "Windows" else "bin/python3.13")
    command = [str(python), "-m", "pip", "--isolated", "install", "--disable-pip-version-check",
               "--only-binary=:all:", "--no-compile", "--target", str(site),
               "-r", str(ROOT / "helper/requirements.txt")]
    if wheelhouse:
        command += ["--no-index", "--find-links", str(wheelhouse)]
    subprocess.run(command, check=True)
    for directory in ("include", "share", "lib/python3.13/test", "lib/pkgconfig"):
        if (runtime / directory).is_dir():
            shutil.rmtree(runtime / directory)
    for lib in (runtime / "lib").glob("*.a"):
        lib.unlink()
    print(f"-DBILIVE_HELPER_RUNTIME={runtime} -DBILIVE_HELPER_SITE={site}")


if __name__ == "__main__":
    parser = ArgumentParser()
    parser.add_argument("output", type=lambda value: Path(value).resolve())
    parser.add_argument("--source", type=Path)
    parser.add_argument("--wheelhouse", type=Path)
    args = parser.parse_args()
    prepare(args.output, args.source, args.wheelhouse)
