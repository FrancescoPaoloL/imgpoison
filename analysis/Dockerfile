FROM python:3.12-slim

WORKDIR /app

ENV TORCH_HOME=/app/.cache/torch

COPY requirements.txt .

RUN pip install --no-cache-dir --index-url https://download.pytorch.org/whl/cpu torch torchvision \
 && pip install --no-cache-dir -r requirements.txt

RUN python3 -c "import lpips; lpips.LPIPS(net='alex')"

COPY images/ ./images/

COPY mask_proto_azure.py .

RUN useradd --create-home appuser && chown -R appuser:appuser /app
USER appuser

CMD ["python3", "mask_proto_azure.py"]

