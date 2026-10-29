#!/usr/bin/env python3
"""Read-only inspection of the local CUPS filter chain.

Never calls lp/lpr, never creates a print job and never opens a network
connection. If a vendor filter cannot be executed safely offline, the report
stays "open" instead of claiming a pass.
"""
import argparse
import ctypes
import ctypes.util
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

FORBIDDEN = {"lp", "lpr", "lpstat", "lpadmin", "cupsenable", "cupsaccept"}
ALLOWED_FILTERS = {"pdftopdf", "pdftoraster", "gstoraster", "gstopdf", "bannertopdf"}


def load_ppd(queue):
    lib = ctypes.util.find_library("cups")
    if not lib:
        return None, "libcups not found"
    cups = ctypes.CDLL(lib)
    cups.cupsGetPPD.restype = ctypes.c_char_p
    cups.cupsGetPPD.argtypes = [ctypes.c_char_p]
    path = cups.cupsGetPPD(queue.encode())
    if not path:
        return None, "cupsGetPPD returned no path"
    candidate = Path(path.decode())
    try:
        text = candidate.read_text(errors="replace")
    except OSError as exc:
        return None, f"PPD unreadable: {exc}"
    return text, None


def parse_chain(ppd):
    filters = []
    for line in ppd.splitlines():
        m = re.match(r'\*cupsFilter2?:\s*"([^"]+)"', line)
        if not m:
            continue
        parts = m.group(1).split()
        if len(parts) >= 4:
            filters.append(
                {"source": parts[0], "destination": parts[1],
                 "cost": parts[2], "filter": parts[3]}
            )
        elif len(parts) == 3:
            filters.append(
                {"source": parts[0], "destination": "printer",
                 "cost": parts[1], "filter": parts[2]}
            )
    return filters


def parse_option(ppd, key):
    return sorted(set(re.findall(rf'^\*{re.escape(key)}\s+([^:/]+)', ppd, re.M)))


def ticket_mapping(ppd):
    page_sizes = parse_option(ppd, "PageSize")
    media = parse_option(ppd, "BRMediaType") + parse_option(ppd, "MediaType")
    quality = parse_option(ppd, "BRResolution") + parse_option(ppd, "cupsPrintQuality")
    color = parse_option(ppd, "BRMonoColor") + parse_option(ppd, "ColorModel")
    return {
        "pageSize": "BrPostC4x6_S" if "BrPostC4x6_S" in page_sizes else ("4x6" if "4x6" in page_sizes else None),
        "media": "Glossy" if "Glossy" in media else ("PhotographicGlossy" if "PhotographicGlossy" in media else None),
        "quality": "Fine" if "Fine" in quality else ("High" if "High" in quality else None),
        "color": "FullColor" if "FullColor" in color else ("RGB" if "RGB" in color else None),
        "duplex": "None" if "None" in parse_option(ppd, "Duplex") else None,
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--queue", required=True)
    parser.add_argument("--pdf", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()

    pdf = Path(args.pdf).resolve()
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    report = {"queue": args.queue, "pdf": str(pdf), "status": "open",
              "executed": [], "chain": [], "ticket": {}, "notes": []}

    if not pdf.is_file():
        report["notes"].append("input PDF missing")
        (out / "report.json").write_text(json.dumps(report, indent=2))
        return 2

    ppd, error = load_ppd(args.queue)
    if ppd is None:
        report["notes"].append(error)
        (out / "report.json").write_text(json.dumps(report, indent=2))
        return 2

    (out / "queue.ppd").write_text(ppd)
    report["chain"] = parse_chain(ppd)
    report["ticket"] = ticket_mapping(ppd)
    (out / "filter-chain.json").write_text(json.dumps(report["chain"], indent=2))

    filter_dir = Path("/usr/lib/cups/filter")
    for entry in report["chain"]:
        name = entry["filter"]
        if name in FORBIDDEN:
            report["notes"].append(f"refusing forbidden command {name}")
            continue
        path = filter_dir / name
        entry["path"] = str(path)
        entry["exists"] = path.exists()
        if not path.exists():
            report["notes"].append(f"filter missing: {name}")
            continue
        if name not in ALLOWED_FILTERS:
            report["notes"].append(
                f"vendor filter {name} not executed offline; output evidence stays open")
            continue
        # Only safe, well-known PDF filters are run, with a fixed local argv.
        target = out / f"{name}.out"
        with target.open("wb") as handle:
            completed = subprocess.run(
                [str(path), "1", "offline", "PhotoPrint", "1", "", str(pdf)],
                stdin=subprocess.DEVNULL, stdout=handle, stderr=subprocess.PIPE,
                check=False, timeout=120,
            )
        report["executed"].append({
            "filter": name, "returncode": completed.returncode,
            "bytes": target.stat().st_size,
            "stderr": completed.stderr.decode(errors="replace")[:1000],
        })

    if report["executed"] and all(item["returncode"] == 0 and item["bytes"] > 0
                                  for item in report["executed"]):
        report["status"] = "ran"
    (out / "report.json").write_text(json.dumps(report, indent=2))
    return 0 if report["status"] == "ran" else 2


if __name__ == "__main__":
    sys.exit(main())
