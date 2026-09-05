FROM python:3.12-slim

WORKDIR /app

ENV TORCH_HOME=/app/.cache/torch

RUN apt-get update && apt-get install -y --no-install-recommends \
      libjpeg62-turbo zlib1g \
 && rm -rf /var/lib/apt/lists/*

COPY analysis/requirements.txt .

RUN pip install --no-cache-dir --index-url https://download.pytorch.org/whl/cpu torch torchvision \
 && pip install --no-cache-dir -r requirements.txt

RUN python3 -c "import lpips; lpips.LPIPS(net='alex')"

COPY bin/imgpoison bin/imgpoison
COPY analysis/run_c_paired_suite.py run_c_paired_suite.py
COPY analysis/validate_interpolation.py validate_interpolation.py
COPY analysis/sweep_gamma_attack.py sweep_gamma_attack.py
COPY analysis/run_all.sh run_all.sh

RUN chmod +x run_all.sh

COPY images/ img/coco_val_100/

RUN useradd --create-home appuser && chown -R appuser:appuser /app
USER appuser

CMD ["./run_all.sh"]

