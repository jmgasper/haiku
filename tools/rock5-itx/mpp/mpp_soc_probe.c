#include <stdio.h>
#include "mpp_platform.h"
#include "mpp_soc.h"
int main(void) {
    const char* name = mpp_get_soc_name();
    int type = mpp_get_soc_type();
    unsigned codecs = mpp_get_vcodec_type();
    printf("ROCK5_MPP_SOC name=%s type=%d codec_mask=%08x\n",
        name ? name : "(null)", type, codecs);
    return name && type == ROCKCHIP_SOC_RK3588 && codecs != 0 ? 0 : 1;
}
