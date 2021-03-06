/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* Copyright 2009-2013 Freescale Semiconductor, Inc. */

#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>

#include <cutils/ashmem.h>
#include <cutils/log.h>
#include <cutils/atomic.h>

#include <hardware/hardware.h>
#include <hardware/gralloc.h>

#include "gralloc_priv.h"
#include "gr.h"

#include <ion/ion.h>

/*****************************************************************************/

struct gralloc_context_t {
    alloc_device_t  device;
    /* our private data here */
};

static int gralloc_alloc_buffer(alloc_device_t* dev,
        size_t size, int usage, buffer_handle_t* pHandle);

/*****************************************************************************/

int fb_device_open(const hw_module_t* module, const char* name,
        hw_device_t** device);

static int gralloc_device_open(const hw_module_t* module, const char* name,
        hw_device_t** device);

extern int gralloc_lock(gralloc_module_t const* module,
        buffer_handle_t handle, int usage,
        int l, int t, int w, int h,
        void** vaddr);

extern int gralloc_unlock(gralloc_module_t const* module, 
        buffer_handle_t handle);

extern int gralloc_register_buffer(gralloc_module_t const* module,
        buffer_handle_t handle);

extern int gralloc_unregister_buffer(gralloc_module_t const* module,
        buffer_handle_t handle);

extern int gralloc_perform(struct gralloc_module_t const* module,
                           int operation, ... );

/*****************************************************************************/

static struct hw_module_methods_t gralloc_module_methods = {
        open: gralloc_device_open
};

struct private_module_t HAL_MODULE_INFO_SYM = {
    base: {
        common: {
            tag: HARDWARE_MODULE_TAG,
            version_major: 1,
            version_minor: 0,
            id: GRALLOC_HARDWARE_MODULE_ID,
            name: "Graphics Memory Allocator Module",
            author: "The Android Open Source Project",
            methods: &gralloc_module_methods,
            dso: 0,
            reserved: {0},
        },
        registerBuffer: gralloc_register_buffer,
        unregisterBuffer: gralloc_unregister_buffer,
        lock: gralloc_lock,
        unlock: gralloc_unlock,
        perform: gralloc_perform,
        lock_ycbcr: 0,
        reserved_proc: {0},
    },
    framebuffer: 0,
    flags: 0,
    numBuffers: 0,
    bufferMask: 0,
    lock: PTHREAD_MUTEX_INITIALIZER,
    currentBuffer: 0,
    ion_master: -1,
    master_phys: 0,
};

#define ION_GPU_POOL_ID 2

/*****************************************************************************/

static int gralloc_alloc_framebuffer_locked(alloc_device_t* dev,
        size_t size, int usage, buffer_handle_t* pHandle)
{
    private_module_t* m = reinterpret_cast<private_module_t*>(
            dev->common.module);

    // allocate the framebuffer
    if (m->framebuffer == NULL) {
        // initialize the framebuffer, the framebuffer is mapped once
        // and forever.
        int err = mapFrameBufferLocked(m);
        if (err < 0) {
            return err;
        }
    }

    const uint32_t bufferMask = m->bufferMask;
    const uint32_t numBuffers = m->numBuffers;
    const size_t bufferSize = m->finfo.line_length * ALIGN_PIXEL_128(m->info.yres);
    if (numBuffers == 1) {
        // If we have only one buffer, we never use page-flipping. Instead,
        // we return a regular buffer which will be memcpy'ed to the main
        // screen when post is called.
        int newUsage = (usage & ~GRALLOC_USAGE_HW_FB) | GRALLOC_USAGE_HW_2D;
        pthread_mutex_unlock(&m->lock);
        int ret = gralloc_alloc_buffer(dev, bufferSize, newUsage, pHandle);
        pthread_mutex_lock(&m->lock);
        return ret;
    }

    if (bufferMask >= ((1LU<<numBuffers)-1)) {
        // We ran out of buffers.
        return -ENOMEM;
    }

    // create a "fake" handles for it
    intptr_t vaddr = intptr_t(m->framebuffer->base);
    private_handle_t* hnd = new private_handle_t(dup(m->framebuffer->fd), size,
            private_handle_t::PRIV_FLAGS_USES_ION |
            private_handle_t::PRIV_FLAGS_FRAMEBUFFER);

    // find a free slot
    for (uint32_t i=0 ; i<numBuffers ; i++) {
        if ((bufferMask & (1LU<<i)) == 0) {
            m->bufferMask |= (1LU<<i);
            break;
        }
        vaddr += bufferSize;
    }
    
    hnd->base = vaddr;
    hnd->offset = vaddr - intptr_t(m->framebuffer->base);
    hnd->phys = intptr_t(m->framebuffer->phys) + hnd->offset;
    *pHandle = hnd;

    return 0;
}

static int gralloc_alloc_framebuffer(alloc_device_t* dev,
        size_t size, int usage, buffer_handle_t* pHandle)
{
    private_module_t* m = reinterpret_cast<private_module_t*>(
            dev->common.module);
    pthread_mutex_lock(&m->lock);
    int err = gralloc_alloc_framebuffer_locked(dev, size, usage, pHandle);
    pthread_mutex_unlock(&m->lock);
    return err;
}

