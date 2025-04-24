#!/bin/bash
#
# Compile script for Neophyte kernel
# Copyright (C) 2025 k4ngcaribug

SECONDS=0 # builtin bash timer
ZIPNAME="Neophyte-Kernel-Vince-KSU-$(TZ=Asia/Kolkata date +"%Y%m%d-%H%M").zip"
ZIPNAME_KSU="Neophyte-Kernel-Vince-KSU-$(TZ=Asia/Jakarta date +"%Y%m%d-%H%M").zip"
TC_DIR="/workspace/gitpod/clang"
AK3_DIR="AnyKernel3"
DEFCONFIG="vendor/vince_defconfig"
export PATH="$TC_DIR/bin:$PATH"

# Check for essentials
if ! [ -d "${TC_DIR}" ]; then
echo "Clang not found! Cloning to ${TC_DIR}..."
if ! git clone --depth=1 https://gitlab.com/Panchajanya1999/azure-clang -b main ${TC_DIR}; then
echo "Cloning failed! Aborting..."
exit 1
fi
fi

if [[ $1 = "-r" || $1 = "--regen" ]]; then
make O=out ARCH=arm64 $DEFCONFIG savedefconfig
cp out/defconfig arch/arm64/configs/$DEFCONFIG
exit
fi

mkdir -p out
make O=out ARCH=arm64 $DEFCONFIG

if [[ $1 = "-k" || $1 = "--ksu" ]]; then
	echo -e "\nCleanup KernelSU first on local build\n"
	rm -rf KernelSU drivers/kernelsu
else
	echo -e "\nSet No KernelSU Install, just skip\n"
fi

# Set function for KSU variant
if [[ $1 = "-k" || $1 = "--ksu" ]]; then
echo -e "\nKSU Support, let's Make it On\n"
curl -LSs "https://raw.githubusercontent.com/KernelSu-Next/KernelSU-Next/next-susfs/kernel/setup.sh" | bash -s next-susfs
else
echo -e "\nKSU not Support, let's Skip\n"
fi

echo -e "\nStarting compilation...\n"
make -j$(nproc --all) CC=clang CROSS_COMPILE=aarch64-linux-gnu- CROSS_COMPILE_ARM32=arm-linux-gnueabi- O=out ARCH=arm64

if [ -f "out/arch/arm64/boot/Image.gz-dtb" ]; then
echo -e "\nKernel compiled succesfully! Zipping up...\n"
fi
cp out/arch/arm64/boot/Image.gz-dtb AnyKernel3
rm -f *zip
cd AnyKernel3
git checkout master &> /dev/null
if [[ $1 = "-k" || $1 = "--ksu" ]]; then
zip -r9 "../$ZIPNAME_KSU" * -x '*.git*' README.md *placeholder
else
zip -r9 "../$ZIPNAME" * -x '*.git*' README.md *placeholder
fi
cd ..
echo -e "Completed in $((SECONDS / 60)) minute(s) and $((SECONDS % 60)) second(s) !"
if [[ $1 = "-k" || $1 = "--ksu" ]]; then
echo "Zip: $ZIPNAME_KSU"
else
echo "Zip: $ZIPNAME"
fi
