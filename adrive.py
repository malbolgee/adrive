#!/usr/bin/env python3

import json
import os
import subprocess
import sys
import time
from json import JSONDecodeError
from pathlib import Path
from typing import Optional

import requests

ROOT_BASE = "https://artifacts.mot.com:443/artifactory"
REPO = "scratch_US"
CHUNK = 8 * 1024 * 1024  # 8 MiB chunks


def load_basic_auth():
    user = os.getenv("ARTIFACTORY_USER")
    key = os.getenv("ARTIFACTORY_API_KEY")
    return user, key


def default_user_path_or_die(path_hint: Optional[str]) -> str:
    """
    If path_hint is provided, return it. Otherwise, use ARTIFACTORY_USER.
    Fail early if ARTIFACTORY_USER isn't available.
    """
    if path_hint and path_hint.strip():
        return path_hint.strip("/")
    user, _ = load_basic_auth()
    if not user:
        print("ARTIFACTORY_USER is required when --path is omitted.", file=sys.stderr)
        sys.exit(1)
    return user.strip("/")


def human(n: int) -> Optional[str]:
    size = float(n)
    for u in ["B", "KB", "MB", "GB", "TB"]:
        if size < 1024 or u == "TB":
            return f"{size:.2f} {u}"
        size /= 1024.0


def human_duration(seconds: float) -> str:
    seconds = int(seconds)
    d, r = divmod(seconds, 86400)
    h, r = divmod(r, 3600)
    m, s = divmod(r, 60)
    if d > 0:
        return f"{d}d {h}h {m}m {s}s"
    if h > 0:
        return f"{h}h {m}m {s}s"
    if m > 0:
        return f"{m}m {s}s"
    return f"{s}s"


def progress(prefix: str, done: int, total: int, start: float, base_done: int = 0):
    elapsed = max(time.time() - start, 1e-6)
    speed = (done - base_done) / elapsed
    eta = (total - done) / speed if speed > 0 and total else 0
    pct = (done / total) if total else 0
    bar = "#" * int(pct * 30) + "-" * (30 - int(pct * 30))
    sys.stdout.write(
        f"\r{prefix} [{bar}] {pct * 100:6.2f}% {human(done)}/{human(total)} speed={human(int(speed))}/s elapsed={human_duration(elapsed)} ETA={human_duration(eta)}\033[K"
    )
    sys.stdout.flush()


class ProgressFile:
    """File-like wrapper with __len__ so requests sets Content-Length; shows progress during read()."""

    def __init__(self, path: str):
        self.f = open(path, "rb")
        self.total = os.path.getsize(path)
        self.read_bytes = 0
        self.start = time.time()

    def __len__(self):
        return self.total

    def read(self, amt=CHUNK):
        data = self.f.read(amt)
        if not data:
            return b""
        self.read_bytes += len(data)
        progress("Uploading", self.read_bytes, self.total, self.start)
        return data

    def close(self):
        self.f.close()


def _short_sha1(path: str, length: int = 8) -> str:
    import hashlib
    h = hashlib.sha1()
    with open(path, "rb") as f:
        while True:
            b = f.read(1024 * 1024)  # 1 MiB buffer for hashing (not upload)
            if not b:
                break
            h.update(b)
    return h.hexdigest()[:length]


def _with_hash_suffix(filename: str, suffix: str) -> str:
    """
    Insert __<suffix> before the extension, or at the end if no extension.
    e.g. 'a.apk' -> 'a__abcd1234.apk', 'README' -> 'README__abcd1234'
    """
    if "." in filename and not filename.startswith("."):
        base, ext = filename.rsplit(".", 1)
        return f"{base}__{suffix}.{ext}"
    return f"{filename}__{suffix}"


