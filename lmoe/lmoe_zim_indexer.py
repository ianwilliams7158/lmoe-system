#!/usr/bin/env python3
"""
LMoE ZIM Indexer
================
Scans a folder of ZIM files and builds a lightweight search index for use
with LMoE's Intelligence (RAG) panel.

For each ZIM it extracts:
  - ZIM title, description, language
  - For every article: title + first ~500 characters of plain text

The output is a single JSON file (lmoe_zim_index.json) that LMoE loads.
Progress is saved after each ZIM so the script can be safely interrupted
and resumed.

USAGE
-----
    python3 lmoe_zim_indexer.py --zim-dir /path/to/zims --output lmoe_zim_index.json

    # Limit articles per ZIM (useful for testing or very large ZIMs)
    python3 lmoe_zim_indexer.py --zim-dir /path/to/zims --max-articles 50000

    # Recurse into subdirectories
    python3 lmoe_zim_indexer.py --zim-dir /path/to/zims --recursive

REQUIREMENTS
------------
    pip install libzim beautifulsoup4
    (beautifulsoup4 is optional but improves text extraction from HTML articles)

PLATFORM
--------
    Works on Linux (Raspberry Pi OS) and Windows.
    On Windows use forward slashes or raw strings for paths:
        python lmoe_zim_indexer.py --zim-dir "C:/zims" --output "C:/zims/index.json"
"""

import argparse
import html
import json
import os
import re
import sys
import time
from pathlib import Path

# ── optional HTML parser ──────────────────────────────────────────────────────
try:
    from bs4 import BeautifulSoup
    HAS_BS4 = True
except ImportError:
    HAS_BS4 = False

try:
    import libzim
    from libzim import Archive
except ImportError:
    print("ERROR: libzim not installed.  Run:  pip install libzim", file=sys.stderr)
    sys.exit(1)


# ── text extraction ───────────────────────────────────────────────────────────

def _strip_html_bs4(raw: bytes) -> str:
    """Extract plain text using BeautifulSoup (better quality)."""
    try:
        soup = BeautifulSoup(raw, "html.parser")
        # Remove script / style / nav / header / footer noise
        for tag in soup(["script", "style", "nav", "header", "footer",
                          "figure", "figcaption", "table"]):
            tag.decompose()
        return soup.get_text(" ", strip=True)
    except Exception:
        return ""


def _strip_html_regex(raw: bytes) -> str:
    """Minimal HTML stripper when BeautifulSoup is unavailable."""
    try:
        text = raw.decode("utf-8", errors="replace")
        # Remove script/style blocks
        text = re.sub(r"<(script|style)[^>]*>.*?</(script|style)>",
                      " ", text, flags=re.S | re.I)
        # Remove all remaining tags
        text = re.sub(r"<[^>]+>", " ", text)
        # Decode HTML entities
        text = html.unescape(text)
        # Collapse whitespace
        return re.sub(r"\s+", " ", text).strip()
    except Exception:
        return ""


def extract_text(item, max_chars: int = 600) -> str:
    """Return up to max_chars of plain text from a ZIM item."""
    try:
        mime = item.mimetype
        if "html" not in mime:
            # Plain text, XML etc — decode directly
            raw = bytes(item.content)
            return raw.decode("utf-8", errors="replace")[:max_chars].strip()

        raw = bytes(item.content)
        if not raw:
            return ""

        text = _strip_html_bs4(raw) if HAS_BS4 else _strip_html_regex(raw)
        return text[:max_chars].strip()
    except Exception:
        return ""


# ── ZIM indexing ──────────────────────────────────────────────────────────────

def index_zim(zim_path: Path, max_articles: int, progress_data: dict) -> dict:
    """
    Index a single ZIM file.  Returns a dict:
        {
          "path":        "/abs/path/to/file.zim",
          "title":       "Wikipedia (English)",
          "description": "...",
          "language":    "en",
          "article_count": 1234,
          "articles": [
              {"title": "Beekeeping", "path": "A/Beekeeping", "snippet": "..."},
              ...
          ]
        }
    progress_data is the already-indexed article list if we're resuming.
    """
    path_str = str(zim_path.resolve())
    print(f"\n  Opening: {zim_path.name}  ({zim_path.stat().st_size / 1e9:.2f} GB)")

    zim = Archive(zim_path)

    # ZIM metadata
    def meta(key):
        try:
            return zim.get_metadata(key).decode("utf-8", errors="replace").strip()
        except Exception:
            return ""

    zim_title  = meta("Title")  or zim_path.stem
    zim_desc   = meta("Description")
    zim_lang   = meta("Language")
    total      = zim.entry_count

    print(f"  Title: {zim_title}  |  Articles: {total:,}  |  Lang: {zim_lang}")

    articles   = progress_data.get("articles", [])
    start_from = len(articles)

    if start_from > 0:
        print(f"  Resuming from article {start_from:,}")

    effective_max = min(max_articles, total) if max_articles > 0 else total
    t0 = time.time()
    skipped = 0

    for i in range(start_from, effective_max):
        try:
            entry = zim._get_entry_by_id(i)

            # Skip redirects
            if entry.is_redirect():
                skipped += 1
                continue

            title = entry.title.strip()
            if not title:
                skipped += 1
                continue

            item   = entry.get_item()
            mime   = item.mimetype

            # Only index HTML articles and plain text — skip images, CSS, JS etc
            if not ("html" in mime or "text/plain" in mime):
                skipped += 1
                continue

            snippet = extract_text(item, max_chars=500)

            articles.append({
                "title":   title,
                "path":    entry.path,
                "snippet": snippet,
            })

        except Exception as e:
            # Individual article errors are non-fatal
            pass

        # Progress report every 5000 articles
        if (i - start_from + 1) % 5000 == 0:
            elapsed  = time.time() - t0
            rate     = (i - start_from + 1) / max(elapsed, 1)
            remaining = (effective_max - i - 1) / max(rate, 1)
            print(f"    {i+1:>8,} / {effective_max:,}  "
                  f"({rate:.0f}/s  ~{remaining/60:.1f} min remaining)  "
                  f"indexed: {len(articles):,}  skipped: {skipped:,}")

    print(f"  Done: {len(articles):,} articles indexed, {skipped:,} skipped  "
          f"({time.time()-t0:.1f}s)")

    return {
        "path":          path_str,
        "title":         zim_title,
        "description":   zim_desc,
        "language":      zim_lang,
        "article_count": len(articles),
        "articles":      articles,
    }


