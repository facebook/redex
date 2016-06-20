# Docker Container Deployments

## Dockerfiles and DockerHub Images

This section lists Dockerfiles and DockerHub images that provide various
deployments of Redex.

### Contribution by Andrew Chen (@yongjhih)
* GitHub source: https://github.com/yongjhih/docker-redex
* DockerHub image: https://hub.docker.com/r/yongjhih/redex/

Usage:
```sh
$ docker build --rm -t redex .
$ docker run -it -v $ANDROID_SDK:/opt/android-sdk-linux -v $(pwd):/redex redex redex path/to/your.apk -o path/to/output.apk
```

### Contribution by Sofian Hadiwijaya (@sofianhw) and Nicola Corti (@cortinico)
* GitHub source: https://github.com/sofianhw/docker-redex
* DockerHub image: https://hub.docker.com/r/sofianhw/redex/

Usage:
```sh
$ docker build --rm -t redex . or docker pull sofianhw/redex
$ docker run -v /your-apk-folder:/data/redex sofianhw/redex redex your-apk.apk -o out.apk
```
