FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    pkg-config \
    libopencv-dev \
    libsqlite3-dev \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . .

RUN cmake -B build \
          -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG" \
          -DBUILD_TESTING=OFF && \
    cmake --build build --config Release -j$(nproc) --target main

RUN strip /app/build/main


# Production Runtime Image
FROM ubuntu:24.04 AS runner

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    libopencv-core406t64 \
    libopencv-imgproc406t64 \
    libopencv-videoio406t64 \
    libopencv-highgui406t64 \
    libopencv-imgcodecs406t64 \
    libsqlite3-0 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY --from=builder /app/build/main /app/EdgeStream
COPY --from=builder /app/assets /app/assets

RUN mkdir -p /app/database

VOLUME ["/app/database"]

ENTRYPOINT ["/app/EdgeStream"]
CMD ["assets/sample_video.mp4"]
