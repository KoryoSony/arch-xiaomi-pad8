# Build environment. Kernel builds need a case-sensitive filesystem (the tree
# has xt_MARK.h and xt_mark.h side by side), so this also works on macOS hosts.
#
#   docker build -t yupei-build .
#   docker run --rm -it --privileged -v "$PWD:/port" yupei-build ./build.sh all
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
