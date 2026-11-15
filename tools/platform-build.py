#!/usr/bin/env python3
"""Build and package the Qt SDK matched OBS 33.0.1 frontend plugin."""
from argparse import ArgumentParser
from hashlib import sha256
from pathlib import Path
import os
import platform
import re
import shutil
import subprocess
from urllib.request import Request, urlopen
import zipfile

ROOT = Path(__file__).resolve().parents[1]
OBS = "33.0.1"
QT = "2026-10-23"
OUT = "0.3.0"
OBS_URL = f"https://github.com/obsproject/obs-studio/releases/download/{OBS}/"
QT_URL = f"https://github.com/obsproject/obs-deps/releases/download/{QT}/"


def run(*cmd, **kwargs):
    return subprocess.run(cmd, check=True, **kwargs)


def asset(url, digest, path):
    if not path.exists():
        partial = path.with_suffix(path.suffix + ".download")
        with urlopen(Request(url, headers={"User-Agent": "bilive-build"}), timeout=90) as response:
            with partial.open("wb") as output:
                shutil.copyfileobj(response, output)
        partial.replace(path)
    with path.open("rb") as stream:
        current = sha256()
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            current.update(chunk)
        if current.hexdigest() != digest:
            raise RuntimeError(f"Checksum mismatch: {path}")
    return path


def unpack(path, dest):
    dest.mkdir(exist_ok=True, parents=True)
    if path.suffix == ".zip":
        with zipfile.ZipFile(path) as archive:
            archive.extractall(dest)
    else:
        run("tar", "-xf", str(path), "-C", str(dest))


def obs_import_library(dll, dest):
    symbols = []
    output = run("dumpbin", "/nologo", "/exports", str(dll), capture_output=True, text=True).stdout
    for line in output.splitlines():
        match = re.match(r"^\s*\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]{8,16}\s+([\w@$?]+)(?:\s|$)", line)
        if match:
            symbols.append(match.group(1))
    if not symbols:
        raise RuntimeError(f"No exports found in {dll}")
    definition = dest / (dll.stem + ".def")
    definition.write_text(f"LIBRARY {dll.name}\nEXPORTS\n" + "\n".join(symbols) + "\n", encoding="ascii")
    lib = dest / (dll.stem + ".lib")
    run("lib", "/nologo", "/machine:x64", f"/def:{definition}", f"/out:{lib}")
    return lib


def platform_sdk(work, obs_app):
    system = platform.system()
    if system == "Darwin":
        apple = platform.machine() == "arm64"
        arch = "arm64" if apple else "x86_64"
        qt = f"macos-deps-qt6-{QT}-{arch}.tar.xz"
        qt_digest = ("3f9153cd85b68526d865b60bd8b66fc98b945a8acefea77b2ae3a9eb2bf8718a" if apple else
                     "adc9a8b526baae51a951606d08c94a55480c3ad358a516eed3587567717a6225")
        unpack(asset(QT_URL + qt, qt_digest, work / qt), work / "qt")
        if obs_app is None:
            dmg = f"OBS-Studio-{OBS}-macOS-{'Apple' if apple else 'Intel'}.dmg"
            digest = ("3d6b2eedeb56416602af635adaba32327fdf11ff9d2806a4ce7d5b608de73238" if apple else
                      "745257d65e06eb0f784af229659223a863ad50d036464bb2b52f8a2d9278c2b1")
            mount = work / "mount"
            mount.mkdir(exist_ok=True)
            run("hdiutil", "attach", "-nobrowse", "-readonly", "-mountpoint", str(mount),
                str(asset(OBS_URL + dmg, digest, work / dmg)))
            obs_app = work / "OBS.app"
            try:
                run("ditto", str(mount / "OBS.app"), str(obs_app))
            finally:
                run("hdiutil", "detach", str(mount))
        return f"macos-{arch}", [f"-DBILIVE_OBS_APP={obs_app}", f"-DCMAKE_PREFIX_PATH={work / 'qt'}"]

    if system == "Windows":
        name = f"OBS-Studio-{OBS}-Windows-x64.zip"
        unpack(asset(OBS_URL + name, "0d9436451c2d772b33428e3fdae9776f071895c335de17283176306fb72651d3",
                     work / name), work / "obs")
        name = f"windows-deps-qt6-{QT}-x64-RelWithDebInfo.zip"
        unpack(asset(QT_URL + name, "fb2512e4308ebd37a6c732c16c0f3f92fa9a9d58df97408f2acae53cab0623da",
                     work / name), work / "qt")
        qt_config = next((work / "qt").rglob("Qt6Config.cmake"))
        qt_prefix = qt_config.parents[3]
        obs = work / "obs" / "bin" / "64bit"
        obs_lib = obs_import_library(obs / "obs.dll", work)
        frontend = obs_import_library(obs / "obs-frontend-api.dll", work)
        os.environ["PATH"] = str(qt_prefix / "bin") + os.pathsep + os.environ["PATH"]
        return "windows-x64", [f"-DBILIVE_OBS_LIB={obs_lib}", f"-DBILIVE_FRONTEND_LIB={frontend}",
                               f"-DCMAKE_PREFIX_PATH={qt_prefix}"]

    name = f"OBS-Studio-{OBS}-Ubuntu-26.04-x86_64.deb"
    package = asset(OBS_URL + name, "a9f062057576074439e6467f2716e3bbed6a0edab6ebf2c2f9595a3d2f1dc57f",
                    work / name)
    run("dpkg-deb", "-x", str(package), str(work / "obs"))
    libs = work / "obs" / "usr"
    def shared(prefix):
        matches = sorted(p for p in libs.rglob(prefix + ".so*") if p.is_file())
        if not matches:
            raise RuntimeError(f"{prefix} missing from {package}")
        return matches[0]
    return "linux-x64", [f"-DBILIVE_OBS_LIB={shared('libobs')}",
                         f"-DBILIVE_FRONTEND_LIB={shared('libobs-frontend-api')}"]


