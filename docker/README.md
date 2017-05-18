# docker images for redex

This directory, `docker/` inside the redex repo,
contains a docker file to install redex within a
[docker](https://www.docker.com/) container. This can be used to
quickly try redex or to deploy redex.


## Pre-requisites

To use this docker image, you will need a working docker
installation. See the instructions for
[Linux](http://docs.docker.com/linux/step_one/) or
[MacOSX](http://docs.docker.com/mac/step_one/) as appropriate.


## How to use

This docker file will use current commit of redex.

```sh
docker build --rm -t redex:android -f docker/android/Dockerfile .
./docker/redex path/to/your.apk -o path/to/output.apk
```