def upload(dest_path: Optional[str], local_file: str, filename_override: Optional[str] = None):
    """Deploy Artifact (PUT /{repo}/{path}/{filename}) with a simple hash suffix."""
    user, key = load_basic_auth()
    if not (user and key):
        print("export ARTIFACTORY_USER and ARTIFACTORY_API_KEY.", file=sys.stderr)
        sys.exit(1)

    src = Path(local_file)
    if not src.is_file():
        print(f"File not found: {local_file}", file=sys.stderr)
        sys.exit(2)

    # If dest_path not provided, default to ARTIFACTORY_USER
    targetpath = (dest_path.strip("/") if (dest_path and dest_path.strip()) else user.strip("/"))

    # Build filename with hash suffix (use filename_override if provided, else source name)
    original_name = filename_override or src.name
    short_hash = _short_sha1(str(src), length=8)  # adjust length (8–12) to your preference
    filename = _with_hash_suffix(original_name, short_hash)

    url = f"{ROOT_BASE}/{REPO}/{targetpath}/{filename}"

    pf = ProgressFile(str(src))
    try:
        resp = requests.put(
            url,
            data=pf,  # fixed-length stream (requests sets Content-Length)
            auth=(user, key),  # Basic auth (user:apikey) — like curl -u
            headers={"Content-Type": "application/octet-stream"},
            timeout=300
        )
        sys.stdout.write("\n")
        resp.raise_for_status()
    except requests.HTTPError:
        sys.stdout.write("\n")
        try:
            msg = json.dumps(resp.json(), indent=2)
        except JSONDecodeError:
            msg = resp.text
        print(f"HTTP error: {msg}", file=sys.stderr)
        sys.exit(1)
    finally:
        pf.close()

    try:
        print(json.dumps(resp.json(), indent=2))
    except JSONDecodeError:
        print(resp.text)


def list_files(path: Optional[str]):
    """File List (GET /api/storage/{repo}/{path}?list&deep=1)"""
    user, key = load_basic_auth()
    if not (user and key):
        print("Set ARTIFACTORY_USER and ARTIFACTORY_API_KEY.", file=sys.stderr)
        sys.exit(1)

    p = default_user_path_or_die(path)
    url = f"{ROOT_BASE}/api/storage/{REPO}/{p}?list&deep=1&listFolders=0"
    r = requests.get(url, auth=(user, key), timeout=60)
    r.raise_for_status()
    data = r.json()

    files = [
        {
            "name": e.get("uri", "").lstrip("/"),
            "size": int(e.get("size", "0")) if isinstance(e.get("size"), str) else e.get("size", 0),
            "sha1": e.get("sha1"),
            "lastModified": e.get("lastModified"),
        }
        for e in data.get("files", [])
        if str(e.get("folder", "false")).lower() == "false"
    ]
    if not files:
        print("No files found.")
        return

    print(f"Files under {REPO}/{p}:\n")
    for f in files:
        print(f"  {f['name']}  size={human(f['size'])}  sha1={f['sha1']}  lastModified={f['lastModified']}")


def _find_by_sha1_anywhere(sha1: str) -> Optional[tuple]:
    """
    Checksum Search (GET /api/search/checksum?sha1=...&repos=...)
    Returns (path_without_repo, filename) for the first match anywhere in the repo.
    """
    user, key = load_basic_auth()
    if not (user and key):
        return None
    url = f"{ROOT_BASE}/api/search/checksum"
    params = {"sha1": sha1, "repos": REPO}
    r = requests.get(url, auth=(user, key), params=params, timeout=60)
    r.raise_for_status()
    j = r.json()
    needle = f"/api/storage/{REPO}/"
    for res in j.get("results", []):
        uri = res.get("uri", "")
        if needle in uri:
            sub = uri.split(needle, 1)[1]  # "{path}/{filename}"
            parts = sub.strip("/").split("/")
            if parts:
                filename = parts[-1]
                path_without_repo = "/".join(parts[:-1])  # may be empty
                return path_without_repo, filename
    return None


def normalize_filename(filename: str) -> str:
    """Strip trailing __[a-f0-9]{8} hash suffix before matching extension."""
    import re
    clean = re.sub(r"__[a-f0-9]{8}\.", ".", filename)
    clean = re.sub(r"__[a-f0-9]{8}$", "", clean)
    return clean


EXTRACT_MAPS = [
    {"mime": "application/zip", "ext": ".zip", "prog": "unzip", "attrs": ""},
    {"mime": "application/x-tar", "ext": ".tar", "prog": "tar", "attrs": "xf"},
    {"mime": "application/x-gzip", "ext": ".tar.gz", "prog": "tar", "attrs": "xzf"},
    {"mime": "application/gzip", "ext": ".gz", "prog": "gunzip", "attrs": ""},
    {"mime": "application/x-bzip2", "ext": ".bz2", "prog": "bunzip2", "attrs": ""},
    {"mime": "application/x-xz", "ext": ".xz", "prog": "unxz", "attrs": ""},
]