def build(work, obs_app):
    work.mkdir(parents=True, exist_ok=True)
    source = asset(f"https://codeload.github.com/obsproject/obs-studio/tar.gz/refs/tags/{OBS}",
                   "9174ee0e3677c92d0d060bdce4c5d44dfee9fd3afddfb6333f098fd029b8de2b", work / "obs-source.tgz")
    simde = asset("https://codeload.github.com/simd-everywhere/simde/tar.gz/refs/tags/v0.8.2",
                  "ed2a3268658f2f2a9b5367628a85ccd4cf9516460ed8604eed369653d49b25fb", work / "simde.tgz")
    unpack(source, work)
    unpack(simde, work)
    platform_id, sdk = platform_sdk(work, obs_app)
    build_dir = work / "build"
    run("cmake", "-S", str(ROOT), "-B", str(build_dir), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
        "-DBILIVE_OBS_PLUGIN=ON", f"-DBILIVE_OBS_SOURCE={work / ('obs-studio-' + OBS)}",
        f"-DBILIVE_SIMDE_INCLUDE={work / 'simde-0.8.2'}", *sdk)
    run("cmake", "--build", str(build_dir))
    run("ctest", "--test-dir", str(build_dir), "--output-on-failure")
    staged = work / "package"
    if staged.exists():
        shutil.rmtree(staged)
    run("cmake", "--install", str(build_dir), "--prefix", str(staged))
    dist = ROOT / "dist"
    dist.mkdir(exist_ok=True)
    base = dist / f"bilive-{OUT}-{platform_id}"
    if platform.system() == "Darwin":
        run("codesign", "--force", "--sign", "-",
            str(staged / "bilive.plugin/Contents/PlugIns/qt/tls/libqsecuretransportbackend.dylib"))
        run("codesign", "--force", "--sign", "-", str(staged / "bilive.plugin"))
        run("codesign", "--verify", "--strict", "--deep", str(staged / "bilive.plugin"))
        zip_path = str(base) + ".zip"
        Path(zip_path).unlink(missing_ok=True)
        run("ditto", "-ck", "--keepParent", str(staged / "bilive.plugin"), zip_path)
    elif platform.system() == "Linux":
        run("tar", "-czf", str(base) + ".tar.gz", "-C", str(staged), ".")
    else:
        shutil.make_archive(str(base), "zip", root_dir=staged)


if __name__ == "__main__":
    parser = ArgumentParser(description=__doc__)
    parser.add_argument("work", type=lambda value: Path(value).resolve())
    parser.add_argument("--obs-app", type=Path)
    args = parser.parse_args()
    build(args.work, args.obs_app)
