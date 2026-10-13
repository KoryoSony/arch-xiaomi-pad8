# Build environment.
#
#   docker build --platform linux/amd64 -t yupei-build .
#   docker run --rm -it --privileged --platform linux/amd64 \
#       -v "$PWD:/port" -v yupei-work:/port/work yupei-build ./build.sh all ROM.tgz
#
# work/ MUST be a Docker volume on macOS: the kernel tree has xt_MARK.h next to
# xt_mark.h and the rootfs has case-colliding files, which a case-insensitive
# APFS bind mount silently merges. Only out/ (the images) lands on the host.
#
# On Apple Silicon this image runs under x86_64 emulation: the kernel build is
# slow but works.
# --privileged is only needed for the rootfs step (loop mounts + binfmt chroot).
FROM archlinux:latest

RUN pacman -Syu --noconfirm --needed \
        base-devel bc cpio git python python-setuptools openssl perl \
        clang lld llvm kmod \
        android-tools erofs-utils e2fsprogs dosfstools mtools \
        arch-install-scripts qemu-user-static qemu-user-static-binfmt \
        file wget curl zstd lz4 libarchive \
    && pacman -Scc --noconfirm

WORKDIR /port
