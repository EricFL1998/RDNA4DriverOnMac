#!/usr/bin/env python3
"""Select log evidence by the boot UUID, not a polluted timestamp filter."""

import json
import pathlib
import sys


def records(path):
    with path.open(errors="replace") as stream:
        for line in stream:
            try:
                record = json.loads(line)
            except (ValueError, json.JSONDecodeError):
                continue
            if isinstance(record, dict) and isinstance(record.get("bootUUID"), str):
                yield record


def message(record):
    return record.get("eventMessage") or record.get("formatString") or ""


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: n48-boot-evidence.py OUT EVENTS.ndjson")
    out = pathlib.Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    ws_suffix = "/Resources/WindowServer"
    def is_ws(record):
        return record.get("processImagePath", "").endswith(ws_suffix)

    events = list(records(pathlib.Path(sys.argv[2])))
    starts = [r for r in events if is_ws(r) and message(r) == "Server is starting up"]
    compositor = [r for r in events if is_ws(r) and "Metal compositor activated." in message(r)]
    if not starts and not compositor:
        raise SystemExit("no WindowServer start or compositor marker with a boot UUID")
    # A targeted --last boot query need not include the earlier startup line.
    # Prefer the compositor's own boot; its later WindowServer sessions have
    # different boot UUIDs and must not replace the desktop's evidence.
    boot = (compositor or starts)[-1]["bootUUID"]
    main = [r for r in events if r.get("bootUUID") == boot and is_ws(r)]
    selected = [{"timestamp": r.get("timestamp"), "processID": r.get("processID"),
                 "senderImagePath": r.get("senderImagePath"),
                 "eventMessage": message(r)} for r in main]
    (out / "boot-evidence.json").write_text(json.dumps({
        "boot_uuid": boot, "events": selected,
    }, indent=2, ensure_ascii=False) + "\n")
    messages = [message(r) for r in main]
    evidence_lines = ["boot_uuid=" + boot]
    for text in messages:
        if any(key in text for key in ["Metal compositor", "BootSet=",
                                          "CA origin observe", "CA SW origin",
                                          "CA context import",
                                          "CA origin probe NOT installed"]):
            evidence_lines.append(text)
    (out / "ca-boot.txt").write_text("\n".join(evidence_lines) + "\n")
    print("\n".join(evidence_lines))
    if not any("Metal compositor activated." in m for m in messages):
        raise SystemExit("no Metal compositor evidence in the current boot")
    if not any("CA origin observe-only wrappers installed" in m for m in messages):
        raise SystemExit("no Metal origin observer evidence in the current boot")
    if not any("CA SW origin observe-only installed=1" in m for m in messages):
        raise SystemExit("no SW origin observer evidence in the current boot")


if __name__ == "__main__":
    main()
