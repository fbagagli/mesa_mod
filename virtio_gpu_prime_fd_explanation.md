# Understanding `drmPrimeFDToHandle` and `virtio-gpu` in Mesa

When working with Mesa's OpenGL libraries on top of a virtualized GPU environment (virgl), you may notice that Mesa calls the `drmPrimeFDToHandle` function to import file descriptors (FDs). In your specific context (Linux v5.10), importing a dma-buf from another device fails because `virtiogpu_gem_prime_import_sg_table` returns `-ENODEV`.

This document explores why this is the case, how it can be implemented, and why Mesa uses `WINSYS_HANDLE_TYPE_FD`.

---

## 1. Why is it not implemented?

In Linux 5.10, the `virtio-gpu` driver implements `.gem_prime_import_sg_table` as a simple stub:

```c
struct drm_gem_object *virtgpu_gem_prime_import_sg_table(
	struct drm_device *dev, struct dma_buf_attachment *attach,
	struct sg_table *table)
{
	return ERR_PTR(-ENODEV);
}
```

When you call `drmPrimeFDToHandle()` in user-space (Mesa), the DRM subsystem calls `DRM_IOCTL_PRIME_FD_TO_HANDLE`. The kernel's `drm_gem_prime_fd_to_handle` processes this ioctl by taking the dma-buf FD, converting it back to the underlying `dma_buf` struct, and calling `.gem_prime_import` on the importing driver (`virtio-gpu`).

If the dma-buf was exported by `virtio-gpu` itself, `virtgpu_gem_prime_import` recognizes it and increments the reference count. However, if the dma-buf was exported by **another device** (e.g., a discrete GPU or a video decoder), the kernel falls back to the generic `drm_gem_prime_import`, which eventually calls `.gem_prime_import_sg_table` to map the physical memory pages (the scatter-gather table, or `sg_table`) into the importing device.

Because it returns `-ENODEV`, `virtio-gpu` in 5.10 fundamentally **cannot import dma-bufs exported by other devices**.

**Architectural Reasons:**
Historically, `virtio-gpu` resources were strictly backed by host-side memory allocations (via the hypervisor). A guest-side dma-buf imported from a guest physical device (like a passed-through GPU) is just a list of guest physical pages. The host-side compositor (which actually does the rendering) cannot access those guest physical pages unless there is a mechanism to share them with the host. In 5.10, `virtio-gpu` lacked the "guest blob" infrastructure required to communicate these arbitrary guest memory pages to the host hypervisor.

---

## 2. How can it be implemented?

To successfully implement dma-buf imports from other devices into `virtio-gpu`, the driver needs the ability to take the guest physical memory pages of the imported buffer and share them with the host.

This was actually implemented in the upstream Linux kernel starting around v5.16 with the introduction of **guest blob resources** (`VIRTGPU_BLOB_MEM_GUEST`).

Here is how the implementation works conceptually and how it was added to the kernel:

1. **Use `drm_gem_shmem` helpers:**
   Instead of a hardcoded `-ENODEV`, `virtio-gpu` needs a real GEM object to represent the imported memory. Since `virtio-gpu` uses the DRM shmem helpers for guest memory, it can use `drm_gem_shmem_prime_import_sg_table`.

2. **Wrap the imported memory in a virtio-gpu object:**
   When the sg_table is imported, the driver must create a `virtio_gpu_object` that holds onto those pages.

3. **Map the memory to the host (Hypervisor):**
   The driver needs a way to tell the host about these pages. By using the `VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB` command with the `VIRTGPU_BLOB_MEM_GUEST` flag, the driver instructs the host to create a resource backed by the guest's memory pages.

4. **Attach backing to the host resource:**
   The driver sends the scatter-gather list (the actual guest physical addresses from the imported `sg_table`) to the host using `VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING`.

In modern Linux kernels, the implementation looks roughly like this:

```c
struct drm_gem_object *virtgpu_gem_prime_import_sg_table(
	struct drm_device *dev, struct dma_buf_attachment *attach,
	struct sg_table *table)
{
    // 1. Create a GEM object from the imported scatter-gather table
    struct drm_gem_object *obj = drm_gem_shmem_prime_import_sg_table(dev, attach, table);
    if (IS_ERR(obj)) return obj;

    // 2. Wrap it in a virtio_gpu_object
    struct virtio_gpu_object *bo = gem_to_virtio_gpu_obj(obj);

    // 3. To make it usable by the host, it must be created as a guest blob
    // (Actual kernel implementation requires VIRTGPU_BLOB_MEM_GUEST feature)
    virtio_gpu_cmd_resource_create_blob(vgdev, bo, ...);
    virtio_gpu_object_attach(vgdev, bo, bo->base.pages);

    return obj;
}
```

To support this in your environment, you would need to backport the guest blob memory feature (commits from `drivers/gpu/drm/virtio/`) from a newer kernel (v5.16+) or upgrade the kernel, and ensure the hypervisor (QEMU/crosvm) supports `VIRTGPU_BLOB_MEM_GUEST`.

---

## 3. Why is `whandle->type` set to `WINSYS_HANDLE_TYPE_FD` instead of `WINSYS_HANDLE_TYPE_SHARE`? Is this correct in the context of virtio?

Yes, this is absolutely correct and represents modern best practices for the Linux graphics stack.

Here is why Mesa explicitly handles `WINSYS_HANDLE_TYPE_FD` instead of `WINSYS_HANDLE_TYPE_SHARE`:

### What are these types?
* **`WINSYS_HANDLE_TYPE_SHARE` (GEM Flink Names):**
  This uses a global 32-bit integer name generated by the `DRM_IOCTL_GEM_FLINK` ioctl. Any process on the system that guesses or knows this 32-bit integer can open the buffer using `DRM_IOCTL_GEM_OPEN`.
* **`WINSYS_HANDLE_TYPE_FD` (DMA-BUFs):**
  This uses a Unix file descriptor (FD) generated by `DRM_IOCTL_PRIME_HANDLE_TO_FD`. The FD is local to the process. To share it with another process, it must be explicitly passed over a Unix domain socket (e.g., via Wayland protocol messages).

### Why `WINSYS_HANDLE_TYPE_FD` is correct:
1. **Security:** GEM flink names (`WINSYS_HANDLE_TYPE_SHARE`) are fundamentally insecure. Because they are global, any malicious process can poll for new names and hijack graphical buffers belonging to other processes, potentially reading screen contents. DMA-BUFs (`TYPE_FD`) are secure because they follow Unix file descriptor access rules; a process cannot access the buffer unless the compositor explicitly grants it access by sending the FD.
2. **Cross-Device Sharing:** GEM flink names are local to a specific DRM device node (e.g., `/dev/dri/card0`). They cannot be used to share memory between different devices (e.g., an Intel iGPU and a virtio-gpu device). DMA-BUFs (`TYPE_FD`) are designed specifically for the Prime infrastructure to allow zero-copy memory sharing across completely different hardware devices.
3. **Synchronization:** DMA-BUFs carry implicit synchronization mechanisms (dma_resv fences). When you pass an FD, the kernel tracks whether the GPU is currently reading or writing to it, preventing tearing and rendering glitches. Flink names do not easily support this cross-process synchronization.

In the context of `virtio-gpu` (and Wayland, which mandates it), modern graphics architectures completely deprecate `WINSYS_HANDLE_TYPE_SHARE`. Using `WINSYS_HANDLE_TYPE_FD` is required for secure, efficient, and standardized sharing of buffers between the Wayland compositor, guest applications, and hardware devices.