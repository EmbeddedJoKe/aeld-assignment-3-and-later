#!/bin/bash
# Script outline to install and build kernel.
# Author: Siddhant Jajoo.

set -e
set -u

OUTDIR=/tmp/aeld
KERNEL_REPO=git://git.kernel.org/pub/scm/linux/kernel/git/stable/linux-stable.git
KERNEL_VERSION=v5.15.163
BUSYBOX_VERSION=1_33_1
FINDER_APP_DIR=$(realpath $(dirname $0))
ARCH=arm64
CROSS_COMPILE=aarch64-none-linux-gnu-

if [ $# -lt 1 ]
then
	echo "Using default directory ${OUTDIR} for output"
else
	OUTDIR=$1
	echo "Using passed directory ${OUTDIR} for output"
fi

# Create the output directory
mkdir -p "${OUTDIR}"

# Convert the output directory to an absolute path
OUTDIR=$(realpath "${OUTDIR}")

cd "$OUTDIR"
if [ ! -d "${OUTDIR}/linux-stable" ]; then
    #Clone only if the repository does not exist.
	echo "CLONING GIT LINUX STABLE VERSION ${KERNEL_VERSION} IN ${OUTDIR}"
	git clone --depth 1 --single-branch \
    --branch "${KERNEL_VERSION}" \
    "${KERNEL_REPO}" "${OUTDIR}/linux-stable"
fi
if [ ! -e ${OUTDIR}/linux-stable/arch/${ARCH}/boot/Image ]; then
    cd linux-stable
    echo "Checking out version ${KERNEL_VERSION}"
    git checkout ${KERNEL_VERSION}

    # TODO: Add your kernel build steps here
    # Configure the ARM64 kernel with default settings
    make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} defconfig

    # Build the ARM64 Linux kernel image
    make -j$(nproc) ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} Image
fi

echo "Adding the Image in outdir"
#QEMU scripts expect kernal at /tmp/aeld/Image rather than inside kernel source directory
cp "${OUTDIR}/linux-stable/arch/${ARCH}/boot/Image" "${OUTDIR}/Image"

echo "Creating the staging directory for the root filesystem"
cd "$OUTDIR"
if [ -d "${OUTDIR}/rootfs" ]
then
	echo "Deleting rootfs directory at ${OUTDIR}/rootfs and starting over"
    sudo rm  -rf ${OUTDIR}/rootfs
fi

# TODO: Create necessary base directories
# Create the root filesystem directory structure
mkdir -p "${OUTDIR}/rootfs"

cd "${OUTDIR}/rootfs"

mkdir -p bin dev etc home lib lib64 proc sbin sys tmp usr/bin usr/lib usr/sbin var/log

cd "$OUTDIR"

# Clone BusyBox if it does not already exist
if [ ! -d "${OUTDIR}/busybox" ]; then
    git clone https://git.busybox.net/busybox "${OUTDIR}/busybox"
    cd "${OUTDIR}/busybox"
    git checkout "${BUSYBOX_VERSION}"
else
    cd "${OUTDIR}/busybox"
fi

# Configure BusyBox if configuration is missing
if [ ! -f .config ]; then
    make distclean
    make ARCH="${ARCH}" CROSS_COMPILE="${CROSS_COMPILE}" defconfig
fi

# Build BusyBox for ARM64
make ARCH="${ARCH}" CROSS_COMPILE="${CROSS_COMPILE}" -j2

# Install BusyBox into the target root filesystem
make ARCH="${ARCH}" CROSS_COMPILE="${CROSS_COMPILE}" \
    CONFIG_PREFIX="${OUTDIR}/rootfs" install

# Check ARM64 BusyBox library dependencies
echo "Library dependencies"

${CROSS_COMPILE}readelf -l "${OUTDIR}/rootfs/bin/busybox" | grep "interpreter"
${CROSS_COMPILE}readelf -d "${OUTDIR}/rootfs/bin/busybox" | grep "NEEDED"

# TODO: Add library dependencies to rootfs
# Locate the ARM64 toolchain sysroot
SYSROOT=$(${CROSS_COMPILE}gcc -print-sysroot)

echo "Copying ARM64 shared libraries from ${SYSROOT}"

# Create library directories in the target root filesystem
mkdir -p "${OUTDIR}/rootfs/lib"
mkdir -p "${OUTDIR}/rootfs/lib64"

# Copy the ARM64 dynamic loader
cp -L "${SYSROOT}/lib/ld-linux-aarch64.so.1" \
    "${OUTDIR}/rootfs/lib/"

# Copy the ARM64 runtime libraries
cp -a "${SYSROOT}/lib64/." \
    "${OUTDIR}/rootfs/lib64/"

# Copy any libraries located in the sysroot's lib directory
cp -a "${SYSROOT}/lib/." \
    "${OUTDIR}/rootfs/lib/"

# TODO: Make device nodes
# Create device nodes in the target root filesystem
echo "Creating device nodes"

# Create /dev/null
sudo mknod -m 666 "${OUTDIR}/rootfs/dev/null" c 1 3

# Create /dev/console
sudo mknod -m 600 "${OUTDIR}/rootfs/dev/console" c 5 1

# TODO: Clean and build the writer utility
# Clean and build the writer utility for ARM64
echo "Building writer for ARM64"

cd "${FINDER_APP_DIR}"

# Remove the previous writer executable and object files
make clean

# Cross-compile writer using the ARM64 toolchain
make CROSS_COMPILE="${CROSS_COMPILE}"

# Copy the ARM64 executable into the target root filesystem
cp writer "${OUTDIR}/rootfs/home/writer"

# TODO: Copy the finder related scripts and executables to the /home directory
# on the target rootfs
# Copy finder scripts and configuration files to the root filesystem
echo "Copying finder application files"

# Create the configuration directory
mkdir -p "${OUTDIR}/rootfs/home/conf"

# Copy finder scripts
cp "${FINDER_APP_DIR}/finder.sh" \
   "${OUTDIR}/rootfs/home/"

cp "${FINDER_APP_DIR}/finder-test.sh" \
   "${OUTDIR}/rootfs/home/"

cp "${FINDER_APP_DIR}/autorun-qemu.sh" \
   "${OUTDIR}/rootfs/home/"

# Copy configuration files
cp "${FINDER_APP_DIR}/../conf/username.txt" \
   "${OUTDIR}/rootfs/home/conf/"

cp "${FINDER_APP_DIR}/../conf/assignment.txt" \
   "${OUTDIR}/rootfs/home/conf/"

# Ensure scripts are executable
chmod +x "${OUTDIR}/rootfs/home/finder.sh"
chmod +x "${OUTDIR}/rootfs/home/finder-test.sh"
chmod +x "${OUTDIR}/rootfs/home/autorun-qemu.sh"
chmod +x "${OUTDIR}/rootfs/home/writer"

# TODO: Chown the root directory
# Set root ownership for the target root filesystem
echo "Setting root filesystem ownership"

sudo chown -R root:root "${OUTDIR}/rootfs"

# TODO: Create initramfs.cpio.gz
# Create the compressed initramfs archive
echo "Creating initramfs.cpio.gz"

# Move into the root filesystem staging directory
cd "${OUTDIR}/rootfs"

# Package all rootfs contents into a cpio archive and compress it
find . -print0 | cpio --null -ov --format=newc | gzip -9 > "${OUTDIR}/initramfs.cpio.gz"

echo "Initramfs created at ${OUTDIR}/initramfs.cpio.gz"
