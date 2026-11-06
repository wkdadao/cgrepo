#!/usr/bin/env python3
"""Check the PDF-to-Markdown corpus without the original conversion workspace.

Default checks use only Python's standard library. --pdf-text additionally
requires PyMuPDF (pip install PyMuPDF). Text coverage is page-local and ignores
punctuation/layout; it does not prove table semantics or technical correctness.
"""

import argparse
import collections
import hashlib
import html
import json
from pathlib import Path
import re
import struct
from urllib.parse import unquote, urlsplit
import zlib


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def table_cells(line):
    return [cell.strip() for cell in re.split(r"(?<!\\)\|", line.strip())[1:-1]]


def inspect_markdown(text, filename, errors):
    """Return fences, tables, explicit anchors, and non-code Markdown links."""
    fences = collections.Counter()
    tables = []
    table = []
    prose = []
    fence = None

    def flush_table():
        if table:
            tables.append(list(table))
            table.clear()

    for number, line in enumerate(text.splitlines(), 1):
        match = re.match(r"^(`{3,}|~{3,})(.*)$", line)
        if fence:
            if match and match[1][0] == fence[0] and len(match[1]) >= len(fence) and not match[2].strip():
                fence = None
            continue
        if match:
            flush_table()
            fence = match[1]
            fences[match[2].strip()] += 1
            continue
        if re.match(r"^[QA]:", line) and (line.count("Q:") > 1 or line.count("A:") > 1):
            errors.append(f"{filename}:{number}: merged question/answer paragraphs")
        prose.append(line)
        if line.startswith("|") and line.endswith("|"):
            table.append((number, table_cells(line)))
        else:
            flush_table()
    flush_table()
    if fence:
        errors.append(f"{filename}: unclosed code fence")

    for rows in tables:
        start, header = rows[0]
        width = len(header)
        if len(rows) < 2 or not all(re.fullmatch(r":?-{3,}:?", cell) for cell in rows[1][1]):
            errors.append(f"{filename}:{start}: missing Markdown table separator")
            continue
        if any(not cell for cell in header):
            errors.append(f"{filename}:{start}: empty table header (possible false column)")
        for number, cells in rows:
            if len(cells) != width:
                errors.append(f"{filename}:{number}: inconsistent table width")
        for number, cells in rows[2:]:
            # Corpus-specific review signal: these source tables have no genuine
            # one-cell data rows. Wrapped cells and callouts caused such rows.
            if sum(bool(cell) for cell in cells) < 2:
                errors.append(f"{filename}:{number}: sparse table row (review source layout)")

    body = "\n".join(prose)
    anchors = re.findall(r'<a id="([^"]+)"', body)
    if len(anchors) != len(set(anchors)):
        errors.append(f"{filename}: duplicate explicit anchor")
    if body.count("<details>") != body.count("</details>"):
        errors.append(f"{filename}: unbalanced details blocks")
    for diagram in re.findall(r"```mermaid\n(.*?)\n```", text, re.S):
        if diagram.startswith("stateDiagram") and re.search(r"-->[^\n]*:[^\n]*;", diagram):
            errors.append(f"{filename}: semicolon in state transition label can create unintended states")
    return fences, tables, set(anchors), re.findall(r"\]\(([^)\n]+)\)", body)


def check_png(path):
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("invalid PNG signature")
    offset = 8
    width = height = 0
    image_data = []
    while offset < len(data):
        length = struct.unpack_from(">I", data, offset)[0]
        tag = data[offset + 4:offset + 8]
        payload = data[offset + 8:offset + 8 + length]
        crc = struct.unpack_from(">I", data, offset + 8 + length)[0]
        if zlib.crc32(tag + payload) & 0xFFFFFFFF != crc:
            raise ValueError("PNG checksum mismatch")
        if tag == b"IHDR":
            width, height = struct.unpack_from(">II", payload)
        elif tag == b"IDAT":
            image_data.append(payload)
        offset += length + 12
        if tag == b"IEND":
            if offset != len(data) or min(width, height) < 8:
                raise ValueError("invalid PNG dimensions/end")
            zlib.decompress(b"".join(image_data))
            return
    raise ValueError("missing PNG end chunk")


def normalized(text):
    # Some source fonts encode µ as NUL; other NULs are list markers.
    text = re.sub("\x00(?=ops?\\b|s\\b)", "µ", text)
    return "".join(char.lower() for char in html.unescape(text) if char.isalnum())


def check_pdf_text(pdf_path, sections, errors):
    import fitz

    count = 0
    with fitz.open(pdf_path) as pdf:
        if len(pdf) != len(sections):
            errors.append(f"{pdf_path.name}: PDF/Markdown page count mismatch")
        for number, page in enumerate(pdf, 1):
            output = normalized(sections.get(number, ""))
            for block in page.get_text("dict")["blocks"]:
                for line in block.get("lines", []):
                    for span in line["spans"]:
                        # Repeated running headers/footers are intentionally omitted.
                        if span["bbox"][3] < 45 or span["origin"][1] > page.rect.height - 40:
                            continue
                        count += 1
                        source = normalized(span["text"])
                        if len(source) > 4 and source not in output:
                            errors.append(f"{pdf_path.name}:p{number}: unmatched source span {span['text']!r}")
    return count


