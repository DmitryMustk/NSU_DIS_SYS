FROM debian:12-slim AS build
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        build-essential pkg-config librabbitmq-dev libmongoc-dev libbson-dev ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN gcc -O2 -o nob nob.c && ./nob

FROM debian:12-slim
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        librabbitmq4 libmongoc-1.0-0 libbson-1.0-0 ca-certificates wget \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /src/bin/manager /usr/local/bin/manager
EXPOSE 8080
USER nobody
ENTRYPOINT ["/usr/local/bin/manager"]
