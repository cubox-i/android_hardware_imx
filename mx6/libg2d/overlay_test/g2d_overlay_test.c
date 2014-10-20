/*
 *  Copyright (C) 2013 Freescale Semiconductor, Inc.
 *  All Rights Reserved.
 *
 *  The following programs are the sole property of Freescale Semiconductor Inc.,
 *  and contain its proprietary and confidential information.
 *
 */

/*
 * @file g2d_overly_test.c
 *
 * @brief G2D overlay test application
 *
 */

#ifdef __cplusplus
extern "C"{
#endif

/*=======================================================================
                                        INCLUDE FILES
=======================================================================*/
/* Standard Include Files */
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/mxcfb.h>
#include <sys/mman.h>
#include <math.h>
#include <string.h>
#include <malloc.h>
#include <g2d.h>

#include "fsl_logo_bgr_480x360.c"
#include "ginger_bgr_800x600.c"
#include "akiyo_I420_176x144.c"
#include "forest_NV12_640x480.c"
#include "wall-1024x768-565rgb.c"
#include "NV16_352x288.c"
#include "yuyv_352x288.c"
#include "uyvy_640x480.c"

#define TFAIL -1
#define TPASS 0

#define CACHEABLE 0

int fd_fb0 = 0;
unsigned short * fb0;
int g_fb0_size;
int g_fb0_phys;

static void draw_image_to_framebuffer(int img_width, int img_height, int img_size, int img_format, unsigned char *img_ptr,
		 struct fb_var_screeninfo *screen_info, int left, int top, int to_width, int to_height, int set_alpha, int rotation)
{
	int i;
	struct g2d_surface src,dst;
	struct g2d_buf *buf;
	void *g2dHandle;

	if ( ( (left+to_width) > (int)screen_info->xres ) || ( (top+to_height) > (int)screen_info->yres ) )  {
		printf("Bad display image dimensions!\n");
		return;
	}

#if CACHEABLE
	buf = g2d_alloc(img_size, 1);//alloc physical contiguous memory for source image data with cacheable attribute
#else
	buf = g2d_alloc(img_size, 0);//alloc physical contiguous memory for source image data
#endif
	if(!buf) {
		printf("Fail to allocate physical memory for image buffer!\n");
		return;
	}

	memcpy(buf->buf_vaddr, img_ptr, img_size);

#if CACHEABLE
        g2d_cache_op(buf, G2D_CACHE_FLUSH);
#endif

	if(g2d_open(&g2dHandle) == -1 || g2dHandle == NULL) {
		printf("Fail to open g2d device!\n");
		g2d_free(buf);
		return;
	}

/*
	NOTE: in this example, all the test image data meet with the alignment requirement.
	Thus, in your code, you need to pay attention on that.

	Pixel buffer address alignment requirement,
	RGB/BGR:  pixel data in planes [0] with 16bytes alignment,
	NV12/NV16:  Y in planes [0], UV in planes [1], with 64bytes alignment,
	I420:    Y in planes [0], U in planes [1], V in planes [2], with 64 bytes alignment,
	YV12:  Y in planes [0], V in planes [1], U in planes [2], with 64 bytes alignment,
	NV21/NV61:  Y in planes [0], VU in planes [1], with 64bytes alignment,
	YUYV/YVYU/UYVY/VYUY:  in planes[0], buffer address is with 16bytes alignment.
*/

	src.format = img_format;
	switch (src.format) {
	case G2D_RGB565:
	case G2D_RGBA8888:
	case G2D_RGBX8888:
	case G2D_BGRA8888:
	case G2D_BGRX8888:
	case G2D_BGR565:
	case G2D_YUYV:
	case G2D_UYVY:
		src.planes[0] = buf->buf_paddr;
		break;
	case G2D_NV12:
		src.planes[0] = buf->buf_paddr;
		src.planes[1] = buf->buf_paddr + img_width * img_height;
		break;
	case G2D_I420:
		src.planes[0] = buf->buf_paddr;
		src.planes[1] = buf->buf_paddr + img_width * img_height;
		src.planes[2] = src.planes[1]  + img_width * img_height / 4;
		break;
	case G2D_NV16:
		src.planes[0] = buf->buf_paddr;
		src.planes[1] = buf->buf_paddr + img_width * img_height;
                break;
	default:
		printf("Unsupport image format in the example code\n");
		return;
	}

	src.left = 0;
	src.top = 0;
	src.right = img_width;
	src.bottom = img_height;
	src.stride = img_width;
	src.width  = img_width;
	src.height = img_height;
	src.rot  = G2D_ROTATION_0;

	dst.planes[0] = g_fb0_phys;
	dst.left = left;
	dst.top = top;
	dst.right = left + to_width;
	dst.bottom = top + to_height;
	dst.stride = screen_info->xres;
	dst.width  = screen_info->xres;
	dst.height = screen_info->yres;
	dst.rot    = rotation;
	dst.format = screen_info->bits_per_pixel == 16 ? G2D_RGB565 : (screen_info->red.offset == 0 ? G2D_RGBA8888 : G2D_BGRA8888);

	if (set_alpha)
	{
		src.blendfunc = G2D_ONE;
		dst.blendfunc = G2D_ONE_MINUS_SRC_ALPHA;
	
		src.global_alpha = 0x80;
		dst.global_alpha = 0xff;
	
		g2d_enable(g2dHandle, G2D_BLEND);
		g2d_enable(g2dHandle, G2D_GLOBAL_ALPHA);
	}

	g2d_blit(g2dHandle, &src, &dst);
	g2d_finish(g2dHandle);

	if (set_alpha)
	{
		g2d_disable(g2dHandle, G2D_GLOBAL_ALPHA);
		g2d_disable(g2dHandle, G2D_BLEND);
	}

	g2d_close(g2dHandle);
	g2d_free(buf);
}

int
main(int argc, char **argv)
{
	int retval = TPASS;
	struct fb_var_screeninfo screen_info;
	struct fb_fix_screeninfo fb_info;
	struct timeval tv1,tv2;
	
	if ((fd_fb0 = open("/dev/fb0", O_RDWR, 0)) < 0) {
		if ((fd_fb0 = open("/dev/graphics/fb0", O_RDWR, 0)) < 0) {
			printf("Unable to open fb0\n");
			retval = TFAIL;
			goto err0;
		}
	}

	/* Get fix screen info. */
	retval = ioctl(fd_fb0, FBIOGET_FSCREENINFO, &fb_info);
	if(retval < 0) {
		goto err2;
	}
	g_fb0_phys = fb_info.smem_start;

	/* Get variable screen info. */
	retval = ioctl(fd_fb0, FBIOGET_VSCREENINFO, &screen_info);
	if (retval < 0) {
		goto err2;
	}

	g_fb0_phys += (screen_info.xres_virtual * screen_info.yoffset * screen_info.bits_per_pixel / 8);

	g_fb0_size = screen_info.xres_virtual * screen_info.yres_virtual * screen_info.bits_per_pixel / 8;

	/* Map the device to memory*/
	fb0 = (unsigned short *) mmap(0, g_fb0_size,PROT_READ | PROT_WRITE, MAP_SHARED, fd_fb0, 0);
	if ((int)fb0 <= 0) {
		printf("\nError: failed to map framebuffer device 0 to memory.\n");
		goto err2;
	}

	gettimeofday(&tv1, NULL);

	draw_image_to_framebuffer(1024, 768, 1024*768*2, G2D_RGB565, (unsigned char *)wall_1024x768_565rgb, &screen_info, 0, 0, 1024, 768, 1, G2D_ROTATION_0);

	draw_image_to_framebuffer(800, 600, 800*600*2, G2D_BGR565, (unsigned char *) ginger_bgr_800x600, &screen_info, 100, 40, 500, 300, 1, G2D_ROTATION_0);

	draw_image_to_framebuffer(480, 360, 480*360*2, G2D_BGR565, (unsigned char *) fsl_logo_bgr_480x360, &screen_info, 350, 260, 400, 300, 0, G2D_ROTATION_0);

	draw_image_to_framebuffer(800, 600, 800*600*2, G2D_BGR565, (unsigned char *) ginger_bgr_800x600, &screen_info, 650, 450, 300, 200, 1, G2D_ROTATION_90);

	draw_image_to_framebuffer(800, 600, 800*600*2, G2D_BGR565, (unsigned char *) ginger_bgr_800x600, &screen_info, 50, 400, 300, 200, 0, G2D_ROTATION_180);

	draw_image_to_framebuffer(176, 144, 176*144*3/2, G2D_I420, akiyo_I420_176x144, &screen_info, 550, 40, 150, 120, 0, G2D_ROTATION_0);

	draw_image_to_framebuffer(640, 480, 640*480*3/2, G2D_NV12, forest_NV12_640x480, &screen_info, 600, 140, 400, 300, 1, G2D_ROTATION_0);

	draw_image_to_framebuffer(352, 288, 352*288*2, G2D_NV16, NV16_352x288, &screen_info, 0, 620, 176, 144, 1, G2D_ROTATION_0);

	draw_image_to_framebuffer(352, 288, 352*288*2, G2D_YUYV, yuyv_352x288, &screen_info, 420, 620, 176, 144, 1, G2D_ROTATION_0);

	draw_image_to_framebuffer(640, 480, 640*480*2, G2D_UYVY, uyvy_640x480, &screen_info, 860, 0, 160, 120, 1, G2D_ROTATION_0);

	gettimeofday(&tv2, NULL);

	printf("Overlay rendering time %dus .\n",(int)((tv2.tv_sec-tv1.tv_sec)*1000000 + tv2.tv_usec-tv1.tv_usec));

err3:
        munmap(fb0, g_fb0_size);
err2:
        close(fd_fb0);
err0:
        return retval;
}
