#!/bin/bash
# Builds OrbisInstrumentalizer.prx inside an Ubuntu container, no local toolchain needed.
# Downloads the OpenOrbis toolchain and the GoldHEN plugin SDK into /cache on the first run.
#
#   docker run --rm -v "<repo>:/work/OrbisInstrumentalizer" -v "<cache dir>:/cache" \
#       ubuntu:24.04 bash /work/OrbisInstrumentalizer/tools/build-docker.sh
set -euo pipefail

apt-get update -qq >/dev/null
DEBIAN_FRONTEND=noninteractive apt-get install -y -qq clang lld llvm make wget git ca-certificates >/dev/null

export OO_PS4_TOOLCHAIN=/cache/OpenOrbis/PS4Toolchain
export GOLDHEN_SDK=/cache/GoldHEN_Plugins_SDK

if [ ! -d "$OO_PS4_TOOLCHAIN" ]; then
    mkdir -p /cache/OpenOrbis
    wget -q -O /tmp/toolchain.tar.gz https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/releases/download/v0.5.4/toolchain-llvm-18.tar.gz
    # the archive contains OpenOrbis/PS4Toolchain
    tar -xzf /tmp/toolchain.tar.gz -C /cache
fi
echo "toolchain: $(ls $OO_PS4_TOOLCHAIN | tr '\n' ' ')"
chmod +x "$OO_PS4_TOOLCHAIN"/bin/linux/* || true

if [ ! -f "$GOLDHEN_SDK/libGoldHEN_Hook.a" ]; then
    rm -rf "$GOLDHEN_SDK"
    git clone -q --depth 1 https://github.com/GoldHEN/GoldHEN_Plugins_SDK "$GOLDHEN_SDK"
    make -C "$GOLDHEN_SDK" >/tmp/sdk-build.log 2>&1 || { tail -40 /tmp/sdk-build.log; exit 1; }
fi

# plugins include the SDK headers as <GoldHEN/...>
mkdir -p "$GOLDHEN_SDK/include/GoldHEN"
cp "$GOLDHEN_SDK"/include/*.h "$GOLDHEN_SDK/include/GoldHEN/"

cd /work/OrbisInstrumentalizer
make clean >/dev/null
make 2>&1
ls -la bin/
