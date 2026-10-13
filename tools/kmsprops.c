// kmsprops: list property names/values of a DRM object (debug helper).
// Usage: kmsprops <obj id> <crtc|plane|connector>
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#define U64(p) ((uint64_t)(uintptr_t)(p))
int main(int argc, char **argv)
{
	int fd = open("/dev/dri/card0", O_RDWR);
	struct drm_set_client_cap cap = { DRM_CLIENT_CAP_ATOMIC, 1 };
	ioctl(fd, DRM_IOCTL_SET_CLIENT_CAP, &cap);
	uint32_t type = !strcmp(argv[2], "crtc") ? DRM_MODE_OBJECT_CRTC :
			!strcmp(argv[2], "plane") ? DRM_MODE_OBJECT_PLANE : DRM_MODE_OBJECT_CONNECTOR;
	struct drm_mode_obj_get_properties gp = { .obj_id = atoi(argv[1]), .obj_type = type };
	ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &gp);
	uint32_t *ids = calloc(gp.count_props + 1, 4); uint64_t *vals = calloc(gp.count_props + 1, 8);
	gp.props_ptr = U64(ids); gp.prop_values_ptr = U64(vals);
	ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &gp);
	for (uint32_t i = 0; i < gp.count_props; i++) {
		struct drm_mode_get_property p = { .prop_id = ids[i] };
		ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &p);
		printf("%s=%llu ", p.name, (unsigned long long)vals[i]);
	}
	printf("\n");
	return 0;
}
