"""
Train YOLO model for Metra ball tracking.

This script downloads the dataset from Roboflow and trains a YOLO model
with optimized settings for detection tasks.
"""

import os
from dotenv import load_dotenv
import sys
from pathlib import Path

load_dotenv()
# Add src to path for imports
sys.path.insert(0, str(Path(__file__).parent.parent.parent))

from roboflow import Roboflow
from ultralytics import YOLO

from src.utils.config import (
    get_roboflow_api_key,
    DATA_DATASETS_DIR,
    OUTPUTS_RUNS_DIR,
    ensure_dir_exists
)
from src.utils.logger import get_logger
from src.models.model_utils import export_model_to_tensorrt

logger = get_logger(__name__, log_file="train_model")


def download_dataset(
    workspace: str,
    project: str,
    version: int,
    dataset_format: str = "yolo26"
) -> str:
    """
    Download dataset from Roboflow.
    
    Args:
        workspace: Roboflow workspace name
        project: Project name
        version: Dataset version number
        dataset_format: Dataset format (default: "yolo26")
        
    Returns:
        Path to downloaded dataset
    """
    api_key = get_roboflow_api_key()
    if not api_key:
        raise ValueError(
            "Roboflow API key not found. "
            "Set ROBOFLOW_API_KEY environment variable."
        )
    
    logger.info(f"Downloading dataset from Roboflow: {workspace}/{project} v{version}")
    
    rf = Roboflow(api_key=api_key)
    project_obj = rf.workspace(workspace).project(project)
    version_obj = project_obj.version(version)
    
    # Download to data/datasets directory
    ensure_dir_exists(DATA_DATASETS_DIR)
    dataset = version_obj.download(dataset_format)
    
    print(f"Dataset downloaded to: {dataset.location}")
    
    logger.info(f"Dataset downloaded to: {dataset.location}")
    return dataset.location


def train_model(
    data_yaml_path: str,
    base_model: str = "yolo26m.pt",
    epochs: int = 50,
    imgsz = (1280, 720),
    device: str = "0",
    batch: int = 4,
    experiment_name: str = "yolo26m_run_1",
    close_mosaic: int = 10,
    lr0: float = 0.01,
    label_smoothing: float = 0.1,
    mixup: float = 0.1,
    fliplr: float = 0.5,
    hsv_s: float = 0.4,
    hsv_v: float = 0.4,
    lrf: float = 0.01,
    warmup_epochs: float = 3.0,
    **kwargs
) -> YOLO:
    """
    Train YOLO model with specified parameters.
    
    Args:
        data_yaml_path: Path to data.yaml file
        base_model: Base model to use (e.g., "yolov8l.pt")
        epochs: Number of training epochs
        imgsz: Image size for training
        device: Device to use ("0" for GPU, "cpu" for CPU)
        batch: Batch size
        experiment_name: Name for this training run
        **kwargs: Additional training parameters
        
    Returns:
        Trained YOLO model
    """
    logger.info(f"Loading base model: {base_model}")
    model = YOLO(base_model)
    
    # Ensure output directory exists
    ensure_dir_exists(OUTPUTS_RUNS_DIR / "detect")
    
    logger.info(f"Starting training: {experiment_name}")
    logger.info(f"  Epochs: {epochs}")
    logger.info(f"  Image size: {imgsz}")
    logger.info(f"  Batch size: {batch}")
    logger.info(f"  Device: {device}")
    
    # Default training parameters optimized for RTX GPUs
    train_params = {
        'data': data_yaml_path,
        'epochs': epochs,
        'imgsz': imgsz,
        'device': device,
        'batch': batch,
        'patience': 50,
        'optimizer': 'AdamW',
        'lr0': lr0,
        'lrf': lrf,
        'cos_lr': True,
        'close_mosaic': close_mosaic,
        'label_smoothing': label_smoothing,
        'amp': True,
        'workers': 8,
        'name': experiment_name,
        'project': str(OUTPUTS_RUNS_DIR / "detect"),
        'mixup': mixup,
        'fliplr': fliplr,
        'hsv_s': hsv_s,
        'hsv_v': hsv_v,
        'warmup_epochs': warmup_epochs
    }
    
    # Update with any additional parameters
    train_params.update(kwargs)
    
    # Train the model
    model.train(**train_params)
    
    logger.info("Training completed!")
    logger.info(f"Results saved to: {OUTPUTS_RUNS_DIR / 'detect' / experiment_name}")
    logger.info(f"Best model: {OUTPUTS_RUNS_DIR / 'detect' / experiment_name / 'weights' / 'best.pt'}")
    
    return model


