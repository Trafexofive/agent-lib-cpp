FROM python:3.12-slim

WORKDIR /app
COPY app/server.py /app/server.py
ENV DIAGRAM_WORKSPACE_DATA=/data
EXPOSE 8127
CMD ["python3", "/app/server.py"]
