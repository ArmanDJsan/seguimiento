"""
Copia las imagenes y dibuja las etiquetas del JSON afinado
(modelo / imagen / resultados) para revisarlas visualmente.

Uso:
    python annotate_predictions.py
    python annotate_predictions.py --pred predictions_tuned.json \
        --images cvat_images --out labeled_preview
"""

import argparse
import json
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

HERE = Path(__file__).resolve().parent
DEFAULT_PRED = HERE / "predictions_tuned.json"
DEFAULT_IMAGES = HERE / "cvat_images"
DEFAULT_OUT = HERE / "labeled_preview"


def load_colors() -> dict[str, str]:
    with open(HERE / "cvat_labels_pool.json", encoding="utf-8") as f:
        return {c["name"]: c["color"] for c in json.load(f)}


def text_color(hex_color: str) -> tuple[int, int, int]:
    h = hex_color.lstrip("#")
    r, g, b = (int(h[i:i + 2], 16) for i in (0, 2, 4))
    return (0, 0, 0) if (0.299 * r + 0.587 * g + 0.114 * b) > 140 else (255, 255, 255)


def font(size: int) -> ImageFont.FreeTypeFont:
    for name in ("arial.ttf", "DejaVuSans.ttf", "segoeui.ttf"):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            continue
    return ImageFont.load_default()


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pred", type=Path, default=DEFAULT_PRED)
    ap.add_argument("--images", type=Path, default=DEFAULT_IMAGES)
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT)
    args = ap.parse_args()

    data = json.loads(args.pred.read_text(encoding="utf-8"))
    colors = load_colors()
    args.out.mkdir(parents=True, exist_ok=True)
    fnt = font(18)
    fnt_small = font(16)

    total = 0
    for entry in data["resultados"]:
        src = args.images / entry["imagen"]
        if not src.exists():
            print(f"AVISO: no existe {src}")
            continue
        with Image.open(src) as im:
            canvas = im.convert("RGB")
        draw = ImageDraw.Draw(canvas)

        dets = sorted(entry["resultados"], key=lambda d: d["orden"])
        for d in dets:
            cid = int(d["clase_id"])
            name = d["clase"]
            color = colors.get(name, "#00ff00")
            x1, y1, x2, y2 = d["bbox_xyxy"]
            draw.rectangle([x1, y1, x2, y2], outline=color, width=4)

            label = f'{d["orden"]}:{cid}'
            tw = draw.textlength(label, font=fnt)
            th = 20
            ly = y1 - th - 6
            if ly < 0:
                ly = y2 + 6
            draw.rectangle([x1, ly, x1 + tw + 8, ly + th], fill=color)
            draw.text((x1 + 4, ly + 1), label, font=fnt, fill=text_color(color))
            total += 1

        header = f'{entry["imagen"]}  |  {data.get("estrategia", "")}'
        draw.text((12, 10), header, font=fnt_small, fill=(255, 255, 255),
                  stroke_width=2, stroke_fill=(0, 0, 0))
        seq = " ".join(str(d["clase_id"]) for d in dets)
        draw.text((12, 32), seq, font=fnt_small, fill=(255, 255, 0),
                  stroke_width=2, stroke_fill=(0, 0, 0))

        dst = args.out / entry["imagen"]
        canvas.save(dst)
        print(f"{entry['imagen']:<16} {len(dets):2d} etiquetas -> {dst}")

    print(f"\nTotal: {total} etiquetas en {args.out}")


if __name__ == "__main__":
    main()