def validate(root, pdf_text=False):
    manifest = json.loads((root / "assets/conversion_manifest.json").read_text())
    errors = []
    totals = collections.Counter(documents=0, pages=0, tables=0, code_blocks=0, diagrams=0)
    representations = collections.Counter()
    assets = set()
    counts = collections.Counter()
    for doc in manifest["documents"]:
        filename = doc["markdown"]
        pdf_path, md_path = root / doc["pdf"], root / filename
        if sha256(pdf_path) != doc["pdf_sha256"]:
            errors.append(f"{doc['pdf']}: SHA-256 mismatch")
        if sha256(md_path) != doc["markdown_sha256"]:
            errors.append(f"{filename}: SHA-256 mismatch")
        text = md_path.read_text()
        if "\x00" in text or "\ufffd" in text:
            errors.append(f"{filename}: invalid text glyph")
        fences, tables, anchors, links = inspect_markdown(text, filename, errors)
        parts = re.split(r"<!-- source-pdf-page: (\d+) -->", text)
        sections = {int(parts[i]): parts[i + 1] for i in range(1, len(parts), 2)}
        page_numbers = [int(parts[i]) for i in range(1, len(parts), 2)]
        if page_numbers != list(range(1, doc["pages"] + 1)):
            errors.append(f"{filename}: missing, duplicate, or reordered source page")
        for number in range(1, doc["pages"] + 1):
            if f"pdf-page-{number:03d}" not in anchors:
                errors.append(f"{filename}: missing page anchor {number}")
            expected = f"./{doc['pdf']}#page={number}"
            if expected not in sections.get(number, ""):
                errors.append(f"{filename}: missing source PDF link for page {number}")
        for link in links:
            url = urlsplit(link)
            if url.scheme or url.netloc:
                continue
            target = root / unquote(url.path) if url.path else md_path
            if not target.is_file():
                errors.append(f"{filename}: broken relative link {link}")
            elif url.fragment and target.suffix == ".md":
                target_anchors = set(re.findall(r'<a id="([^"]+)"', target.read_text()))
                if unquote(url.fragment) not in target_anchors:
                    errors.append(f"{filename}: broken anchor {link}")
        diagram_types = collections.Counter(item["representation"] for item in doc["diagrams"])
        if fences["mermaid"] != diagram_types["mermaid"]:
            errors.append(f"{filename}: Mermaid count mismatch")
        if sum(fences.values()) - fences["mermaid"] != doc["code_blocks"]:
            errors.append(f"{filename}: code block count mismatch")
        if len(tables) != doc["tables"] + diagram_types["table"] + diagram_types["comparison"]:
            errors.append(f"{filename}: table count mismatch")
        for item in doc["diagrams"]:
            asset = item["asset"]
            if asset in assets:
                errors.append(f"{filename}: duplicate asset {asset}")
            assets.add(asset)
            if links.count(asset) != 1:
                errors.append(f"{filename}: asset must be linked exactly once: {asset}")
            try:
                check_png(root / asset)
            except (OSError, ValueError, struct.error, zlib.error) as exc:
                errors.append(f"{asset}: {exc}")
        if pdf_text:
            counts["source_spans_checked"] += check_pdf_text(pdf_path, sections, errors)
        totals.update(documents=1, pages=doc["pages"], tables=doc["tables"], code_blocks=doc["code_blocks"], diagrams=len(doc["diagrams"]))
        representations.update(diagram_types)
        counts.update(markdown_tables=len(tables), mermaid=fences["mermaid"])

    for key, value in totals.items():
        if manifest["totals"][key] != value:
            errors.append(f"manifest: {key} total mismatch")
    if manifest["totals"]["representations"] != dict(representations):
        errors.append("manifest: representation totals mismatch")
    actual = {str(path.relative_to(root)) for path in (root / "assets").rglob("*.png")}
    if actual != assets:
        errors.append("manifest: missing or unlisted PNG assets")
    for filename in ("README.md", "CONVERSION_NOTES.md"):
        _, _, _, links = inspect_markdown((root / filename).read_text(), filename, errors)
        for link in links:
            url = urlsplit(link)
            if not url.scheme and not url.netloc and url.path and not (root / unquote(url.path)).is_file():
                errors.append(f"{filename}: broken relative link {link}")
    return {"totals": dict(totals), "checks": dict(counts), "errors": errors}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parent)
    parser.add_argument("--pdf-text", action="store_true", help="also compare page-local PDF text spans (requires PyMuPDF)")
    args = parser.parse_args()
    result = validate(args.root, args.pdf_text)
    print(json.dumps(result, ensure_ascii=False, indent=2))
    raise SystemExit(bool(result["errors"]))


if __name__ == "__main__":
    main()
