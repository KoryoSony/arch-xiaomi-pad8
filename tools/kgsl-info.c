// kgsl-info: print the Adreno chip id and GMEM size from KGSL (debug helper).
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

struct kgsl_devinfo {
	unsigned int device_id, chip_id, mmu_enabled;
	unsigned long gmem_gpubaseaddr;
	unsigned int gpu_id;
	size_t gmem_sizebytes;
};
struct kgsl_device_getproperty { unsigned int type; void *value; size_t sizebytes; };
#define IOCTL_KGSL_DEVICE_GETPROPERTY _IOWR(0x09, 0x2, struct kgsl_device_getproperty)
#define KGSL_PROP_DEVICE_INFO 0x1
#define KGSL_PROP_UCHE_GMEM_VADDR 0x13
#define KGSL_PROP_HIGHEST_BANK_BIT 0x17
#define KGSL_PROP_UBWC_MODE 0x1B

static int prop(int fd, unsigned type, void *v, size_t n)
{
	struct kgsl_device_getproperty p = { type, v, n };
	return ioctl(fd, IOCTL_KGSL_DEVICE_GETPROPERTY, &p);
}

int main(void)
{
	int fd = open("/dev/kgsl-3d0", O_RDWR);
	if (fd < 0) { perror("/dev/kgsl-3d0"); return 1; }
	struct kgsl_devinfo d = {0};
	if (prop(fd, KGSL_PROP_DEVICE_INFO, &d, sizeof d) < 0) { perror("DEVICE_INFO"); return 1; }
	printf("device_id=%u chip_id=0x%08x gpu_id=%u gmem=%zu KiB gmem_base=0x%lx\n",
	       d.device_id, d.chip_id, d.gpu_id, d.gmem_sizebytes / 1024, d.gmem_gpubaseaddr);
	unsigned int v = 0;
	if (!prop(fd, KGSL_PROP_HIGHEST_BANK_BIT, &v, sizeof v)) printf("highest_bank_bit=%u\n", v);
	if (!prop(fd, KGSL_PROP_UBWC_MODE, &v, sizeof v)) printf("ubwc_mode=%u\n", v);
	uint64_t va = 0;
	if (!prop(fd, KGSL_PROP_UCHE_GMEM_VADDR, &va, sizeof va)) printf("uche_gmem_vaddr=0x%llx\n", (unsigned long long)va);
	return 0;
}
