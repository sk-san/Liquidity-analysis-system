FROM python:3.11-slim
WORKDIR /opt/hft-platform
COPY packages/market-data-protocol packages/market-data-protocol
COPY components/pacing-server components/pacing-server
RUN pip install --no-cache-dir ./packages/market-data-protocol ./components/pacing-server
ENTRYPOINT ["abides-pacing-server"]
