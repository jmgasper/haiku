/*
 * Interfaces that the RM core of NVIDIA's proprietary Linux driver imports
 * in addition to those used by the open RM.
 */

#include <string.h>

#include "nv-include.h"


extern "C" {

// The proprietary RM drives pre-Turing GPUs without GSP firmware.
const NvBool nv_is_rm_firmware_supported_os = NV_FALSE;


NvBool NV_API_CALL
nv_is_rm_firmware_active(nv_state_t *nv)
{
	return NV_FALSE;
}


int
nvswitch_os_memcmp(const void *s1, const void *s2, unsigned long size)
{
	return memcmp(s1, s2, size);
}


char *
nvswitch_os_strncat(char *dest, const char *src, unsigned long length)
{
	return strncat(dest, src, length);
}

}