static int init_ion_area_locked(private_module_t* m)
{
    int err = 0;
    int master_fd = ion_open();
    if (master_fd >= 0) {
        m->ion_master = master_fd;
    } else {
        err = -errno;
    }
    return err;
}

static int init_ion_area(private_module_t* m)
{
    pthread_mutex_lock(&m->lock);
    int err = m->ion_master;
    if (err == -1) {
        // first time, try to initialize ion
        err = init_ion_area_locked(m);
        if (err) {
            m->ion_master = err;
        }
    } else if (err < 0) {
        // ion couldn't be initialized, never use it
    } else {
        // ion OK
        err = 0;
    }
    pthread_mutex_unlock(&m->lock);
    return err;
}

static int gralloc_alloc_buffer(alloc_device_t* dev,
        size_t size, int usage, buffer_handle_t* pHandle)
{
    int err = 0;
    int flags = 0;

    int fd = -1;
    void* base = 0;
    int offset = 0;
    int lockState = 0;
    void *buffer_handle = NULL;

    size = roundUpToPageSize(size);

    if (usage & GRALLOC_USAGE_HW_TEXTURE) {
        // enable ion in that case, so our software GL can fallback to
        // the copybit module.
        flags |= private_handle_t::PRIV_FLAGS_USES_ION;
    }
    
    if (usage & GRALLOC_USAGE_HW_2D) {
        flags |= private_handle_t::PRIV_FLAGS_USES_ION;
    }

    /* If not ION, try ashmem */
    if ((flags & private_handle_t::PRIV_FLAGS_USES_ION) == 0) {
try_ashmem:
        fd = ashmem_create_region("gralloc-buffer", size);
        if (fd < 0) {
            ALOGE("couldn't create ashmem (%s)", strerror(-errno));
            err = -errno;
        }
    } else {
        private_module_t* m = reinterpret_cast<private_module_t*>(
                dev->common.module);

        err = init_ion_area(m);
        if (err == 0) {
            struct ion_handle *handle, *alloc_handle;
            unsigned char *base = 0;

            err = ion_alloc(m->ion_master, size, PAGE_SIZE, ION_GPU_POOL_ID, &handle);
            if(err < 0) {
                ALOGE("Cannot allocate ion size = %d err = %d", size, err);
                return err;
            }

            alloc_handle = handle;
            buffer_handle = (void*)handle;

            err = ion_share(m->ion_master, handle, &fd);
            if(err < 0) {
                ALOGE("Cannot share ion handle = %p err = %d", handle, err);
                return err;
            }

            m->master_phys = ion_phys(m->ion_master, handle);
            if(m->master_phys == 0) {
                ALOGE("Cannot get physical for ion handle = %p", handle);
                return -errno;
            }

            err = ion_free(m->ion_master, alloc_handle);
            if(err < 0) {
                ALOGE("Cannot free ion handle = %p err = %d", alloc_handle, err);
                return err;
            }
        } else {
            if ((usage & GRALLOC_USAGE_HW_2D) == 0) {
                // the caller didn't request ION, so we can try something else
                flags &= ~private_handle_t::PRIV_FLAGS_USES_ION;
                err = 0;
                goto try_ashmem;
            } else {
                ALOGE("couldn't open ion (%s)", strerror(-errno));
            }
        }
    }

    if (err == 0) {
        private_handle_t* hnd = new private_handle_t(fd, size, flags);
        hnd->offset = 0 ;
        hnd->base = int(base);
        hnd->lockState = lockState;
        hnd->handle = buffer_handle;
        if (flags & private_handle_t::PRIV_FLAGS_USES_ION) {
            private_module_t* m = reinterpret_cast<private_module_t*>(
                    dev->common.module);
            hnd->phys = m->master_phys;
        }
        *pHandle = hnd;
    }

    ALOGE_IF(err, "gralloc failed err=%s", strerror(-err));
    
    return err;
}

/*****************************************************************************/

