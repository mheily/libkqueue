#!/bin/sh
set -e

container_name="libkqueue-dev"
dev_image="libkqueue-clang-cmake-dev:latest"
platform="linux/amd64"

# Verbosity
verbose=0

debug() {
  if [ "$verbose" -eq 1 ]; then
    echo "$@"
  fi
}

error() {
  echo "$@" >&2;
}

# Parse flags
while [ $# -gt 0 ]; do
  case "$1" in
    -v)
      verbose=1
      shift
      ;;
    start|stop|restart|status)
      cmd="$1"
      shift
      ;;
    *)
      echo "Usage: $0 [-v] {start|stop|restart|status}"
      exit 1
      ;;
  esac
done

[ -z "$cmd" ] && cmd="start"

hash_file() {
  # portable sha256
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$1" | awk '{print $1}'
  else
    # macOS has openssl by default
    openssl dgst -sha256 "$1" | awk '{print $NF}'
  fi
}

image_build() {
  [ -f Dockerfile ] || { error "no Dockerfile in $(pwd)"; exit 1; }
  hash=$(hash_file Dockerfile)
  debug "(re-)building image with $platform -t $dev_image --build-arg IMAGE_FINGERPRINT=$hash ."
  docker build --platform=$platform -t "$dev_image" --build-arg "IMAGE_FINGERPRINT=$hash" .
}

run_container() {
    debug "Starting container '$container_name' with image '$dev_image'"
    docker run --rm -it \
      --platform=${platform} \
      --name "$container_name" \
      --hostname "$container_name" \
      -u "$(id -u)":"$(id -g)" \
      -e HOME=/home/dev \
      -w /home/dev \
      -v "$PWD":/home/dev \
      "$dev_image"
}

start_container() {
  expected=$(hash_file Dockerfile)
  current=$(docker image inspect "$dev_image" \
            --format '{{ index .Config.Labels "dev.fingerprint" }}' 2>/dev/null || true)

  if [ -z "$current" ]; then
    debug "Image missing or unlabeled, building"
    image_build
    run_container
  fi

  if [ "$current" != "$expected" ]; then
    debug "fingerprint changed ($current -> $expected), rebuilding"
    image_build
    docker rm -f "$container_name"
    start_container
  fi

  if ! docker ps -a --format '{{.Names}}' | grep -q "^${container_name}$"; then
    run_container
  else
    debug "Getting shell on existing container '$container_name'"
    docker exec -it ${container_name} /bin/bash
  fi
}

stop_container() {
  if docker ps -a --format '{{.Names}}' | grep -q "^${container_name}$"; then
    debug "Stopping and removing container '$container_name'"
    docker rm -f "$container_name" > /dev/null
  else
    debug "Container '$container_name' is not running"
  fi
}

status_container() {
  if docker ps --format '{{.Names}}' | grep -q "^${container_name}$"; then
    echo "Container '$container_name' is running"
  else
    echo "Container '$container_name' is not running"
    exit 1
  fi
}

case "$cmd" in
  start)
    start_container
    ;;
  stop)
    stop_container
    ;;
  restart)
    stop_container
    start_container
    ;;
  status)
    status_container
    ;;
esac
