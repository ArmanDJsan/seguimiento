"""
Prepara el dataset de llegada para entrenar con Ultralytics YOLO.

Entrada:  llegada_dataset.zip  (exportado de CVAT en formato "YOLO 1.1" CON imágenes)
Salida:   training/llegada/ con layout de Ultralytics:
            images/{train,val}/  labels/{train,val}/  data.yaml
          y un hold-out opcional (training/llegada/holdout/) para validación final.

Uso:
    python prepare_dataset.py [--zip llegada_dataset.zip] [--seed 42]
                              [--holdout 20] [--val-frac 0.2]
"""

import argparse
import json
import shutil
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
EXPECTED_LABELS_JSON = HERE / "cvat_labels_pool.json"
RAW_DIR = HERE / "llegada" / "raw"
OUT_DIR = HERE / "llegada"
IMG_EXTS = {".jpg", ".jpeg", ".png", ".bmp", ".ppm"}


def load_expected_classes() -> list[str]:
    """Lee el orden de clases desde cvat_labels_pool.json (bola_0..bola_9)."""
    with open(EXPECTED_LABELS_JSON, "r", encoding="utf-8") as f:
        data = json.load(f)
    return [item["name"] for item in data]


def find_classes_file(raw: Path) -> Path:
    """Localiza obj.names (o labels.txt) dentro del contenido descomprimido."""
    for name in ("obj.names", "labels.txt", "obj.data"):
        for p in raw.rglob(name):
            if p.is_file():
                return p
    raise FileNotFoundError(
        "No se encontró 'obj.names' ni 'labels.txt' en el zip. "
        "¿Exportaste desde CVAT en formato 'YOLO 1.1'?"
    )


def read_class_names(path: Path) -> list[str]:
    """Lee nombres de clase de obj.names / labels.txt (una por línea)."""
    if path.name == "obj.data":
        # obj.data: "classes = N", "names = obj.names" -> se lee obj.names al lado
        names_file = path.with_name("obj.names")
        lines = names_file.read_text(encoding="utf-8").splitlines()
    else:
        lines = path.read_text(encoding="utf-8").splitlines()
    names = [ln.strip() for ln in lines if ln.strip()]
    return names


def find_images_and_labels(raw: Path) -> tuple[list[Path], dict[str, Path]]:
    """Devuelve (lista de imágenes, {stem -> label .txt})."""
    # Buscar directorio obj_train_data (o cualquiera con .txt de etiquetas)
    label_dirs = [p.parent for p in raw.rglob("*.txt") if "obj" in p.parent.name.lower() or p.parent.name.lower() in ("obj_train_data", "labels")]
    # Fallback: todos los .txt que no sean obj.names/labels.txt/train.txt/valid.txt
    if not label_dirs:
        label_dirs = [p.parent for p in raw.rglob("*.txt")
                      if p.name not in ("obj.names", "labels.txt", "train.txt", "valid.txt", "obj.data")]

    if not label_dirs:
        raise FileNotFoundError("No se encontraron archivos .txt de etiquetas en el zip.")

    # Elegir el directorio con más .txt (debería ser obj_train_data)
    label_dir = max(set(label_dirs), key=lambda d: len(list(d.glob("*.txt"))))

    labels: dict[str, Path] = {}
    for txt in label_dir.glob("*.txt"):
        if txt.name.lower() in ("obj.names", "labels.txt", "train.txt", "valid.txt", "obj.data"):
            continue
        labels[txt.stem] = txt

    images: list[Path] = []
    for stem, _ in labels.items():
        img = None
        for ext in IMG_EXTS:
            cand = label_dir / f"{stem}{ext}"
            if cand.exists():
                img = cand
                break
        if img is None:
            # intentar en todo el raw
            for ext in IMG_EXTS:
                cands = list(raw.rglob(f"{stem}{ext}"))
                if cands:
                    img = cands[0]
                    break
        if img is None:
            raise FileNotFoundError(f"No se encontró la imagen para la etiqueta '{stem}.txt'")
        images.append(img)

    return images, labels