def perform_extraction(filename, content_type, content_encoding):
    eff_type = content_type
    if eff_type:
        eff_type = eff_type.split(";")[0].strip()

    if not eff_type or eff_type == "application/octet-stream":
        eff_type = get_mime_type_from_file(filename)

    prog = None
    attrs = None

    # 1. Try normalized filename extension matching first
    norm = normalize_filename(filename)
    for entry in EXTRACT_MAPS:
        if norm.lower().endswith(entry["ext"]):
            prog = entry["prog"]
            attrs = entry["attrs"]
            break

    # 2. Fall back to MIME type matching
    if not prog and eff_type:
        for entry in EXTRACT_MAPS:
            if eff_type.lower() == entry["mime"]:
                prog = entry["prog"]
                attrs = entry["attrs"]
                break

    if not prog:
        print(f"No standard extraction config found for file: {filename} (Type='{eff_type or '(unknown)'}')")
        return

    print(f"Extraction: Type='{eff_type or '(unknown)'}', Encoding='{content_encoding or '(null)'}' using '{prog}'")
    cmd = f"{prog} {attrs if attrs else ''} \"{filename}\""
    print(f"Executing: {cmd}")
    ret = subprocess.call(cmd, shell=True)
    if ret != 0:
        print(f"Extraction failed with code {ret}")
    else:
        print("Extraction successful.")


def download(path: Optional[str], name: Optional[str], last: bool, _id: Optional[str], out: Optional[str],
             extract: bool = False):
    """Retrieve Artifact (GET /{repo}/{path}/{filename}) with progress."""
    user, key = load_basic_auth()
    if not (user and key):
        print("Set ARTIFACTORY_USER and ARTIFACTORY_API_KEY.", file=sys.stderr)
        sys.exit(1)

    # Case 1: --id anywhere in repo (ignore path)
    if _id and not name:
        resolved = _find_by_sha1_anywhere(_id)
        if not resolved:
            print(f"No artifact with sha1={_id} found in repo {REPO}", file=sys.stderr)
            sys.exit(4)
        path_without_repo, name = resolved
        p = path_without_repo  # may be empty if artifact is at repo root
        if not p:
            print("Resolved SHA1 at repo root.")
    else:
        # Case 2: use provided path or default to ARTIFACTORY_USER (for --last or explicit --name)
        p = default_user_path_or_die(path)

        # Case 3: --last and no name => pick latest in chosen path
        if last and not name:
            list_url = f"{ROOT_BASE}/api/storage/{REPO}/{p}?list&deep=1&listFolders=0"
            r = requests.get(list_url, auth=(user, key), timeout=60)
            r.raise_for_status()
            files = [e for e in r.json().get("files", []) if str(e.get("folder", "false")).lower() == "false"]
            if not files:
                print("No files found for --last.", file=sys.stderr)
                sys.exit(5)
            files.sort(key=lambda x: x.get("lastModified") or "", reverse=True)
            name = files[0]["uri"].lstrip("/")
            print(f"Selected latest file: {name}")

    if not name:
        print("Must provide --name, --id, or --last", file=sys.stderr)
        sys.exit(3)

    # Build download URL from resolved p and name
    url = f"{ROOT_BASE}/{REPO}/{p}/{name}" if p else f"{ROOT_BASE}/{REPO}/{name}"
    out = out or name

    resume_pos = 0
    if os.path.exists(out):
        resume_pos = os.path.getsize(out)

    # 1. Fetch file size and metadata using a HEAD request
    r_head = requests.head(url, auth=(user, key), allow_redirects=True, timeout=60)
    total_size = int(r_head.headers.get("Content-Length", 0))
    content_type = r_head.headers.get("Content-Type")
    content_encoding = r_head.headers.get("Content-Encoding")

    # 2. Parallel download if file >= 10 MB and there is no active resume
    if total_size >= 10 * 1024 * 1024 and resume_pos == 0:
        THREAD_COUNT = 4
        print(f"Starting parallel download with {THREAD_COUNT} threads...")

        # Pre-allocate sparse file dynamically
        with open(out, "wb") as f_alloc:
            f_alloc.truncate(total_size)

        chunk_size = total_size // THREAD_COUNT
        ranges = []
        for i in range(THREAD_COUNT):
            start_pos = i * chunk_size
            end_pos = (total_size - 1) if i == THREAD_COUNT - 1 else (i + 1) * chunk_size - 1
            ranges.append((start_pos, end_pos))

        thread_progress = [0] * THREAD_COUNT
        worker_success = [True] * THREAD_COUNT

        def download_segment(tid, s_pos, e_pos):
            try:
                headers = {"Range": f"bytes={s_pos}-{e_pos}"}
                r_seg = requests.get(url, auth=(user, key), headers=headers, stream=True, timeout=300)
                r_seg.raise_for_status()
                with open(out, "r+b") as f:
                    f.seek(s_pos)
                    got = 0
                    # read chunk-by-chunk and seek-write concurrently
                    for block in r_seg.iter_content(chunk_size=1024*1024):
                        if block:
                            f.write(block)
                            got += len(block)
                            thread_progress[tid] = got
            except Exception as ex:
                print(f"\nThread {tid} failed: {ex}", file=sys.stderr)
                worker_success[tid] = False

        import threading
        threads_running = True

        def progress_printer():
            start_time = time.time()
            while threads_running:
                total_dl = sum(thread_progress)
                progress("Downloading (Parallel)", total_dl, total_size, start_time)
                time.sleep(0.2)
            total_dl = sum(thread_progress)
            progress("Downloading (Parallel)", total_dl, total_size, start_time)

        printer_thread = threading.Thread(target=progress_printer)
        printer_thread.start()

        workers = []
        for i, (s_pos, e_pos) in enumerate(ranges):
            w = threading.Thread(target=download_segment, args=(i, s_pos, e_pos))
            workers.append(w)
            w.start()

        for w in workers:
            w.join()

        threads_running = False
        printer_thread.join()
        sys.stdout.write("\n")

        if not all(worker_success):
            print("One or more download threads failed.", file=sys.stderr)
            sys.exit(1)

        print(f"Saved to {out}")
        if extract:
            perform_extraction(out, content_type, content_encoding)
        return

    # Fallback: Single-threaded stream download (resumes or small files)
    headers = {}
    if resume_pos > 0:
        headers["Range"] = f"bytes={resume_pos}-"

    r = requests.get(url, auth=(user, key), headers=headers, stream=True, timeout=300)
    if r.status_code == 416:
        r.close()
        resume_pos = 0
        r = requests.get(url, auth=(user, key), stream=True, timeout=300)

    r.raise_for_status()

    mode = "wb"
    if r.status_code == 206:
        mode = "ab"
        cr = r.headers.get("Content-Range", "")
        total = int(cr.rsplit("/", 1)[1]) if "/" in cr else resume_pos + int(r.headers.get("Content-Length", "0"))
    else:
        resume_pos = 0
        total = int(r.headers.get("Content-Length", "0"))

    content_type = r.headers.get("Content-Type")
    content_encoding = r.headers.get("Content-Encoding")

    with r:
        got = resume_pos
        start = time.time()
        with open(out, mode) as f:
            while True:
                chunk = r.raw.read(CHUNK, decode_content=False)
                if not chunk:
                    break
                f.write(chunk)
                got += len(chunk)
                progress("Downloading", got, total, start, base_done=resume_pos)
    sys.stdout.write("\n")
    print(f"Saved to {out}")

    if extract:
        perform_extraction(out, content_type, content_encoding)