def main():
    """Main training workflow."""
    # Configuration
    WORKSPACE = "metrabola"
    PROJECT = "carrera-de-bolas"
    DATASET_VERSION = 6
    BASE_MODEL = "yolo26m.pt"
    """ EPOCHS = 50
    IMAGE_SIZE = (1920, 1080)
    BATCH_SIZE = 4
    EXPERIMENT_NAME = "yolov26m_run_1"
    CLOSE_MOSAIC = 10
    LR0 = 0.01
    LRF = 0.01
    LABEL_SMOOTHING = 0.1
    MIXUP = 0.1
    FLIPLR = 0.5
    HSV_S = 0.4
    HSV_V = 0.4
    WARMUP_EPOCHS = 3.0 """
    EPOCHS = 300
    IMAGE_SIZE = [1088, 1920]   # h, w
    BATCH_SIZE = 32             # try 3 only if VRAM allows
    EXPERIMENT_NAME = "yolov26m_run_1"
    CLOSE_MOSAIC = 20
    LR0 = 0.01
    LRF = 0.0001
    LABEL_SMOOTHING = 0.1
    MIXUP = 0.0
    FLIPLR = 0.5
    HSV_S = 0.4
    HSV_V = 0.4
    WARMUP_EPOCHS = 5.0
    CACHE = True
    WORKERS = 12
    MOSAIC=0
    OPTIMIZER = "AdamW"
    
    
    logger.info("=" * 60)
    logger.info("YOLO Model Training - Metra Ball Tracker")
    logger.info("=" * 60)
    
    # Step 1: Download dataset
    logger.info("\n[1/3] Downloading dataset from Roboflow...")
    dataset_location = download_dataset(WORKSPACE, PROJECT, DATASET_VERSION)
    # dataset_location is the correct folder (from dataset.location), data.yaml is inside it
    data_yaml = os.path.join(dataset_location, "data.yaml")
    
    print(f"Dataset ready at: {dataset_location}") 
    
    # Step 2: Train model
    logger.info("\n[2/3] Training model...")
    model = train_model(
        data_yaml_path=data_yaml,
        base_model=BASE_MODEL,
        epochs=EPOCHS,
        imgsz=IMAGE_SIZE,
        batch=BATCH_SIZE,
        experiment_name=EXPERIMENT_NAME,
        close_mosaic=CLOSE_MOSAIC,
        lr0=LR0,
        lrf=LRF,
        label_smoothing=LABEL_SMOOTHING,
        mixup=MIXUP,
        fliplr=FLIPLR,
        hsv_s=HSV_S,
        hsv_v=HSV_V,
        warmup_epochs=WARMUP_EPOCHS,
        rect=True,  # use rectangular training for better aspect ratio handling
        cache=CACHE,
        workers=WORKERS,
        mosaic=MOSAIC,
        optimizer=OPTIMIZER
    )
    
    # Step 3: Export to TensorRT
    logger.info("\n[3/3] Exporting model to TensorRT engine...")
    try:
        export_model_to_tensorrt(model, imgsz=IMAGE_SIZE, half=True, dynamic=True)
        logger.info("Model exported successfully!")
    except Exception as e:
        logger.warning(f"Failed to export to TensorRT: {e}")
        logger.warning("You can export manually later using scripts/export/export_to_tensorrt.py")
    
    logger.info("\n" + "=" * 60)
    logger.info("Training workflow completed successfully!")
    logger.info("=" * 60)


if __name__ == "__main__":
    main()