def make_split(n: int, holdout: int, val_frac: float, seed: int) -> tuple[list[int], list[int], list[int]]:
    """Divide índices 0..n-1 en holdout / train / val (determinístico)."""
    import random
    rng = random.Random(seed)
    idx = list(range(n))
    rng.shuffle(idx)
    ho = idx[:holdout]
    rest = idx[holdout:]
    n_val = int(round(len(rest) * val_frac))
    val = rest[:n_val]
    train = rest[n_val:]
    return train, val, ho


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--zip", type=Path, default=HERE / "llegada_dataset.zip")
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--holdout", type=int, default=20)
    ap.add_argument("--val-frac", type=float, default=0.2)
    ap.add_argument("--out", type=Path, default=OUT_DIR)
    args = ap.parse_args()

    zip_path = args.zip
    if not zip_path.exists():
        raise FileNotFoundError(
            f"No se encontró '{zip_path}'. Coloca el zip exportado de CVAT ahí "
            "o pásalo con --zip <ruta>."
        )

    expected = load_expected_classes()
    print(f"Clases esperadas ({len(expected)}): {expected}")

    # 1. Descomprimir
    RAW_DIR.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(zip_path) as z:
        z.extractall(RAW_DIR)
    print(f"Descomprimido en {RAW_DIR}")

    # 2. Leer clases reales
    classes_file = find_classes_file(RAW_DIR)
    classes = read_class_names(classes_file)
    print(f"Clases en el dataset ({len(classes)}): {classes}")

    if classes != expected:
        print("⚠️  ADVERTENCIA: el orden/lista de clases NO coincide con cvat_labels_pool.json.")
        print(f"   Esperado: {expected}")
        print(f"   Real:     {classes}")
        print("   El class_id del modelo mapea directo a ballID (0-9); revisa antes de entrenar.")
        # No aborta: se usa el orden real, pero se avisa.
    nc = len(classes)

    # 3. Localizar imágenes y etiquetas
    images, labels = find_images_and_labels(RAW_DIR)
    images = sorted(images, key=lambda p: p.name)
    n = len(images)
    print(f"Imágenes encontradas: {n}")
    if n == 0:
        raise SystemExit("No hay imágenes. Verifica el contenido del zip.")

    # 4. Split
    train_idx, val_idx, holdout_idx = make_split(n, args.holdout, args.val_frac, args.seed)
    print(f"Split -> train: {len(train_idx)} | val: {len(val_idx)} | holdout: {len(holdout_idx)}")

    # 5. Copiar a layout Ultralytics
    for split, idxs in (("train", train_idx), ("val", val_idx)):
        (args.out / "images" / split).mkdir(parents=True, exist_ok=True)
        (args.out / "labels" / split).mkdir(parents=True, exist_ok=True)
        for i in idxs:
            img = images[i]
            stem = img.stem
            shutil.copy2(img, args.out / "images" / split / img.name)
            shutil.copy2(labels[stem], args.out / "labels" / split / f"{stem}.txt")

    for i in holdout_idx:
        img = images[i]
        stem = img.stem
        (args.out / "holdout" / "images").mkdir(parents=True, exist_ok=True)
        (args.out / "holdout" / "labels").mkdir(parents=True, exist_ok=True)
        shutil.copy2(img, args.out / "holdout" / "images" / img.name)
        shutil.copy2(labels[stem], args.out / "holdout" / "labels" / f"{stem}.txt")

    # 6. Escribir data.yaml
    data_yaml = args.out / "data.yaml"
    names_block = "\n".join(f"  {i}: {name}" for i, name in enumerate(classes))
    abs_out = args.out.resolve()
    data_yaml.write_text(
        f"# Generado por prepare_dataset.py\n"
        f"path: {abs_out}\n"
        f"train: images/train\n"
        f"val: images/val\n"
        f"names:\n{names_block}\n",
        encoding="utf-8",
    )
    print(f"data.yaml -> {data_yaml}")
    print("Dataset listo para entrenar.")


if __name__ == "__main__":
    main()
