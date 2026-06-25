#!/usr/bin/env python3
"""Build a vgmrips-style VGM/VGZ pack from a directory of SSF files."""

from __future__ import annotations

import argparse
import gzip
import os
import re
import shutil
import subprocess
import zipfile
from collections import Counter
from datetime import date
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Convert an SSF rip directory to a vgmrips-style VGZ pack."
    )
    parser.add_argument("input_dir", nargs="?", type=Path,
                        help="SSF rip directory")
    parser.add_argument("--input", dest="input_opt", type=Path, default=None,
                        help="SSF rip directory; alternative to positional input_dir")
    parser.add_argument("--output", type=Path, default=None,
                        help="Output pack directory")
    parser.add_argument("--pack-title", default=None,
                        help="Game/pack title; defaults to SSF game tag or input directory name")
    parser.add_argument("--pack-dir-name", default=None,
                        help="Base name for .m3u/.txt/image files; defaults to output directory name")
    parser.add_argument("--system", default="Sega Saturn",
                        help="System name for the txt metadata")
    parser.add_argument("--hardware", default="SCSP",
                        help="Music hardware for the txt metadata")
    parser.add_argument("--artist", action="append", default=None,
                        help="Music author line; may be passed multiple times")
    parser.add_argument("--developer", default=None,
                        help="Game developer metadata")
    parser.add_argument("--publisher", default=None,
                        help="Game publisher metadata")
    parser.add_argument("--release-date", default=None,
                        help="Game release date metadata")
    parser.add_argument("--notes", action="append", default=[],
                        help="Extra note paragraph; may be passed multiple times")
    parser.add_argument("--ssf2vgm", type=Path, default=None,
                        help="Path to ssf2vgm; auto-detected by default")
    parser.add_argument("--vgm-cmp", default=None,
                        help="Path to vgm_cmp; auto-detected from PATH by default")
    parser.add_argument("--creator", default=os.environ.get("USER", "unknown"),
                        help="Package creator name for the txt metadata")
    parser.add_argument("--package-version", default="1.00",
                        help="Package version for the txt metadata")
    parser.add_argument("--package-date", default=os.environ.get("SSFPLAY_PACK_DATE", date.today().isoformat()),
                        help="Package history date")
    parser.add_argument("--length-ms", type=int, default=None,
                        help="Override every track length, useful for quick test packs")
    parser.add_argument("--keep-vgm", action="store_true",
                        help="Keep temporary raw and vgm_cmp VGM files")
    parser.add_argument("--force", action="store_true",
                        help="Delete the output directory first if it already exists")
    parser.add_argument("--zip", action="store_true",
                        help="Also write a zip archive next to the output directory")
    return parser.parse_args()


def strip_rip_suffix(name: str) -> str:
    name = re.sub(r"\s*\(EMU\)\.zophar$", "", name, flags=re.IGNORECASE)
    name = re.sub(r"\.zophar$", "", name, flags=re.IGNORECASE)
    return name.strip() or "SSF Pack"


def most_common_tag(all_tags: list[dict[str, str]], key: str, fallback: str = "") -> str:
    values = [tags.get(key, "").strip() for tags in all_tags if tags.get(key, "").strip()]
    if not values:
        return fallback
    return Counter(values).most_common(1)[0][0]


def split_people(value: str) -> list[str]:
    if not value:
        return []
    return [part.strip() for part in re.split(r"\s*,\s*", value) if part.strip()]


def find_tool(explicit: Path | None, names: list[str]) -> str:
    if explicit:
        if explicit.exists():
            return str(explicit)
        raise SystemExit(f"tool not found: {explicit}")

    candidates: list[Path] = []
    for candidate in [
        ROOT / "build" / names[0],
        ROOT / "build-ci-fix" / names[0],
        ROOT / "build" / "Release" / names[0],
        ROOT / "build-ci-fix" / "Release" / names[0],
    ]:
        if candidate.exists():
            candidates.append(candidate)

    for name in names:
        found = shutil.which(name)
        if found:
            candidates.append(Path(found))
    if candidates:
        newest = max(candidates, key=lambda path: path.stat().st_mtime)
        return str(newest)
    raise SystemExit(f"tool not found: one of {', '.join(names)}")


def read_tags(path: Path) -> dict[str, str]:
    data = path.read_bytes()
    marker = data.rfind(b"[TAG]")
    if marker < 0:
        return {}
    text = data[marker + 5:].decode("utf-8", errors="replace")
    tags: dict[str, str] = {}
    for raw_line in text.replace("\r\n", "\n").replace("\r", "\n").split("\n"):
        line = raw_line.strip()
        if not line or "=" not in line:
            continue
        key, value = line.split("=", 1)
        tags[key.strip().lower()] = value.strip()
    return tags


