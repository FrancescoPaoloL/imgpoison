#!/bin/sh
set -e

echo "=== 1/3: paired suite (Wilcoxon/median/bootstrap, raw + interpolated) ==="
python3 run_c_paired_suite.py img/coco_val_100/ --psnr-target 42.3 --seed 42

echo ""
echo "=== 2/3: interpolation validation (Task 1) ==="
python3 validate_interpolation.py img/coco_val_100/ c_paired_results.json --n 20

echo ""
echo "=== 3/3: gamma x attack sweep (Task 4) ==="

SAMPLE_IMAGE=$(ls img/coco_val_100/*.jpg | head -1)
echo "using $SAMPLE_IMAGE"
python3 sweep_gamma_attack.py "$SAMPLE_IMAGE"

