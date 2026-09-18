#!/usr/bin/env python3
"""Export one Fortify teacher script into a local Xournal++ marking trial.

This utility is intentionally read-only. It never updates Fortify and writes its
output under a caller-provided local directory. Keep real student exports under
the gitignored `.marking-local/` directory.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
from typing import Any
from urllib.parse import quote, urlencode
from urllib.request import Request, urlopen
import xml.etree.ElementTree as ET


def load_env_file(path: str) -> dict[str, str]:
    values: dict[str, str] = {}
    if not path:
        return values
    for raw_line in Path(path).read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        values[key.strip()] = value.strip().strip("\"'")
    return values


def request(url: str, service_key: str) -> bytes:
    headers = {
        "apikey": service_key,
        "authorization": f"Bearer {service_key}",
    }
    with urlopen(Request(url, headers=headers), timeout=60) as response:
        return response.read()


def text(value: Any, fallback: str = "") -> str:
    return fallback if value is None else str(value)


def number(value: Any, fallback: float = 0.0) -> str:
    try:
        return f"{float(value):g}"
    except (TypeError, ValueError):
        return f"{fallback:g}"


def valid_box(value: Any) -> list[int] | None:
    if not isinstance(value, list) or len(value) != 4:
        return None
    try:
        box = [int(round(float(item))) for item in value]
    except (TypeError, ValueError):
        return None
    y0, x0, y1, x1 = box
    if not (0 <= y0 < y1 <= 1000 and 0 <= x0 < x1 <= 1000):
        return None
    return box


def canonical_part_id(value: Any, known_ids: set[str]) -> str:
    part_id = text(value, "unassigned")
    if part_id in known_ids:
        return part_id
    match = re.fullmatch(r"q(\d+)-([a-z])", part_id)
    if match:
        question, suffix = match.groups()
        candidate = f"csq-{question}{suffix}" if question == "1" else f"e{question}-{suffix}"
        if candidate in known_ids:
            return candidate
    return part_id


def anchor_node(parent: ET.Element, annotation: dict[str, Any]) -> None:
    text_anchor = annotation.get("text_anchor")
    diagram_anchor = annotation.get("diagram_anchor")
    if isinstance(text_anchor, dict):
        ET.SubElement(
            parent,
            "text-anchor",
            {
                "block-id": text(text_anchor.get("block_id")),
                "exact": text(text_anchor.get("exact")),
            },
        )
    elif isinstance(diagram_anchor, dict):
        ET.SubElement(
            parent,
            "diagram-anchor",
            {"block-id": text(diagram_anchor.get("block_id"))},
        )


def build_manifest(
    row: dict[str, Any],
    source_filename: str,
    mode: str,
    cancelled_work_excluded: bool,
) -> tuple[ET.ElementTree, int]:
    root = ET.Element("marking", {"version": "1"})
    ET.SubElement(
        root,
        "assignment",
        {
            "id": text(row.get("id")),
            "title": text(row.get("title"), "Teacher marking"),
            "student": text(row.get("student_name")),
            "mode": mode,
            "source-pdf": source_filename,
            "cancelled-work-excluded": "true" if cancelled_work_excluded else "false",
        },
    )

    parts = row.get("teacher_part_scores") or []
    if not isinstance(parts, list):
        parts = []
    awarded_total = sum(float(part.get("awarded_marks") or 0) for part in parts)
    max_total = sum(float(part.get("max_marks") or 0) for part in parts)
    ET.SubElement(
        root,
        "score",
        {"awarded": f"{awarded_total:g}", "max": f"{max_total:g}"},
    )
    parts_node = ET.SubElement(root, "parts")
    part_by_id: dict[str, dict[str, Any]] = {}
    for index, part in enumerate(parts):
        if not isinstance(part, dict):
            continue
        part_id = text(part.get("part_id") or part.get("id"), f"part-{index + 1}")
        part_by_id[part_id] = part
        ET.SubElement(
            parts_node,
            "part",
            {
                "id": part_id,
                "label": text(part.get("label"), part_id),
                "awarded": number(part.get("awarded_marks")),
                "max": number(part.get("max_marks")),
                "rationale": text(part.get("rationale")),
            },
        )

    annotations_node = ET.SubElement(root, "annotations")
    skipped = 0
    annotations = row.get("teacher_annotations") or []
    if not isinstance(annotations, list):
        annotations = []
    for index, annotation in enumerate(annotations):
        if not isinstance(annotation, dict):
            skipped += 1
            continue
        box = valid_box(annotation.get("bbox_norm"))
        if box is None:
            skipped += 1
            continue
        part_id = canonical_part_id(
            annotation.get("related_part_id")
            or annotation.get("part_id"),
            set(part_by_id),
        )
        atomic_part = part_by_id.get(part_id, {}) if mode == "page-boxes" else {}
        node = ET.SubElement(
            annotations_node,
            "annotation",
            {
                "id": text(annotation.get("id"), f"a{index + 1:03d}"),
                "page": str(max(1, int(annotation.get("page") or 1))),
                "part-id": part_id,
                "verdict": text(annotation.get("verdict"), "unresolved"),
                "source": text(annotation.get("source"), "ai"),
                "severity": text(annotation.get("severity")),
                "target-type": text(annotation.get("target_type"), "image"),
                "reviewed": "false",
                "awarded": number(
                    annotation.get("awarded_marks", atomic_part.get("awarded_marks"))
                ),
                "max": number(annotation.get("max_marks", atomic_part.get("max_marks"))),
            },
        )
        ET.SubElement(
            node,
            "box",
            {
                "y0": str(box[0]),
                "x0": str(box[1]),
                "y1": str(box[2]),
                "x1": str(box[3]),
            },
        )
        ET.SubElement(node, "title").text = text(annotation.get("title"), "Feedback")
        ET.SubElement(node, "comment").text = text(
            annotation.get("comment") or annotation.get("feedback")
        )
        ET.SubElement(node, "how-to-improve").text = text(
            annotation.get("how_to_improve")
        )
        ET.SubElement(node, "evidence").text = text(
            annotation.get("evidence_from_script")
        )
        anchor_node(node, annotation)

    ET.indent(root, space="  ")
    return ET.ElementTree(root), skipped


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("script_id")
    parser.add_argument("output_directory", type=Path)
    parser.add_argument("--mode", choices=("auto", "debox", "page-boxes"), default="auto")
    parser.add_argument(
        "--cancelled-work-excluded",
        action="store_true",
        help="Attest that cancelled STEM work is absent from the produced annotations.",
    )
    parser.add_argument(
        "--allow-trackable-output",
        action="store_true",
        help="Allow private output outside a gitignored path (unsafe; explicit use only).",
    )
    args = parser.parse_args()
    repo_root = Path(__file__).resolve().parents[2]
    ignored = subprocess.run(
        ["git", "check-ignore", "-q", str(args.output_directory)],
        cwd=repo_root,
        check=False,
    ).returncode == 0
    if not ignored and not args.allow_trackable_output:
        parser.error(
            "output_directory must be gitignored (use .marking-local/) "
            "or pass --allow-trackable-output explicitly"
        )

    file_env = load_env_file(os.environ.get("FORTIFY_ENV_FILE", ""))
    base_url = (
        os.environ.get("FORTIFY_SUPABASE_URL")
        or file_env.get("VITE_FORTIFYDATABASE_URL")
        or "https://gdpzixrljadkuzqaoxsx.supabase.co"
    ).rstrip("/")
    service_key = (
        os.environ.get("FORTIFY_SERVICE_KEY")
        or file_env.get("VITE_FORTIFYDATABASE_SERVICE_KEY")
        or file_env.get("VITE_FORTIFYDATABASE_SERVICE_ROLE_KEY")
        or ""
    )
    if not base_url or not service_key:
        parser.error("FORTIFY_SUPABASE_URL and FORTIFY_SERVICE_KEY are required")

    query = urlencode(
        {
            "id": f"eq.{args.script_id}",
            "select": (
                "id,title,student_name,storage_path,extracted_document,"
                "input_type,teacher_score,teacher_max_score,teacher_part_scores,teacher_annotations"
            ),
        }
    )
    rows = json.loads(request(f"{base_url}/rest/v1/teacher_marking_scripts?{query}", service_key))
    if len(rows) != 1:
        print(f"Expected one script, received {len(rows)}", file=sys.stderr)
        return 1
    row = rows[0]
    mode = args.mode
    if mode == "auto":
        mode = (
            "debox"
            if row.get("extracted_document") and row.get("input_type") != "image_bbox"
            else "page-boxes"
        )
    if mode == "page-boxes" and not args.cancelled_work_excluded:
        parser.error(
            "STEM/page-box export requires --cancelled-work-excluded after checking the producer output"
        )
    storage_path = text(row.get("storage_path"))
    if not storage_path:
        print("The script has no source PDF storage path", file=sys.stderr)
        return 1

    source_filename = "submission.pdf"
    manifest, skipped = build_manifest(
        row,
        source_filename,
        mode,
        args.cancelled_work_excluded or mode == "debox",
    )
    if skipped:
        print(
            f"Refusing incomplete export: {skipped} annotations lack valid page boxes",
            file=sys.stderr,
        )
        return 1

    args.output_directory.mkdir(parents=True, exist_ok=True)
    pdf_url = (
        f"{base_url}/storage/v1/object/teacher-marking-uploads/"
        f"{quote(storage_path, safe='/')}"
    )
    (args.output_directory / source_filename).write_bytes(request(pdf_url, service_key))

    manifest_path = args.output_directory / "marking.xoppmark"
    manifest.write(manifest_path, encoding="utf-8", xml_declaration=True)
    print(
        json.dumps(
            {
                "manifest": str(manifest_path),
                "source_pdf": str(args.output_directory / source_filename),
                "annotations": len(row.get("teacher_annotations") or []) - skipped,
                "skipped_without_page_box": skipped,
            }
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
