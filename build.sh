#!/usr/bin/env bash
#
# Use this script on root of kernel directory

SECONDS=0 # builtin bash timer
ZIPNAME="neophyte-EOL-vince-$(TZ=Asia/Jakarta date +"%Y%m%d-%H%M").zip"
TC_DIR="$(pwd)/../tc/"
CLANG_DIR="${TC_DIR}clang"
AK3_DIR="AnyKernel3"
DEFCONFIG="vendor/vince_defconfig"

export PATH="$CLANG_DIR/bin:$PATH"
export LD_LIBRARY_PATH="$CLANG_DIR/lib:$LD_LIBRARY_PATH"
export KBUILD_BUILD_VERSION="1"
export LOCALVERSION

if ! [ -d "${CLANG_DIR}" ]; then
echo "Clang not found! Cloning to ${TC_DIR}..."
if ! git clone --depth=1 -b main https://gitlab.com/Panchajanya1999/azure-clang ${CLANG_DIR}; then
echo "Cloning failed! Aborting..."
exit 1
fi
fi

mkdir -p out
make O=out ARCH=arm64 $DEFCONFIG

echo -e "\nStarting compilation...\n"
make -j$(nproc --all) O=out \
					  ARCH=arm64 \
					  CC=clang \
					  LD=ld.lld \
					  AR=llvm-ar \
					  AS=llvm-as \
					  NM=llvm-nm \
					  OBJCOPY=llvm-objcopy \
					  OBJDUMP=llvm-objdump \
					  STRIP=llvm-strip \
					  CROSS_COMPILE=aarch64-linux-gnu- \
					  CROSS_COMPILE_ARM32=arm-linux-gnueabi-

rm -f AnyKernel3/Image.gz-dtb
cp out/arch/arm64/boot/Image.gz-dtb AnyKernel3
rm -f *zip
cd AnyKernel3
git checkout main &> /dev/null
zip -r9 "../$ZIPNAME" * -x '*.git*' README.md *placeholder
cd ..
rm -rf out/arch/arm64/boot
echo -e "Completed in $((SECONDS / 60)) minute(s) and $((SECONDS % 60)) second(s) !"
echo "Zip: $ZIPNAME"
echo -e "======================================="
