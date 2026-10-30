"""
Convierte los frames PPM capturados por la opcion 10 del menu VIB
(training_frames/cap_XXXX.ppm) a un formato que CVAT puede importar
(PNG o JPG) y, opcionalmente, genera un ZIP listo para subir.

Uso:
    python ppm_to_cvat.py                       # usa rutas por defecto
    python ppm_to_cvat.py --src <carpeta>
    python ppm_to_cvat.py --format jpg --quality 95
    python ppm_to_cvat.py --zip cvat_upload.zip

Por defecto:
    --src  ../src/bin/Release/training_frames
    --out  ./cvat_images
    (el ZIP solo se crea si se pasa --zip)

Despues de etiquetar en CVAT:
    1. Exportar en formato "YOLO 1.1" CON imagenes.
    2. python prepare_dataset.py --zip llegada_dataset.zip
"""

import argparse
import re
import sys
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEFAULT_SRC = HERE.parent / "src" / "bin" / "Release" / "training_frames"
DEFAULT_OUT = HERE / "cvat_images"


def natural_key(path: Path):
    """Orden natural: cap_2 antes que cap_10."""
    return [int(tok) if tok.isdigit() else tok.lower()
            for tok in re.split(r"(\d+)", path.name)]


def find_ppm(src: Path) -> list[Path]:
    if not src.is_dir():
        raise FileNotFoundError(f"No existe la carpeta de origen: {src}")
    files = [p for p in src.rglob("*") if p.is_file() and p.suffix.lower() == ".ppm"]
    return sorted(files, key=natural_key)


def convert(src_files: list[Path], out_dir: Path, fmt: str, quality: int) -> list[Path]:
    from PIL import Image

    out_dir.mkdir(parents=True, exist_ok=True)
    written: list[Path] = []

    for i, src_path in enumerate(src_files, 1):
        dst = out_dir / f"{src_path.stem}.{fmt}"
        with Image.open(src_path) as img:
            img = img.convert("RGB")
            if fmt == "jpg":
                img.save(dst, format="JPEG", quality=quality, subsampling=0)
            else:
                img.save(dst, format="PNG")
        written.append(dst)
        print(f"  [{i}/{len(src_files)}] {src_path.name} -> {dst.name}")

    return written


def make_zip(files: list[Path], zip_path: Path) -> None:
    zip_path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as zf:
        for f in files:
            zf.write(f, arcname=f.name)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--src", type=Path, default=DEFAULT_SRC,
                    help=f"carpeta con los .ppm (default: {DEFAULT_SRC})")
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT,
                    help=f"carpeta de salida (default: {DEFAULT_OUT})")
    ap.add_argument("--format", choices=("png", "jpg"), default="png",
                    help="formato de imagen para CVAT (default: png)")
    ap.add_argument("--quality", type=int, default=95,
                    help="calidad JPEG 1-100, solo con --format jpg (default: 95)")
    ap.add_argument("--zip", type=Path, default=None,
                    help="si se indica, crea un ZIP con las imagenes convertidas")
    args = ap.parse_args()

    src_files = find_ppm(args.src)
    if not src_files:
        sys.exit(f"No se encontraron archivos .ppm en {args.src}")

    print(f"Origen : {args.src}")
    print(f"Salida : {args.out} ({args.format.upper()})")
    print(f"Frames : {len(src_files)}")

    written = convert(src_files, args.out, args.format, args.quality)
    print(f"Convertidas: {len(written)} imagenes")

    if args.zip:
        make_zip(written, args.zip)
        print(f"ZIP para CVAT: {args.zip}")

    print("\nSiguiente paso: subir las imagenes (o el ZIP) a CVAT, etiquetar con")
    print("las clases de cvat_labels_pool.json y exportar en 'YOLO 1.1' con imagenes.")


if __name__ == "__main__":
    main()
