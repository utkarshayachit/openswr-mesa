/*
 * Copyright © 2014-2015 Broadcom
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <errno.h>
#include <err.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "util/u_memory.h"
#include "util/ralloc.h"

#include "vc4_context.h"
#include "vc4_screen.h"

static bool dump_stats = false;

static void
vc4_bo_dump_stats(struct vc4_screen *screen)
{
        struct vc4_bo_cache *cache = &screen->bo_cache;

        fprintf(stderr, "  BOs allocated:   %d\n", screen->bo_count);
        fprintf(stderr, "  BOs size:        %dkb\n", screen->bo_size / 102);
        fprintf(stderr, "  BOs cached:      %d\n", cache->bo_count);
        fprintf(stderr, "  BOs cached size: %dkb\n", cache->bo_size / 102);

        if (!list_empty(&cache->time_list)) {
                struct vc4_bo *first = LIST_ENTRY(struct vc4_bo,
                                                  cache->time_list.next,
                                                  time_list);
                struct vc4_bo *last = LIST_ENTRY(struct vc4_bo,
                                                  cache->time_list.prev,
                                                  time_list);

                fprintf(stderr, "  oldest cache time: %ld\n",
                        (long)first->free_time);
                fprintf(stderr, "  newest cache time: %ld\n",
                        (long)last->free_time);

                struct timespec time;
                clock_gettime(CLOCK_MONOTONIC, &time);
                fprintf(stderr, "  now:               %ld\n",
                        time.tv_sec);
        }
}

static void
vc4_bo_remove_from_cache(struct vc4_bo_cache *cache, struct vc4_bo *bo)
{
        list_del(&bo->time_list);
        list_del(&bo->size_list);
        cache->bo_count--;
        cache->bo_size -= bo->size;
}

static struct vc4_bo *
vc4_bo_from_cache(struct vc4_screen *screen, uint32_t size, const char *name)
{
        struct vc4_bo_cache *cache = &screen->bo_cache;
        uint32_t page_index = size / 4096 - 1;

        if (cache->size_list_size <= page_index)
                return NULL;

        struct vc4_bo *bo = NULL;
        pipe_mutex_lock(cache->lock);
        if (!list_empty(&cache->size_list[page_index])) {
                bo = LIST_ENTRY(struct vc4_bo, cache->size_list[page_index].next,
                                size_list);

                /* Check that the BO has gone idle.  If not, then we want to
                 * allocate something new instead, since we assume that the
                 * user will proceed to CPU map it and fill it with stuff.
                 */
                if (!vc4_bo_wait(bo, 0, NULL)) {
                        pipe_mutex_unlock(cache->lock);
                        return NULL;
                }

                pipe_reference_init(&bo->reference, 1);
                vc4_bo_remove_from_cache(cache, bo);

                bo->name = name;
        }
        pipe_mutex_unlock(cache->lock);
        return bo;
}

struct vc4_bo *
vc4_bo_alloc(struct vc4_screen *screen, uint32_t size, const char *name)
{
        struct vc4_bo *bo;
        int ret;

        size = align(size, 4096);

        bo = vc4_bo_from_cache(screen, size, name);
        if (bo) {
                if (dump_stats) {
                        fprintf(stderr, "Allocated %s %dkb from cache:\n",
                                name, size / 1024);
                        vc4_bo_dump_stats(screen);
                }
                return bo;
        }

        bo = CALLOC_STRUCT(vc4_bo);
        if (!bo)
                return NULL;

        pipe_reference_init(&bo->reference, 1);
        bo->screen = screen;
        bo->size = size;
        bo->name = name;
        bo->private = true;

        if (!using_vc4_simulator) {
                struct drm_vc4_create_bo create;
                memset(&create, 0, sizeof(create));

                create.size = size;

                ret = drmIoctl(screen->fd, DRM_IOCTL_VC4_CREATE_BO, &create);
                bo->handle = create.handle;
        } else {
                struct drm_mode_create_dumb create;
                memset(&create, 0, sizeof(create));

                create.width = 128;
                create.bpp = 8;
                create.height = (size + 127) / 128;

                ret = drmIoctl(screen->fd, DRM_IOCTL_MODE_CREATE_DUMB, &create);
                bo->handle = create.handle;
                assert(create.size >= size);
        }
        if (ret != 0) {
                fprintf(stderr, "create ioctl failure\n");
                abort();
        }

        screen->bo_count++;
        screen->bo_size += bo->size;
        if (dump_stats) {
                fprintf(stderr, "Allocated %s %dkb:\n", name, size / 1024);
                vc4_bo_dump_stats(screen);
        }

        return bo;
}

