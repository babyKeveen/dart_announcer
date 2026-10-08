FROM python:3.13-slim

ENV TZ=Europe/Dublin
RUN apt-get update && apt-get install -y --no-install-recommends tzdata \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY dart_announce ./dart_announce

EXPOSE 8000

CMD ["uvicorn", "dart_announce.app:app", "--host", "0.0.0.0", "--port", "8000"]
