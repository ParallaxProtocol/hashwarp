# Hashwarp container image.
#
#   docker build -f packaging/docker/Dockerfile -t hashwarp .
#   docker run --rm --gpus all hashwarp --pool pool.example.com:3333 --wallet 0xYourAddress
#
# Running on NVIDIA GPUs needs the NVIDIA Container Toolkit on the host.
ARG CUDA_VERSION=12.9.1

FROM nvidia/cuda:${CUDA_VERSION}-devel-ubuntu22.04 AS build
RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      build-essential cmake git python3 ca-certificates mesa-common-dev \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DHASHWARP_TESTS=ON \
 && cmake --build build -j"$(nproc)" \
 && ctest --test-dir build --output-on-failure \
 && strip build/hashwarp/hashwarp

FROM nvidia/cuda:${CUDA_VERSION}-runtime-ubuntu22.04
RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      ocl-icd-libopencl1 ca-certificates \
 && rm -rf /var/lib/apt/lists/* \
 && mkdir -p /etc/OpenCL/vendors \
 && echo "libnvidia-opencl.so.1" > /etc/OpenCL/vendors/nvidia.icd
COPY --from=build /src/build/hashwarp/hashwarp /usr/local/bin/hashwarp
ENV NVIDIA_DRIVER_CAPABILITIES=compute,utility
EXPOSE 21550
ENTRYPOINT ["hashwarp"]
CMD ["--help"]