void
vc4_bo_last_unreference(struct vc4_bo *bo)
{
        struct vc4_screen *screen = bo->screen;

        struct timespec time;
        clock_gettime(CLOCK_MONOTONIC, &time);
        pipe_mutex_lock(screen->bo_cache.lock);
        vc4_bo_last_unreference_locked_timed(bo, time.tv_sec);
        pipe_mutex_unlock(screen->bo_cache.lock);
}

static void
vc4_bo_free(struct vc4_bo *bo)
{
        struct vc4_screen *screen = bo->screen;

        if (bo->map) {
#ifdef USE_VC4_SIMULATOR
                if (bo->simulator_winsys_map) {
                        free(bo->map);
                        bo->map = bo->simulator_winsys_map;
                }
#endif
                munmap(bo->map, bo->size);
        }

        struct drm_gem_close c;
        memset(&c, 0, sizeof(c));
        c.handle = bo->handle;
        int ret = drmIoctl(screen->fd, DRM_IOCTL_GEM_CLOSE, &c);
        if (ret != 0)
                fprintf(stderr, "close object %d: %s\n", bo->handle, strerror(errno));

        screen->bo_count--;
        screen->bo_size -= bo->size;

        if (dump_stats) {
                fprintf(stderr, "Freed %s%s%dkb:\n",
                        bo->name ? bo->name : "",
                        bo->name ? " " : "",
                        bo->size / 1024);
                vc4_bo_dump_stats(screen);
        }

        free(bo);
}

static void
free_stale_bos(struct vc4_screen *screen, time_t time)
{
        struct vc4_bo_cache *cache = &screen->bo_cache;
        bool freed_any = false;

        list_for_each_entry_safe(struct vc4_bo, bo, &cache->time_list,
                                 time_list) {
                if (dump_stats && !freed_any) {
                        fprintf(stderr, "Freeing stale BOs:\n");
                        vc4_bo_dump_stats(screen);
                        freed_any = true;
                }

                /* If it's more than a second old, free it. */
                if (time - bo->free_time > 2) {
                        vc4_bo_remove_from_cache(cache, bo);
                        vc4_bo_free(bo);
                } else {
                        break;
                }
        }

        if (dump_stats && freed_any) {
                fprintf(stderr, "Freed stale BOs:\n");
                vc4_bo_dump_stats(screen);
        }
}

void
vc4_bo_last_unreference_locked_timed(struct vc4_bo *bo, time_t time)
{
        struct vc4_screen *screen = bo->screen;
        struct vc4_bo_cache *cache = &screen->bo_cache;
        uint32_t page_index = bo->size / 4096 - 1;

        if (!bo->private) {
                vc4_bo_free(bo);
                return;
        }

        if (cache->size_list_size <= page_index) {
                struct list_head *new_list =
                        ralloc_array(screen, struct list_head, page_index + 1);

                /* Move old list contents over (since the array has moved, and
                 * therefore the pointers to the list heads have to change).
                 */
                for (int i = 0; i < cache->size_list_size; i++) {
                        struct list_head *old_head = &cache->size_list[i];
                        if (list_empty(old_head))
                                list_inithead(&new_list[i]);
                        else {
                                new_list[i].next = old_head->next;
                                new_list[i].prev = old_head->prev;
                                new_list[i].next->prev = &new_list[i];
                                new_list[i].prev->next = &new_list[i];
                        }
                }
                for (int i = cache->size_list_size; i < page_index + 1; i++)
                        list_inithead(&new_list[i]);

                cache->size_list = new_list;
                cache->size_list_size = page_index + 1;
        }

        bo->free_time = time;
        list_addtail(&bo->size_list, &cache->size_list[page_index]);
        list_addtail(&bo->time_list, &cache->time_list);
        cache->bo_count++;
        cache->bo_size += bo->size;
        if (dump_stats) {
                fprintf(stderr, "Freed %s %dkb to cache:\n",
                        bo->name, bo->size / 1024);
                vc4_bo_dump_stats(screen);
        }
        bo->name = NULL;

        free_stale_bos(screen, time);
}