static int gralloc_alloc(alloc_device_t* dev,
        int w, int h, int format, int usage,
        buffer_handle_t* pHandle, int* pStride)
{
    if (!pHandle || !pStride)
        return -EINVAL;

    size_t size, alignedw, alignedh;
    if (format == HAL_PIXEL_FORMAT_YCbCr_420_SP || format == HAL_PIXEL_FORMAT_YCbCr_422_I ||
        format == HAL_PIXEL_FORMAT_YCbCr_422_SP ||
        format == HAL_PIXEL_FORMAT_YV12         || format == HAL_PIXEL_FORMAT_YCbCr_420_P ||
        format == HAL_PIXEL_FORMAT_YCbCr_422_P)
    {
        int luma_size;
        int chroma_size;

        // FIXME: there is no way to return the alignedh
        // Aligning height and width to 64 forces 4096 alignment of chroma
        // buffer assuming that the luma starts with 4096 alignment or higher.
        // This is required for GPU rendering in ICS for iMX5.
        alignedw = ALIGN_PIXEL_64(w);
        alignedh = ALIGN_PIXEL_64(h);
        luma_size = ALIGN_PIXEL_4096(alignedw * alignedh);
        switch (format) {
            case HAL_PIXEL_FORMAT_YCbCr_422_SP:
            case HAL_PIXEL_FORMAT_YCbCr_422_I:
            case HAL_PIXEL_FORMAT_YCbCr_422_P:
                chroma_size = ALIGN_PIXEL_4096( (alignedw * alignedh) / 2) * 2;
                break;
            case HAL_PIXEL_FORMAT_YCbCr_420_SP: //NV21
            case HAL_PIXEL_FORMAT_YCbCr_420_P:
            case HAL_PIXEL_FORMAT_YV12:
                chroma_size = ALIGN_PIXEL_4096(alignedw/2 * alignedh/2) * 2;
                break;
            default:
                return -EINVAL;
        }
        size = luma_size + chroma_size;
    } else {
        alignedw = ALIGN_PIXEL(w);
        alignedh = ALIGN_PIXEL(h);
        int bpp = 0;
        switch (format) {
            case HAL_PIXEL_FORMAT_RGBA_8888:
            case HAL_PIXEL_FORMAT_RGBX_8888:
            case HAL_PIXEL_FORMAT_BGRA_8888:
                bpp = 4;
                break;
            case HAL_PIXEL_FORMAT_RGB_888:
                bpp = 3;
                break;
            case HAL_PIXEL_FORMAT_RGB_565:
            case HAL_PIXEL_FORMAT_RGBA_5551:
            case HAL_PIXEL_FORMAT_RGBA_4444:
                bpp = 2;
                break;
            default:
                return -EINVAL;
        }
        size = alignedw * alignedh * bpp;
    }

    int err;
    if (usage & GRALLOC_USAGE_HW_FB) {
        err = gralloc_alloc_framebuffer(dev, size, usage, pHandle);
    } else {
        err = gralloc_alloc_buffer(dev, size, usage, pHandle);
    }

    if (err < 0) {
        return err;
    }

    private_handle_t* hnd = (private_handle_t*)(*pHandle);
    hnd->usage = usage;
    hnd->format = format;
    hnd->width = alignedw;
    hnd->height = alignedh;

    *pStride = alignedw;
    return 0;
}

static int gralloc_free(alloc_device_t* dev,
        buffer_handle_t handle)
{
    if (private_handle_t::validate(handle) < 0)
        return -EINVAL;

    private_handle_t const* hnd = reinterpret_cast<private_handle_t const*>(handle);
    if (hnd->flags & private_handle_t::PRIV_FLAGS_FRAMEBUFFER) {
        // free this buffer
        private_module_t* m = reinterpret_cast<private_module_t*>(
                dev->common.module);
        const size_t bufferSize = m->finfo.line_length * ALIGN_PIXEL_128(m->info.yres);
        int index = (hnd->base - m->framebuffer->base) / bufferSize;
        m->bufferMask &= ~(1<<index); 
    } else {
        gralloc_module_t* module = reinterpret_cast<gralloc_module_t*>(
                dev->common.module);
        terminateBuffer(module, const_cast<private_handle_t*>(hnd));
        if (hnd->flags & private_handle_t::PRIV_FLAGS_USES_ION) {
            int fd = ion_open();
            ion_import(fd, hnd->fd, (struct ion_handle **)&hnd->handle);
            if(munmap((void *)hnd->base, hnd->size)) {
                ALOGE("Failed to unmap at %p : %s", (void*)hnd->base, strerror(errno));
            }
            ion_free(fd,(struct ion_handle *)hnd->handle);
            close(fd);
        }

    }

    close(hnd->fd);
    delete hnd;
    return 0;
}

/*****************************************************************************/

static int gralloc_close(struct hw_device_t *dev)
{
    gralloc_context_t* ctx = reinterpret_cast<gralloc_context_t*>(dev);
    if (ctx) {
        /* TODO: keep a list of all buffer_handle_t created, and free them
         * all here.
         */
        free(ctx);
    }
    return 0;
}

int gralloc_device_open(const hw_module_t* module, const char* name,
        hw_device_t** device)
{
    int status = -EINVAL;
    hw_module_t *hw = const_cast<hw_module_t *>(module);
    private_module_t* m = reinterpret_cast<private_module_t*>(hw);

    /* if gpu0 */
    if (!strcmp(name, GRALLOC_HARDWARE_GPU0)) {
        gralloc_context_t *dev;
        dev = (gralloc_context_t*)malloc(sizeof(*dev));

        /* initialize our state here */
        memset(dev, 0, sizeof(*dev));

        /* initialize the procs */
        dev->device.common.tag = HARDWARE_DEVICE_TAG;
        dev->device.common.version = 0;
        dev->device.common.module = const_cast<hw_module_t*>(module);
        dev->device.common.close = gralloc_close;

        dev->device.alloc   = gralloc_alloc;
        dev->device.free    = gralloc_free;

        *device = &dev->device.common;
        status = 0;
    } else {

        m->flags = 0;
        m->ion_master = -1;
        m->master_phys = 0;

        status = fb_device_open(module, name, device);
    }
    return status;
}
