"""Exercise the real helper with tiny disposable packages, never user runtimes.

python -B tests/updater/installer_tests.py --pwsh /path/to/pwsh --owner /path/to/update-owner --output /external/task/path
On Windows use Windows PowerShell 5.1 as --pwsh. Linux exercises filesystem
validation/rollback only; it cannot validate Windows locks, ACLs or UI teardown.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time
import zipfile

ROOT = Path(__file__).resolve().parents[2]


def sha(data):
    return hashlib.sha256(data).hexdigest()


def manifest(files, version="2.0.7", flavor="NVIDIA"):
    return json.dumps(dict(version=version, gpuPackage=flavor, files=[
        dict(path=name, size=len(data), sha256=sha(data)) for name, data in files.items()
    ])).encode()


def wait_for(predicate, child, timeout=15):
    deadline = time.monotonic() + timeout
    while not predicate():
        if child.poll() is not None:
            raise AssertionError(f"helper exited early: {child.returncode}")
        if time.monotonic() > deadline:
            raise AssertionError("helper timeout")
        time.sleep(.05)


def run_case(args, case):
    work = args.output / case
    work.mkdir(parents=True, exist_ok=False)
    root = work / "installed with spaces"
    root.mkdir()
    exe_bytes = args.owner.read_bytes()
    old_files = {"veyra_qml_ui.exe": exe_bytes, "payload.dll": b"original", "obsolete.dll": b"obsolete"}
    new_files = {"veyra_qml_ui.exe": exe_bytes, "payload.dll": b"updated!", "fresh.dll": b"new file"}
    for name, data in old_files.items():
        (root / name).write_bytes(data)
    exe = root / "veyra_qml_ui.exe"
    exe.chmod(0o755)
    old_manifest = manifest(old_files, "2.0.9" if case == "newer-install" else "2.0.7")
    (root / "package-manifest.json").write_bytes(old_manifest)
    (root / "user-settings.json").write_bytes(b"user configuration")
    if case == "modified-install":
        (root / "payload.dll").write_bytes(b"custom DLL")
    if case == "unowned-collision":
        (root / "fresh.dll").write_bytes(b"user file")
    before = {p.name: p.read_bytes() for p in root.iterdir() if p.is_file()}
    archive = work / "package.zip"
    new_manifest = manifest(new_files, "2.0.8", "AMD" if case == "wrong-flavor" else "NVIDIA")
    with zipfile.ZipFile(archive, "w") as z:
        for name, data in new_files.items():
            z.writestr("Veyra-2.0.8/" + name, b"bad hash" if case == "bad-file-hash" and name == "payload.dll" else data)
        z.writestr("Veyra-2.0.8/package-manifest.json", new_manifest)
        if case == "path-traversal":
            z.writestr("Veyra-2.0.8/../../escape", b"bad")
        if case == "case-collision":
            z.writestr("Veyra-2.0.8/PAYLOAD.DLL", b"bad")
    marker = work / "exit-owner"
    restarted = work / "restart arguments.txt"
    owner = subprocess.Popen([str(exe), "--wait", str(marker)])
    env = os.environ.copy()
    for name in ("XDG_CACHE_HOME", "XDG_DATA_HOME", "XDG_CONFIG_HOME"):
        env[name] = str(args.output / name.lower())
    context = dict(pid=owner.pid, directory=str(root), executable=str(exe), archive=str(archive),
                   sha256="0" * 64 if case == "bad-archive-hash" else sha(archive.read_bytes()),
                   version="2.0.8", flavor="NVIDIA", arguments=["--restarted", str(restarted), "space argument", 'quote"argument', "trailing\\"])
    ctx = work / "context.json"
    ctx.write_text(json.dumps(context))
    with (work / "helper-output.log").open("w") as log:
        child = subprocess.Popen([str(args.pwsh), "-NoProfile", "-NonInteractive", "-File",
                                  str(ROOT / "scripts/update/install-update.ps1"), "-Context", str(ctx)],
                                 env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            if case in ("success", "rollback", "cancel", "cancel-and-exit"):
                wait_for(lambda: (work / "ready").exists(), child)
                if case in ("cancel", "cancel-and-exit"):
                    (work / "cancel").touch()
                    if case == "cancel-and-exit":
                        marker.touch(); owner.wait(timeout=5)
                else:
                    # ZIP executable permissions are immaterial on Windows.
                    # Linux needs this solely to run the disposable restart fixture.
                    (work / "stage/veyra_qml_ui.exe").chmod(0o755)
                    if case == "rollback":
                        (work / "stage/payload.dll").write_bytes(b"corrupt!")
                    marker.touch()
                    owner.wait(timeout=5)
            code = child.wait(timeout=20)
            result_log = (work / "update.log").read_text(encoding="utf-8-sig")
            if case == "success":
                assert code == 0, result_log
                assert "UPDATE SUCCEEDED" in result_log
                for name, data in new_files.items():
                    assert (root / name).read_bytes() == data
                assert not (root / "obsolete.dll").exists()
                assert (root / "user-settings.json").read_bytes() == b"user configuration"
                deadline = time.monotonic() + 5
                while not restarted.exists() and time.monotonic() < deadline:
                    time.sleep(.05)
                assert restarted.read_text().splitlines() == ["space argument", 'quote"argument', "trailing\\"]
                backups = list(root.glob(".veyra-update-backup-*"))
                assert len(backups) == 1
                assert (backups[0] / "package-manifest.json").read_bytes() == old_manifest
            else:
                assert code != 0, case
                assert not restarted.exists()
                for name, data in before.items():
                    assert (root / name).read_bytes() == data, (case, name, result_log)
                assert not (args.output / "escape").exists()
                if case == "rollback":
                    assert "ROLLBACK SUCCEEDED" in result_log, result_log
                    assert not (root / "fresh.dll").exists()
            print("PASS", case, flush=True)
        finally:
            if child.poll() is None:
                child.kill(); child.wait()
            if owner.poll() is None:
                owner.terminate(); owner.wait()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--pwsh", type=Path, required=True)
    parser.add_argument("--owner", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output = args.output.resolve()
    for case in ("success", "bad-archive-hash", "bad-file-hash", "wrong-flavor", "newer-install", "path-traversal", "case-collision",
                 "modified-install", "unowned-collision", "cancel", "cancel-and-exit", "rollback"):
        run_case(args, case)