def parse_time_ms(value: str | None) -> int:
    if not value:
        return 0
    parts = value.strip().split(":")
    try:
        if len(parts) == 1:
            seconds = float(parts[0])
        elif len(parts) == 2:
            seconds = int(parts[0]) * 60 + float(parts[1])
        elif len(parts) == 3:
            seconds = int(parts[0]) * 3600 + int(parts[1]) * 60 + float(parts[2])
        else:
            return 0
    except ValueError:
        return 0
    return max(0, int(seconds * 1000 + 0.5))


def format_time(ms: int) -> str:
    total_seconds = int((ms + 500) // 1000)
    hours, rem = divmod(total_seconds, 3600)
    minutes, seconds = divmod(rem, 60)
    if hours:
        return f"{hours}:{minutes:02d}:{seconds:02d}"
    return f"{minutes}:{seconds:02d}"


def fit_text(text: str, width: int) -> str:
    if len(text) <= width:
        return text
    if width <= 3:
        return text[:width]
    clipped = text[:width - 3]
    for separator in [" ", " - ", " [", " ("]:
        pos = clipped.rfind(separator)
        if pos >= width // 2:
            clipped = clipped[:pos]
            break
    return clipped.rstrip(" -([") + "..."


def sort_key(path: Path) -> tuple[int, str]:
    match = re.match(r"^(\d+)", path.name)
    return (int(match.group(1)) if match else 9999, path.name.casefold())


def safe_filename(name: str) -> str:
    name = re.sub(r'[<>:"/\\|?*\x00-\x1f]', " - ", name)
    name = re.sub(r"\s+", " ", name).strip(" .")
    reserved = {
        "CON", "PRN", "AUX", "NUL",
        *(f"COM{i}" for i in range(1, 10)),
        *(f"LPT{i}" for i in range(1, 10)),
    }
    if not name:
        name = "untitled"
    if name.upper() in reserved:
        name = f"_{name}"
    return name[:180].rstrip(" .")


def find_logo(input_dir: Path) -> Path | None:
    preferred = [
        "logo.png", "logo.jpg", "logo.jpeg",
        "cover.png", "cover.jpg", "cover.jpeg",
        "front.png", "front.jpg", "front.jpeg",
    ]
    lower_map = {p.name.lower(): p for p in input_dir.iterdir() if p.is_file()}
    for name in preferred:
        if name in lower_map:
            return lower_map[name]
    images = sorted(
        [p for p in input_dir.iterdir()
         if p.is_file() and p.suffix.lower() in {".png", ".jpg", ".jpeg"}],
        key=lambda p: p.name.casefold(),
    )
    return images[0] if images else None


def run(cmd: list[str]) -> None:
    print("+", " ".join(cmd), flush=True)
    subprocess.run(cmd, check=True)


def gzip_file(src: Path, dst: Path) -> None:
    with src.open("rb") as fin, dst.open("wb") as raw_out:
        with gzip.GzipFile(
            filename="", mode="wb", fileobj=raw_out, compresslevel=9, mtime=0
        ) as fout:
            shutil.copyfileobj(fin, fout)


def write_m3u(pack_dir: Path, pack_dir_name: str, tracks: list[dict[str, object]]) -> None:
    m3u = pack_dir / f"{pack_dir_name}.m3u"
    with m3u.open("w", encoding="utf-8", newline="\r\n") as out:
        for track in tracks:
            out.write(f"{track['vgz_name']}\n")


def write_multiline_field(out, label: str, values: list[str]) -> None:
    if not values:
        return
    out.write(f"{label:<21}{values[0]}\n")
    for value in values[1:]:
        out.write(f"{'':21}{value}\n")


def write_txt(pack_dir: Path, pack_dir_name: str, pack_title: str,
              tracks: list[dict[str, object]], metadata: dict[str, object]) -> None:
    total_ms = sum(int(track["length_ms"]) for track in tracks)
    txt = pack_dir / f"{pack_dir_name}.txt"
    with txt.open("w", encoding="utf-8", newline="\r\n") as out:
        out.write("***********************************************\n")
        out.write("* VGM music package                           *\n")
        out.write("* http://vgmrips.net/                         *\n")
        out.write("***********************************************\n")
        out.write(f"Game name:           {pack_title}\n")
        out.write(f"System:              {metadata['system']}\n")
        out.write(f"Music hardware:      {metadata['hardware']}\n\n")
        write_multiline_field(out, "Music author:", list(metadata["artists"]))
        if metadata.get("developer"):
            out.write(f"Game developer:      {metadata['developer']}\n")
        if metadata.get("publisher"):
            out.write(f"Game publisher:      {metadata['publisher']}\n")
        if metadata.get("release_date"):
            out.write(f"Game release date:   {metadata['release_date']}\n")
        out.write("\n")
        out.write(f"Package created by:  {metadata['creator']}\n")
        out.write(f"Package version:     {metadata['package_version']}\n\n")
        out.write("Song list, in approximate game order:\n")
        out.write("Song name                                              Length:\n")
        out.write("                                                       Total  Loop\n")
        for track in tracks:
            name = fit_text(str(track["display_name"]), 52)
            out.write(f"{name:52} {format_time(int(track['length_ms'])):>6}  -\n")
        out.write("\n")
        out.write(f"Total Length                                      {format_time(total_ms):>6}\n\n\n")
        out.write("Notes:\n")
        out.write("This pack was generated from the supplied SSF rip using ssf2vgm's\n")
        out.write("native Sega Saturn SCSP capture path. Each track was converted to VGM,\n")
        out.write("processed with vgm_cmp, and gzip-compressed to VGZ.\n\n")
        out.write("SSF sets primarily expose ripped music entries. They do not necessarily\n")
        out.write("include every gameplay sound effect or disc-loaded asset from the original game.\n")
        for note in metadata["notes"]:
            out.write("\n")
            out.write(str(note).strip() + "\n")
        out.write("\n\nPackage history:\n")
        out.write(
            f"{metadata['package_version']} {metadata['package_date']} "
            f"{metadata['creator']}: Initial ssf2vgm pack.\n"
        )


def zip_pack(pack_dir: Path) -> Path:
    archive = pack_dir.with_suffix(".zip")
    if archive.exists():
        archive.unlink()
    with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        for path in sorted(pack_dir.iterdir(), key=lambda p: p.name.casefold()):
            zf.write(path, arcname=f"{pack_dir.name}/{path.name}")
    return archive


def main() -> int:
    args = parse_args()
    input_arg = args.input_opt or args.input_dir
    if input_arg is None:
        raise SystemExit("input directory required; pass an SSF rip directory or --input DIR")
    input_dir = input_arg.resolve()
    if not input_dir.is_dir():
        raise SystemExit(f"input directory not found: {input_dir}")

    default_title = strip_rip_suffix(input_dir.name)
    output_dir = (args.output or (ROOT / default_title)).resolve()
    pack_dir_name = args.pack_dir_name or output_dir.name

    ssf_files = sorted(input_dir.glob("*.ssf"), key=sort_key)
    if not ssf_files:
        raise SystemExit(f"no .ssf files found in {input_dir}")
    all_tags = [read_tags(ssf) for ssf in ssf_files]

    pack_title = args.pack_title or most_common_tag(all_tags, "game", default_title)
    artist_values = args.artist or split_people(most_common_tag(all_tags, "artist"))
    metadata = {
        "system": args.system,
        "hardware": args.hardware,
        "artists": artist_values,
        "developer": args.developer,
        "publisher": args.publisher,
        "release_date": args.release_date or most_common_tag(all_tags, "year"),
        "creator": args.creator,
        "package_version": args.package_version,
        "package_date": args.package_date,
        "notes": args.notes,
    }

    ssf2vgm = find_tool(args.ssf2vgm, ["ssf2vgm", "ssf2vgm.exe"])
    vgm_cmp = find_tool(Path(args.vgm_cmp) if args.vgm_cmp else None,
                        ["vgm_cmp", "vgm_cmp.exe"])

    if output_dir.exists():
        if not args.force:
            raise SystemExit(f"output directory exists; pass --force to replace it: {output_dir}")
        shutil.rmtree(output_dir)
    output_dir.mkdir(parents=True)
    work_dir = output_dir / "_work"
    work_dir.mkdir()

    logo = find_logo(input_dir)
    if logo:
        shutil.copy2(logo, output_dir / f"{pack_dir_name}{logo.suffix.lower()}")

    tracks: list[dict[str, object]] = []
    for index, ssf in enumerate(ssf_files, 1):
        tags = all_tags[index - 1]
        title = tags.get("title") or ssf.stem
        prefix_match = re.match(r"^(\d+)", ssf.stem)
        prefix = prefix_match.group(1) if prefix_match else f"{index:02d}"
        display_name = f"{prefix} {title}"
        base = safe_filename(display_name)
        raw_vgm = work_dir / f"{base}.raw.vgm"
        cmp_vgm = work_dir / f"{base}.cmp.vgm"
        final_vgz = output_dir / f"{base}.vgz"

        cmd = [ssf2vgm]
        if args.length_ms is not None:
            cmd += ["--length-ms", str(args.length_ms)]
        cmd += [str(ssf), str(raw_vgm)]
        run(cmd)
        run([vgm_cmp, str(raw_vgm), str(cmp_vgm)])
        gzip_file(cmp_vgm, final_vgz)

        length_ms = args.length_ms if args.length_ms is not None else parse_time_ms(tags.get("length"))
        tracks.append({
            "display_name": display_name,
            "vgz_name": final_vgz.name,
            "length_ms": length_ms,
        })
        print(f"wrote {final_vgz.name} ({format_time(length_ms)})", flush=True)

    write_m3u(output_dir, pack_dir_name, tracks)
    write_txt(output_dir, pack_dir_name, pack_title, tracks, metadata)

    if not args.keep_vgm:
        shutil.rmtree(work_dir)

    if args.zip:
        archive = zip_pack(output_dir)
        print(f"wrote {archive}", flush=True)

    print(f"pack complete: {output_dir}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
