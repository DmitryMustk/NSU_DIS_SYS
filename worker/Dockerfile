FROM debian:12-slim AS build
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        build-essential libomp-dev librabbitmq-dev python3 ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
# Generate dto.h / instpow.h non-interactively (alphabet=36 chars, maxPow=13)
RUN printf '36\n13\n' | python3 generate.py
# Build nob then build the worker
RUN gcc -O2 -o nob nob.c && ./nob

FROM debian:12-slim
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        librabbitmq4 libgomp1 ca-certificates \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /src/bin/worker /usr/local/bin/worker
USER nobody
ENTRYPOINT ["/usr/local/bin/worker"]