# ── main ──────────────────────────────────────────────────────────────────────

def find_zim_files(zim_dir: Path, recursive: bool) -> list[Path]:
    if recursive:
        return sorted(zim_dir.rglob("*.zim"))
    else:
        return sorted(zim_dir.glob("*.zim"))


def load_existing_index(output_path: Path) -> dict:
    """Load existing index so we can resume without re-indexing completed ZIMs."""
    if output_path.exists():
        try:
            with open(output_path, "r", encoding="utf-8") as f:
                return json.load(f)
        except Exception:
            pass
    return {"zims": [], "built": "", "version": 1}


def save_index(index: dict, output_path: Path):
    tmp = output_path.with_suffix(".tmp")
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(index, f, ensure_ascii=False, separators=(",", ":"))
    tmp.replace(output_path)


def main():
    parser = argparse.ArgumentParser(
        description="Build a LMoE RAG index from ZIM files.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument("--zim-dir",      required=True,
                        help="Directory containing .zim files")
    parser.add_argument("--output",       default="lmoe_zim_index.json",
                        help="Output JSON path (default: lmoe_zim_index.json)")
    parser.add_argument("--max-articles", type=int, default=0,
                        help="Max articles per ZIM (0 = no limit)")
    parser.add_argument("--recursive",    action="store_true",
                        help="Recurse into subdirectories")
    parser.add_argument("--force",        action="store_true",
                        help="Re-index all ZIMs even if already in the index")
    args = parser.parse_args()

    zim_dir    = Path(args.zim_dir)
    output     = Path(args.output)

    if not zim_dir.is_dir():
        print(f"ERROR: {zim_dir} is not a directory", file=sys.stderr)
        sys.exit(1)

    zim_files = find_zim_files(zim_dir, args.recursive)
    if not zim_files:
        print(f"No .zim files found in {zim_dir}", file=sys.stderr)
        sys.exit(1)

    print(f"LMoE ZIM Indexer")
    print(f"{'─' * 50}")
    print(f"ZIM directory : {zim_dir}")
    print(f"Output file   : {output}")
    print(f"Max articles  : {args.max_articles or 'unlimited'}")
    print(f"ZIM files     : {len(zim_files)}")
    if not HAS_BS4:
        print("NOTE: beautifulsoup4 not installed — using fallback HTML stripper.")
        print("      For better text quality:  pip install beautifulsoup4")
    print()

    index = load_existing_index(output)

    # Build lookup of already-indexed ZIMs by absolute path
    indexed_by_path = {z["path"]: z for z in index.get("zims", [])}

    total_start = time.time()

    for i, zim_path in enumerate(zim_files, 1):
        abs_path = str(zim_path.resolve())
        print(f"[{i}/{len(zim_files)}] {zim_path.name}")

        # Resume: skip if already fully indexed and --force not set
        existing = indexed_by_path.get(abs_path)
        if existing and not args.force:
            if args.max_articles == 0 or existing["article_count"] >= args.max_articles:
                print(f"  Already indexed ({existing['article_count']:,} articles) — skipping")
                continue
            # Partially indexed — resume from where we left off
            progress_data = existing
        else:
            progress_data = {}

        try:
            result = index_zim(zim_path, args.max_articles, progress_data)
        except KeyboardInterrupt:
            print("\nInterrupted — saving progress so far...")
            # Save whatever we have so far
            save_index(index, output)
            print(f"Progress saved to {output}")
            sys.exit(0)
        except Exception as e:
            print(f"  ERROR indexing {zim_path.name}: {e}")
            continue

        # Update index
        if abs_path in indexed_by_path:
            # Replace in-place
            for j, z in enumerate(index["zims"]):
                if z["path"] == abs_path:
                    index["zims"][j] = result
                    break
        else:
            index["zims"].append(result)

        indexed_by_path[abs_path] = result
        index["built"] = time.strftime("%Y-%m-%d %H:%M:%S")
        index["version"] = 1

        # Save after every ZIM so progress is never lost
        print(f"  Saving index to {output} ...")
        save_index(index, output)

    # Summary
    total_articles = sum(z["article_count"] for z in index["zims"])
    size_mb = output.stat().st_size / 1e6 if output.exists() else 0
    elapsed = time.time() - total_start

    print()
    print(f"{'─' * 50}")
    print(f"Complete in {elapsed/60:.1f} minutes")
    print(f"ZIMs indexed  : {len(index['zims'])}")
    print(f"Total articles: {total_articles:,}")
    print(f"Index size    : {size_mb:.1f} MB")
    print(f"Output        : {output}")
    print()
    print("Load this file in LMoE → Settings → ZIM Library")


if __name__ == "__main__":
    main()
