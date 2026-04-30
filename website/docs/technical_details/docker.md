---
id: docker
title: Docker Container Deployments
---

## Dockerfiles

Usage:

Build with:

```sh
   docker build . -f container/Dockerfile -t redex
```

Assuming input files are placed in `~/project`. Run redex with:

```sh
docker run -it --rm -v ~/project:/input redex -P /input/proguard-rules.pro --config /input/default.config /input/input.apk -o /input/out.apk
```

Build and run tests with

```sh
docker build . --target test -f container/Dockerfile -t redex-test && docker run redex-test
```

Run

```sh
docker run -it --rm --entrypoint bash redex
```