def main(argv=None):
    import argparse
    p = argparse.ArgumentParser(description="Artifactory REST CLI (hardcoded base+repo)")
    sub = p.add_subparsers(dest="cmd", required=True)

    # upload: --dest-path optional (defaults to ARTIFACTORY_USER)
    up = sub.add_parser("upload", help="Upload (REST Deploy Artifact)")
    up.add_argument("--dest-path", required=False, help="Path in repo (default: ARTIFACTORY_USER)")
    up.add_argument("--filename", help="Override filename in Artifactory")
    up.add_argument("file", help="Local file")
    up.set_defaults(func=lambda a: upload(a.dest_path, a.file, a.filename))

    # list: --path optional (defaults to ARTIFACTORY_USER)
    ls = sub.add_parser("list", help="List files (REST File List)")
    ls.add_argument("--path", required=False, help="Path in repo (default: ARTIFACTORY_USER)")
    ls.set_defaults(func=lambda a: list_files(a.path))

    # download:
    # --last may omit --path => default to ARTIFACTORY_USER
    # --id ignores --path and searches entire repo
    dl = sub.add_parser("download", help="Download (REST Retrieve Artifact)")
    dl.add_argument("--path", required=False, help="Path in repo (default: ARTIFACTORY_USER when needed)")
    dl.add_argument("--name", help="Exact filename")
    dl.add_argument("--id", help="SHA1 id (resolve via REST Checksum Search across entire repo)")
    dl.add_argument("--last", action="store_true",
                    help="Download most recent file (uses ARTIFACTORY_USER path when --path omitted)")
    dl.add_argument("--out", help="Output path")
    dl.add_argument("--extract", action="store_true", help="Extract downloaded file")
    dl.set_defaults(func=lambda a: download(a.path, a.name, a.last, a.id, a.out, a.extract))

    args = p.parse_args(argv)
    try:
        args.func(args)
    except requests.HTTPError as e:
        msg = str(e)
        try:
            msg = json.dumps(e.response.json(), indent=2)
        except Exception:
            msg = e.response.text if e.response is not None else msg
        print(f"HTTP error: {msg}", file=sys.stderr)
        sys.exit(1)
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