static struct vc4_bo *
vc4_bo_open_handle(struct vc4_screen *screen,
                   uint32_t winsys_stride,
                   uint32_t handle, uint32_t size)
{
        struct vc4_bo *bo = CALLOC_STRUCT(vc4_bo);

        assert(size);

        pipe_reference_init(&bo->reference, 1);
        bo->screen = screen;
        bo->handle = handle;
        bo->size = size;
        bo->name = "winsys";
        bo->private = false;

#ifdef USE_VC4_SIMULATOR
        vc4_bo_map(bo);
        bo->simulator_winsys_map = bo->map;
        bo->simulator_winsys_stride = winsys_stride;
        bo->map = malloc(bo->size);
#endif

        return bo;
}

struct vc4_bo *
vc4_bo_open_name(struct vc4_screen *screen, uint32_t name,
                 uint32_t winsys_stride)
{
        struct drm_gem_open o = {
                .name = name
        };
        int ret = drmIoctl(screen->fd, DRM_IOCTL_GEM_OPEN, &o);
        if (ret) {
                fprintf(stderr, "Failed to open bo %d: %s\n",
                        name, strerror(errno));
                return NULL;
        }

        return vc4_bo_open_handle(screen, winsys_stride, o.handle, o.size);
}

struct vc4_bo *
vc4_bo_open_dmabuf(struct vc4_screen *screen, int fd, uint32_t winsys_stride)
{
        uint32_t handle;
        int ret = drmPrimeFDToHandle(screen->fd, fd, &handle);
        int size;
        if (ret) {
                fprintf(stderr, "Failed to get vc4 handle for dmabuf %d\n", fd);
                return NULL;
        }

        /* Determine the size of the bo we were handed. */
        size = lseek(fd, 0, SEEK_END);
        if (size == -1) {
                fprintf(stderr, "Couldn't get size of dmabuf fd %d.\n", fd);
                return NULL;
        }

        return vc4_bo_open_handle(screen, winsys_stride, handle, size);
}

int
vc4_bo_get_dmabuf(struct vc4_bo *bo)
{
        int fd;
        int ret = drmPrimeHandleToFD(bo->screen->fd, bo->handle,
                                     O_CLOEXEC, &fd);
        if (ret != 0) {
                fprintf(stderr, "Failed to export gem bo %d to dmabuf\n",
                        bo->handle);
                return -1;
        }
        bo->private = false;

        return fd;
}

struct vc4_bo *
vc4_bo_alloc_shader(struct vc4_screen *screen, const void *data, uint32_t size)
{
        struct vc4_bo *bo;
        int ret;

        bo = CALLOC_STRUCT(vc4_bo);
        if (!bo)
                return NULL;

        pipe_reference_init(&bo->reference, 1);
        bo->screen = screen;
        bo->size = align(size, 4096);
        bo->name = "code";
        bo->private = false; /* Make sure it doesn't go back to the cache. */

        if (!using_vc4_simulator) {
                struct drm_vc4_create_shader_bo create = {
                        .size = size,
                        .data = (uintptr_t)data,
                };

                ret = drmIoctl(screen->fd, DRM_IOCTL_VC4_CREATE_SHADER_BO,
                               &create);
                bo->handle = create.handle;
        } else {
                struct drm_mode_create_dumb create;
                memset(&create, 0, sizeof(create));

                create.width = 128;
                create.bpp = 8;
                create.height = (size + 127) / 128;

                ret = drmIoctl(screen->fd, DRM_IOCTL_MODE_CREATE_DUMB, &create);
                bo->handle = create.handle;
                assert(create.size >= size);

                vc4_bo_map(bo);
                memcpy(bo->map, data, size);
        }
        if (ret != 0) {
                fprintf(stderr, "create shader ioctl failure\n");
                abort();
        }

        screen->bo_count++;
        screen->bo_size += bo->size;
        if (dump_stats) {
                fprintf(stderr, "Allocated shader %dkb:\n", size / 1024);
                vc4_bo_dump_stats(screen);
        }

        return bo;
}

bool
vc4_bo_flink(struct vc4_bo *bo, uint32_t *name)
{
        struct drm_gem_flink flink = {
                .handle = bo->handle,
        };
        int ret = drmIoctl(bo->screen->fd, DRM_IOCTL_GEM_FLINK, &flink);
        if (ret) {
                fprintf(stderr, "Failed to flink bo %d: %s\n",
                        bo->handle, strerror(errno));
                free(bo);
                return false;
        }

        bo->private = false;
        *name = flink.name;

        return true;
}

static int vc4_wait_seqno_ioctl(int fd, uint64_t seqno, uint64_t timeout_ns)
{
        if (using_vc4_simulator)
                return 0;

        struct drm_vc4_wait_seqno wait = {
                .seqno = seqno,
                .timeout_ns = timeout_ns,
        };
        int ret = drmIoctl(fd, DRM_IOCTL_VC4_WAIT_SEQNO, &wait);
        if (ret == -1)
                return -errno;
        else
                return 0;

}

