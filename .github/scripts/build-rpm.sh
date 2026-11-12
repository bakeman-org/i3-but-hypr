#!/usr/bin/env bash
set -euo pipefail

# Runs inside a Fedora container (see .github/workflows/package.yml).
# The repository is mounted at /src.

dnf install -y --setopt=install_weak_deps=False \
  meson ninja-build gcc pkgconf-pkg-config git ca-certificates curl tar gzip \
  jq gettext asciidoc xmlto docbook-dtds docbook-style-xsl perl perl-Pod-Simple python3 \
  libX11-devel libxcb-devel xcb-util-devel xcb-util-cursor-devel \
  xcb-util-keysyms-devel xcb-util-wm-devel xcb-util-xrm-devel \
  libxkbcommon-devel libxkbcommon-x11-devel startup-notification-devel \
  pango-devel cairo-devel yajl-devel libev-devel lua-devel pcre2-devel

# Fedora's default lua-devel is normally 5.4; make the versioned package
# explicit if the pkg-config name is missing.
if ! pkg-config --exists lua5.4; then
  dnf install -y lua5.4-devel
fi

meson setup build-rpm --prefix=/usr --buildtype=release -Dmans=true
ninja -C build-rpm
meson test -C build-rpm i3ext_scan
DESTDIR=/src/pkg-rpm ninja -C build-rpm install

curl -fsSL \
  "https://github.com/goreleaser/nfpm/releases/download/v${NFPM_VERSION}/nfpm_${NFPM_VERSION}_Linux_x86_64.tar.gz" \
  -o /tmp/nfpm.tgz
tar -xzf /tmp/nfpm.tgz -C /usr/local/bin nfpm

VERSION=$(meson introspect build-rpm --projectinfo | jq -r .version)
case "$VERSION" in
  *.*.*) ;;
  *) VERSION="${VERSION}.0" ;;
esac
export VERSION RELEASE="${GITHUB_RUN_NUMBER}" ARCH=amd64

mkdir -p /src/dist
envsubst '${VERSION} ${RELEASE} ${ARCH}' \
  < /src/.github/packaging/nfpm.yaml > /tmp/nfpm.yaml
nfpm package --config /tmp/nfpm.yaml --packager rpm --target /src/dist/
