set -eu
apk add --no-cache --repository https://dl-cdn.alpinelinux.org/alpine/v3.24/main --repository https://dl-cdn.alpinelinux.org/alpine/v3.24/community flashrom
flashrom --version
flashrom --help