bool
vc4_wait_seqno(struct vc4_screen *screen, uint64_t seqno, uint64_t timeout_ns,
               const char *reason)
{
        if (screen->finished_seqno >= seqno)
                return true;

        if (unlikely(vc4_debug & VC4_DEBUG_PERF) && timeout_ns && reason) {
                if (vc4_wait_seqno_ioctl(screen->fd, seqno, 0) == -ETIME) {
                        fprintf(stderr, "Blocking on seqno %lld for %s\n",
                                (long long)seqno, reason);
                }
        }

        int ret = vc4_wait_seqno_ioctl(screen->fd, seqno, timeout_ns);
        if (ret) {
                if (ret != -ETIME) {
                        fprintf(stderr, "wait failed: %d\n", ret);
                        abort();
                }

                return false;
        }

        screen->finished_seqno = seqno;
        return true;
}

static int vc4_wait_bo_ioctl(int fd, uint32_t handle, uint64_t timeout_ns)
{
        if (using_vc4_simulator)
                return 0;

        struct drm_vc4_wait_bo wait = {
                .handle = handle,
                .timeout_ns = timeout_ns,
        };
        int ret = drmIoctl(fd, DRM_IOCTL_VC4_WAIT_BO, &wait);
        if (ret == -1)
                return -errno;
        else
                return 0;

}

bool
vc4_bo_wait(struct vc4_bo *bo, uint64_t timeout_ns, const char *reason)
{
        struct vc4_screen *screen = bo->screen;

        if (unlikely(vc4_debug & VC4_DEBUG_PERF) && timeout_ns && reason) {
                if (vc4_wait_bo_ioctl(screen->fd, bo->handle, 0) == -ETIME) {
                        fprintf(stderr, "Blocking on %s BO for %s\n",
                                bo->name, reason);
                }
        }

        int ret = vc4_wait_bo_ioctl(screen->fd, bo->handle, timeout_ns);
        if (ret) {
                if (ret != -ETIME) {
                        fprintf(stderr, "wait failed: %d\n", ret);
                        abort();
                }

                return false;
        }

        return true;
}

void *
vc4_bo_map_unsynchronized(struct vc4_bo *bo)
{
        uint64_t offset;
        int ret;

        if (bo->map)
                return bo->map;

        if (!using_vc4_simulator) {
                struct drm_vc4_mmap_bo map;
                memset(&map, 0, sizeof(map));
                map.handle = bo->handle;
                ret = drmIoctl(bo->screen->fd, DRM_IOCTL_VC4_MMAP_BO, &map);
                offset = map.offset;
        } else {
                struct drm_mode_map_dumb map;
                memset(&map, 0, sizeof(map));
                map.handle = bo->handle;
                ret = drmIoctl(bo->screen->fd, DRM_IOCTL_MODE_MAP_DUMB, &map);
                offset = map.offset;
        }
        if (ret != 0) {
                fprintf(stderr, "map ioctl failure\n");
                abort();
        }

        bo->map = mmap(NULL, bo->size, PROT_READ | PROT_WRITE, MAP_SHARED,
                       bo->screen->fd, offset);
        if (bo->map == MAP_FAILED) {
                fprintf(stderr, "mmap of bo %d (offset 0x%016llx, size %d) failed\n",
                        bo->handle, (long long)offset, bo->size);
                abort();
        }

        return bo->map;
}

void *
vc4_bo_map(struct vc4_bo *bo)
{
        void *map = vc4_bo_map_unsynchronized(bo);

        bool ok = vc4_bo_wait(bo, PIPE_TIMEOUT_INFINITE, "bo map");
        if (!ok) {
                fprintf(stderr, "BO wait for map failed\n");
                abort();
        }

        return map;
}

void
vc4_bufmgr_destroy(struct pipe_screen *pscreen)
{
        struct vc4_screen *screen = vc4_screen(pscreen);
        struct vc4_bo_cache *cache = &screen->bo_cache;

        list_for_each_entry_safe(struct vc4_bo, bo, &cache->time_list,
                                 time_list) {
                vc4_bo_remove_from_cache(cache, bo);
                vc4_bo_free(bo);
        }

        if (dump_stats) {
                fprintf(stderr, "BO stats after screen destroy:\n");
                vc4_bo_dump_stats(screen);
        }
}
