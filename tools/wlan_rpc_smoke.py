#!/usr/bin/env python3
"""Read-only smoke test for the T-Embed WebFS RPC API."""

from __future__ import annotations

import argparse
import json
import time
import urllib.error
import urllib.parse
import urllib.request


def request(base: str, path: str, *, method: str = "GET", body: dict | None = None):
    data = None if body is None else json.dumps(body).encode("utf-8")
    headers = {"Accept": "application/json"}
    if data is not None:
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(base + path, data=data, headers=headers, method=method)
    with urllib.request.urlopen(req, timeout=10) as response:
        content_type = response.headers.get_content_type()
        payload = response.read()
        if content_type != "application/json":
            raise RuntimeError(f"{path}: expected JSON, got {content_type}")
        return json.loads(payload)


def expect_object(value, path: str):
    if not isinstance(value, dict):
        raise RuntimeError(f"{path}: expected an object")
    return value


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base", required=True, help="For example http://192.168.178.35")
    parser.add_argument("--exercise-cancel", action="store_true")
    args = parser.parse_args()
    base = args.base.rstrip("/")

    status = expect_object(request(base, "/api/status"), "/api/status")
    if status.get("api_version") != "1.1":
        raise RuntimeError("unexpected API version")
    capabilities = expect_object(request(base, "/api/capabilities"), "/api/capabilities")
    if capabilities.get("radio_read_only") is not True:
        raise RuntimeError("radio API must remain read-only")
    jobs = expect_object(request(base, "/api/jobs"), "/api/jobs")
    if not isinstance(jobs.get("jobs"), list):
        raise RuntimeError("jobs list is missing")
    settings = expect_object(request(base, "/api/settings"), "/api/settings")
    diagnostics = expect_object(request(base, "/api/diagnostics"), "/api/diagnostics")
    storage = expect_object(request(base, "/api/storage/list?path=/ext"), "/api/storage/list")

    if args.exercise_cancel:
        job = expect_object(
            request(
                base,
                "/api/subghz/rx",
                method="POST",
                body={"frequency_hz": 433_920_000, "duration_ms": 30_000},
            ),
            "/api/subghz/rx",
        )
        job_id = int(job["id"])
        request(base, f"/api/jobs/cancel?id={job_id}", method="POST")
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            result = request(base, f"/api/jobs?id={job_id}")
            if result.get("state") == "cancelled":
                break
            time.sleep(0.05)
        else:
            raise RuntimeError("job did not reach cancelled state")

    print(json.dumps({
        "status": status,
        "capabilities": capabilities,
        "jobs": jobs,
        "settings": settings,
        "diagnostics": diagnostics,
        "storage_path": storage.get("path"),
    }, indent=2))
    print("PASS: WebFS RPC API 1.1")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, urllib.error.URLError, ValueError, RuntimeError) as exc:
        raise SystemExit(f"FAIL: {exc}") from exc
