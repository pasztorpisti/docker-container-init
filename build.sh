#!/bin/bash
set -euo pipefail
ORIG_DIR="$( pwd )"
cd "$( dirname "${BASH_SOURCE[0]}" )"

if ! type docker >&/dev/null; then
	echo "You have to install docker-engine to run this script."
	exit 1
fi

CONTAINER_SRC_DIR=/src_dir
CONTAINER_OUTPUT_DIR=/output_dir

# The docker image name. e.g: "debian", "ubuntu", "ubuntu:16.04"
# This build script requires an image that has /bin/bash.
IMAGE=${IMAGE-debian}
IMAGE_ARR=(${IMAGE//:/ })

CFLAGS=${CFLAGS-"-Wall"}

if [ "${STATIC_LINK:-0}" -eq 1 ]; then
	CFLAGS+=" -static"
fi

if [[ " centos fedora " =~ " ${IMAGE_ARR[0]} " ]]; then
	DEFAULT_PKG_TOOL=yum
elif [[ " opensuse " =~ " ${IMAGE_ARR[0]} " ]]; then
	DEFAULT_PKG_TOOL=zypper
else
	DEFAULT_PKG_TOOL=apt
fi

PKG_TOOL=${PKG_TOOL:-${DEFAULT_PKG_TOOL}}

if [ "${PKG_TOOL}" == "yum" ]; then
	CC_INSTALL_COMMAND=${CC_INSTALL_COMMAND-"yum -y install gcc glibc-static"}
elif [ "${PKG_TOOL}" == "zypper" ]; then
	CC_INSTALL_COMMAND=${CC_INSTALL_COMMAND-"zypper install -y gcc glibc-static"}
elif [ "${PKG_TOOL}" == "apt" ]; then
	CC_INSTALL_COMMAND=${CC_INSTALL_COMMAND-"apt-get update && apt-get -y install gcc"}
else
	echo "Unsupported DEFAULT_PKG_TOOL value. Consider using CC_INSTALL_COMMAND instead."
	exit 1
fi

if [ -z "${SOURCE_VERSION+x}"] && [ -d ".git" ] && type git >&/dev/null; then
	if [ -z "$( git status -s docker-container-init.c )" ]; then
		SOURCE_VERSION="$( git rev-parse HEAD )"
	fi
fi
if [ -n "${SOURCE_VERSION+x}" ]; then
	VERSION_FLAG="-DVERSION=\\\"${SOURCE_VERSION}\\\""
fi


OUTPUT_FILENAME=${OUTPUT_FILENAME-"$( pwd )/docker-container-init"}
OUTPUT_DIR="$( cd "${ORIG_DIR}" && cd "$( dirname "${OUTPUT_FILENAME}" )" && pwd )"
CONTAINER_OUTPUT_FILENAME="${CONTAINER_OUTPUT_DIR}/$( basename "${OUTPUT_FILENAME}" )"

SCRIPT_INSIDE_DOCKER="
set -xeuo pipefail
${CC_INSTALL_COMMAND}

cc -std=c99 ${CFLAGS} ${VERSION_FLAG-} -o '${CONTAINER_OUTPUT_FILENAME}' '${CONTAINER_SRC_DIR}/docker-container-init.c'
chown ${EUID}:${EUID} '${CONTAINER_OUTPUT_FILENAME}'
"

echo "Building docker-container-init for ${IMAGE}..."

echo -n "${SCRIPT_INSIDE_DOCKER}" | docker run \
	--rm \
	-i \
	-v `pwd`:${CONTAINER_SRC_DIR} \
	-v "${OUTPUT_DIR}:${CONTAINER_OUTPUT_DIR}" \
	${IMAGE} \
		/bin/bash

echo "Finished building for ${IMAGE}. Binary location: ${OUTPUT_FILENAME}"
