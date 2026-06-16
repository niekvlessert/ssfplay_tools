#!/usr/bin/env python3
"""Build a vgmrips-style NiGHTS Into Dreams VGM/VGZ pack from SSF files."""

from __future__ import annotations

import argparse
import gzip
import os
import re
import shutil
import subprocess
import zipfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_INPUT = ROOT / "Nights Into Dreams (EMU).zophar"
DEFAULT_OUTPUT = ROOT / "NiGHTS Into Dreams"
PACK_TITLE = "NiGHTS Into Dreams..."
PACK_DIR_NAME = "NiGHTS Into Dreams"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Convert the local NiGHTS SSF rip to a vgmrips-style VGZ pack."
    )
    parser.add_argument("--input", type=Path, default=DEFAULT_INPUT,
                        help="SSF rip directory")
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT,
                        help="Output pack directory")
    parser.add_argument("--ssf2vgm", type=Path, default=None,
                        help="Path to ssf2vgm; auto-detected by default")
    parser.add_argument("--vgm-cmp", default=None,
                        help="Path to vgm_cmp; auto-detected from PATH by default")
    parser.add_argument("--creator", default=os.environ.get("USER", "unknown"),
                        help="Package creator name for the txt metadata")
    parser.add_argument("--package-version", default="1.00",
                        help="Package version for the txt metadata")
    parser.add_argument("--length-ms", type=int, default=None,
                        help="Override every track length, useful for quick test packs")
    parser.add_argument("--keep-vgm", action="store_true",
                        help="Keep temporary raw and vgm_cmp VGM files")
    parser.add_argument("--force", action="store_true",
                        help="Delete the output directory first if it already exists")
    parser.add_argument("--zip", action="store_true",
                        help="Also write a zip archive next to the output directory")
    return parser.parse_args()


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
    preferred = ["logo.png", "logo.jpg", "logo.jpeg", "cover.png", "cover.jpg", "cover.jpeg"]
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


def write_m3u(pack_dir: Path, tracks: list[dict[str, object]]) -> None:
    m3u = pack_dir / f"{PACK_DIR_NAME}.m3u"
    with m3u.open("w", encoding="utf-8", newline="\r\n") as out:
        for track in tracks:
            out.write(f"{track['vgz_name']}\n")


def write_txt(pack_dir: Path, tracks: list[dict[str, object]],
              creator: str, package_version: str) -> None:
    total_ms = sum(int(track["length_ms"]) for track in tracks)
    txt = pack_dir / f"{PACK_DIR_NAME}.txt"
    with txt.open("w", encoding="utf-8", newline="\r\n") as out:
        out.write("***********************************************\n")
        out.write("* VGM music package                           *\n")
        out.write("* http://vgmrips.net/                         *\n")
        out.write("***********************************************\n")
        out.write(f"Game name:           {PACK_TITLE}\n")
        out.write("System:              Sega Saturn\n")
        out.write("Music hardware:      SCSP\n\n")
        out.write("Music author:        Tomoko Sasaki,\n")
        out.write("                     Naofumi Hataya,\n")
        out.write("                     Fumie Kumatani\n")
        out.write("Game developer:      Sonic Team\n")
        out.write("Game publisher:      Sega\n")
        out.write("Game release date:   1996-07-05\n\n")
        out.write(f"Package created by:  {creator}\n")
        out.write(f"Package version:     {package_version}\n\n")
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
        out.write("The SSF set primarily exposes music entries. It does not include every\n")
        out.write("gameplay sound effect or disc-loaded asset from the original game.\n\n\n")
        out.write("Package history:\n")
        out.write(f"{package_version} {os.environ.get('SSFPLAY_PACK_DATE', '2026-06-16')} {creator}: Initial ssf2vgm pack.\n")


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
    input_dir = args.input.resolve()
    output_dir = args.output.resolve()
    if not input_dir.is_dir():
        raise SystemExit(f"input directory not found: {input_dir}")

    ssf2vgm = find_tool(args.ssf2vgm, ["ssf2vgm", "ssf2vgm.exe"])
    vgm_cmp = find_tool(Path(args.vgm_cmp) if args.vgm_cmp else None,
                        ["vgm_cmp", "vgm_cmp.exe"])

    ssf_files = sorted(input_dir.glob("*.ssf"), key=sort_key)
    if not ssf_files:
        raise SystemExit(f"no .ssf files found in {input_dir}")

    if output_dir.exists():
        if not args.force:
            raise SystemExit(f"output directory exists; pass --force to replace it: {output_dir}")
        shutil.rmtree(output_dir)
    output_dir.mkdir(parents=True)
    work_dir = output_dir / "_work"
    work_dir.mkdir()

    logo = find_logo(input_dir)
    if logo:
        shutil.copy2(logo, output_dir / f"{PACK_DIR_NAME}{logo.suffix.lower()}")

    tracks: list[dict[str, object]] = []
    for index, ssf in enumerate(ssf_files, 1):
        tags = read_tags(ssf)
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

    write_m3u(output_dir, tracks)
    write_txt(output_dir, tracks, args.creator, args.package_version)

    if not args.keep_vgm:
        shutil.rmtree(work_dir)

    if args.zip:
        archive = zip_pack(output_dir)
        print(f"wrote {archive}", flush=True)

    print(f"pack complete: {output_dir}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
