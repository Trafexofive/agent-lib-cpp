FROM python:3.12-slim

WORKDIR /app
# Bundled renderer (used by /diagram/render and optional SSE preview text).
# Build context must be the repo root so these relative paths resolve.
COPY playground/diagram-junky/manifests/relics/diagram-workspace/app/server.py /app/server.py
COPY playground/diagram-junky/diagram_junky/ /app/diagram_junky/
ENV DIAGRAM_WORKSPACE_DATA=/data
EXPOSE 8127
CMD ["python3", "/app/server.py"]
