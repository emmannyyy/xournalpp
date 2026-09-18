#!/usr/bin/env python3
"""Create a secret-free PDF and marking manifest for the MVP."""

from __future__ import annotations

from pathlib import Path
import xml.etree.ElementTree as ET


def pdf_string(value: str) -> str:
    return value.replace("\\", "\\\\").replace("(", "\\(").replace(")", "\\)")


def create_pdf(path: Path, pages: list[list[str]]) -> None:
    page_ids = [4 + index * 2 for index in range(len(pages))]
    objects: dict[int, bytes] = {
        1: b"<< /Type /Catalog /Pages 2 0 R >>",
        2: (
            f"<< /Type /Pages /Kids [{' '.join(f'{page} 0 R' for page in page_ids)}] "
            f"/Count {len(page_ids)} >>"
        ).encode(),
        3: b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    }
    for index, lines in enumerate(pages):
        page_id = page_ids[index]
        content_id = page_id + 1
        commands = ["BT", "/F1 14 Tf", "72 740 Td"]
        for line_index, line in enumerate(lines):
            if line_index:
                commands.append("0 -28 Td")
            commands.append(f"({pdf_string(line)}) Tj")
        commands.append("ET")
        content = "\n".join(commands).encode()
        objects[page_id] = (
            f"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
            f"/Resources << /Font << /F1 3 0 R >> >> /Contents {content_id} 0 R >>"
        ).encode()
        objects[content_id] = (
            f"<< /Length {len(content)} >>\nstream\n".encode()
            + content
            + b"\nendstream"
        )

    output = bytearray(b"%PDF-1.4\n")
    offsets = [0] * (max(objects) + 1)
    for object_id in sorted(objects):
        offsets[object_id] = len(output)
        output.extend(f"{object_id} 0 obj\n".encode())
        output.extend(objects[object_id])
        output.extend(b"\nendobj\n")
    xref = len(output)
    output.extend(f"xref\n0 {len(offsets)}\n".encode())
    output.extend(b"0000000000 65535 f \n")
    for object_id in range(1, len(offsets)):
        output.extend(f"{offsets[object_id]:010d} 00000 n \n".encode())
    output.extend(
        (
            f"trailer\n<< /Size {len(offsets)} /Root 1 0 R >>\n"
            f"startxref\n{xref}\n%%EOF\n"
        ).encode()
    )
    path.write_bytes(output)


def add_annotation(
    parent: ET.Element,
    *,
    annotation_id: str,
    page: int,
    part_id: str,
    verdict: str,
    box: tuple[int, int, int, int],
    title: str,
    comment: str,
    source: str = "ai",
    exact: str | None = None,
) -> None:
    node = ET.SubElement(
        parent,
        "annotation",
        {
            "id": annotation_id,
            "page": str(page),
            "part-id": part_id,
            "verdict": verdict,
            "source": source,
            "severity": "minor" if verdict == "correct" else "major",
            "target-type": "image",
            "reviewed": "false",
            "awarded": "1" if verdict == "correct" else "0",
            "max": "1",
        },
    )
    ET.SubElement(
        node,
        "box",
        dict(zip(("y0", "x0", "y1", "x1"), map(str, box), strict=True)),
    )
    ET.SubElement(node, "title").text = title
    ET.SubElement(node, "comment").text = comment
    ET.SubElement(node, "how-to-improve").text = (
        "Keep the explanation linked to the question."
        if verdict == "correct"
        else "Add the missing intermediate step."
    )
    ET.SubElement(node, "evidence").text = exact or "Selected response"
    if exact is not None:
        ET.SubElement(
            node,
            "text-anchor",
            {"block-id": f"p{page}-b1", "exact": exact},
        )


def create_manifest(path: Path, mode: str) -> None:
    root = ET.Element("marking", {"version": "1"})
    ET.SubElement(
        root,
        "assignment",
        {
            "id": f"synthetic-{mode}",
            "title": "Synthetic teacher-marking demo",
            "student": "Anonymous",
            "mode": mode,
            "source-pdf": "submission.pdf",
            "cancelled-work-excluded": "true",
        },
    )
    ET.SubElement(root, "score", {"awarded": "1", "max": "2"})
    parts = ET.SubElement(root, "parts")
    part_ids = ("q1-a", "q1-a")
    if mode == "page-boxes":
        part_ids = ("q1-a-1", "q1-a-2")
        ET.SubElement(
            parts,
            "part",
            {
                "id": part_ids[0],
                "label": "Question 1(a), mark 1",
                "awarded": "1",
                "max": "1",
                "rationale": "Correct idea.",
            },
        )
        ET.SubElement(
            parts,
            "part",
            {
                "id": part_ids[1],
                "label": "Question 1(a), mark 2",
                "awarded": "0",
                "max": "1",
                "rationale": "Missing intermediate step.",
            },
        )
    else:
        ET.SubElement(
            parts,
            "part",
            {
                "id": part_ids[0],
                "label": "Question 1(a)",
                "awarded": "1",
                "max": "2",
                "rationale": "One idea is complete and one needs development.",
            },
        )
    annotations = ET.SubElement(root, "annotations")
    add_annotation(
        annotations,
        annotation_id="a01",
        page=1,
        part_id=part_ids[0],
        verdict="correct",
        box=(145, 100, 205, 820),
        title="Accurate definition",
        comment="Correct. You identified scarcity and competing uses.",
        exact="Resources are scarce and have competing uses." if mode == "debox" else None,
    )
    add_annotation(
        annotations,
        annotation_id="a02",
        page=2,
        part_id=part_ids[1],
        verdict="partial",
        box=(145, 100, 215, 850),
        title="Complete the mechanism",
        comment="You stated the final effect but skipped the intermediate step.",
        exact="Therefore, price falls." if mode == "debox" else None,
    )
    ET.indent(root, space="  ")
    ET.ElementTree(root).write(path, encoding="utf-8", xml_declaration=True)


def main() -> None:
    root = Path(__file__).resolve().parents[2] / ".marking-local" / "demo"
    root.mkdir(parents=True, exist_ok=True)
    create_pdf(
        root / "submission.pdf",
        [
            ["Question 1(a)", "Resources are scarce and have competing uses."],
            ["Question 1(a), continued", "Therefore, price falls."],
        ],
    )
    create_manifest(root / "humanities-debox.xoppmark", "debox")
    create_manifest(root / "stem-page-boxes.xoppmark", "page-boxes")
    print(root)


if __name__ == "__main__":
    main()
