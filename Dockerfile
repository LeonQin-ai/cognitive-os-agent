FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends build-essential cmake python3 \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY backend/CMakeLists.txt backend/CMakeLists.txt
COPY backend/src backend/src
COPY backend/include backend/include
COPY backend/third_party backend/third_party
COPY backend/apps backend/apps
COPY backend/tools backend/tools
COPY backend/cli backend/cli
COPY backend/tests backend/tests
RUN cmake -S backend -B /build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build /build --target cognitive-os-agent -j 2

FROM debian:bookworm-slim AS runtime
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates libcurl4 curl git python3 \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd --gid 10001 cognitive && useradd --uid 10001 --gid 10001 --home-dir /state cognitive \
    && install -d -o cognitive -g cognitive -m 0700 /state \
    && install -d -o cognitive -g cognitive -m 0750 /workspace
COPY --from=build /build/cognitive-os-agent /usr/local/bin/cognitive-os-agent
COPY deploy/container_entrypoint.py /usr/local/bin/container-entrypoint.py
USER 10001:10001
ENV HOME=/state PYTHONDONTWRITEBYTECODE=1 PYTHONUNBUFFERED=1
WORKDIR /workspace
EXPOSE 18300
HEALTHCHECK --interval=10s --timeout=3s --start-period=10s --retries=6 \
    CMD curl --fail --silent http://127.0.0.1:18300/v1/scheduler > /dev/null || exit 1
ENTRYPOINT ["python3", "/usr/local/bin/container-entrypoint.py"]
CMD ["serve", "18300"]
