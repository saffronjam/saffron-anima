//! The per-frame command/sync ring.
//!
//! A `MAX_FRAMES_IN_FLIGHT` ring of frame slots — one command pool + buffer +
//! image-available semaphore + in-flight fence per slot. It owns its handles and
//! frees them in [`FrameRing::destroy`] (called by the renderer before the device
//! is torn down, since these handles borrow the device and cannot Drop themselves
//! without it).

use ash::vk;

use crate::{Device, Result, checked};

/// Frames the GPU may have in flight before the CPU blocks — the double-buffer depth.
pub const MAX_FRAMES_IN_FLIGHT: usize = 2;

/// One frame slot's command recording + synchronization primitives.
struct FrameData {
    command_pool: vk::CommandPool,
    command_buffer: vk::CommandBuffer,
    image_available: vk::Semaphore,
    in_flight: vk::Fence,
}

/// The ring of [`FrameData`] slots plus the current index.
///
/// Vulkan command pools are not thread-safe and the handles borrow the device, so
/// this is not a `Drop` type: the owning [`crate::Renderer`] calls
/// [`FrameRing::destroy`] after `wait_idle`, before the device is destroyed.
pub struct FrameRing {
    frames: Vec<FrameData>,
    index: usize,
}

impl FrameRing {
    /// Allocates the per-frame command pools/buffers, image-available semaphores,
    /// and (signaled) in-flight fences.
    ///
    /// # Errors
    ///
    /// Returns [`crate::Error::Vk`] for any failing Vulkan call; on partial
    /// failure the already-created handles are freed before returning.
    pub fn new(device: &Device) -> Result<Self> {
        let raw = device.raw();
        let mut frames = Vec::with_capacity(MAX_FRAMES_IN_FLIGHT);
        for _ in 0..MAX_FRAMES_IN_FLIGHT {
            match Self::create_frame(device) {
                Ok(frame) => frames.push(frame),
                Err(err) => {
                    for frame in &frames {
                        // SAFETY: the ash seam. Each handle was created on this
                        // device and is destroyed exactly once on the error path.
                        unsafe { Self::free_frame(raw, frame) };
                    }
                    return Err(err);
                }
            }
        }
        Ok(Self { frames, index: 0 })
    }

    fn create_frame(device: &Device) -> Result<FrameData> {
        let raw = device.raw();
        let pool_info = vk::CommandPoolCreateInfo::default()
            .flags(vk::CommandPoolCreateFlags::RESET_COMMAND_BUFFER)
            .queue_family_index(device.graphics_queue_family);
        // SAFETY: the ash seam. The create-info is valid for the call; the pool is
        // owned and freed in `destroy` / the error path.
        let command_pool = checked(
            unsafe { raw.create_command_pool(&pool_info, None) },
            "create_command_pool",
        )?;

        let alloc_info = vk::CommandBufferAllocateInfo::default()
            .command_pool(command_pool)
            .level(vk::CommandBufferLevel::PRIMARY)
            .command_buffer_count(1);
        // SAFETY: the ash seam. Allocates one primary buffer from the pool above.
        let command_buffer = checked(
            unsafe { raw.allocate_command_buffers(&alloc_info) },
            "allocate_command_buffers",
        )?[0];

        // SAFETY: the ash seam. Default-info semaphore creation.
        let image_available = checked(
            unsafe { raw.create_semaphore(&vk::SemaphoreCreateInfo::default(), None) },
            "create_semaphore",
        )?;

        let fence_info = vk::FenceCreateInfo::default().flags(vk::FenceCreateFlags::SIGNALED);
        // SAFETY: the ash seam. Creates the fence signaled so the first frame's
        // wait returns immediately.
        let in_flight = checked(
            unsafe { raw.create_fence(&fence_info, None) },
            "create_fence",
        )?;

        Ok(FrameData {
            command_pool,
            command_buffer,
            image_available,
            in_flight,
        })
    }

    /// Destroys every slot's handles. Must be called after `wait_idle`, before the
    /// device is torn down (the handles borrow the device).
    pub fn destroy(&mut self, device: &Device) {
        let raw = device.raw();
        for frame in &self.frames {
            // SAFETY: the ash seam. `wait_idle` ran first, so no handle is in use;
            // each is destroyed exactly once.
            unsafe { Self::free_frame(raw, frame) };
        }
        self.frames.clear();
    }

    /// Frees one slot's handles. The pool free also frees its command buffer.
    unsafe fn free_frame(raw: &ash::Device, frame: &FrameData) {
        // SAFETY: the ash seam. The caller guarantees the device is idle and these
        // handles were created on it.
        unsafe {
            raw.destroy_fence(frame.in_flight, None);
            raw.destroy_semaphore(frame.image_available, None);
            raw.destroy_command_pool(frame.command_pool, None);
        }
    }

    /// The current frame slot's in-flight fence.
    pub fn in_flight(&self) -> vk::Fence {
        self.frames[self.index].in_flight
    }

    /// The current frame slot's image-available semaphore.
    pub fn image_available(&self) -> vk::Semaphore {
        self.frames[self.index].image_available
    }

    /// The image-available semaphore for an explicit slot `index`. The windowed present path
    /// acquires in `begin_frame` (the current slot) but presents in `end_frame` after the
    /// offscreen submit advanced the ring, so it reads the just-rendered slot's semaphore by
    /// index rather than the (already advanced) current slot.
    pub fn image_available_for(&self, index: usize) -> vk::Semaphore {
        self.frames[index].image_available
    }

    /// The current frame slot's command pool.
    pub fn command_pool(&self) -> vk::CommandPool {
        self.frames[self.index].command_pool
    }

    /// The current frame slot's command buffer.
    pub fn command_buffer(&self) -> vk::CommandBuffer {
        self.frames[self.index].command_buffer
    }

    /// The current frame slot's index (0..[`MAX_FRAMES_IN_FLIGHT`)), keying the
    /// per-frame instance / material SSBOs.
    pub fn index(&self) -> usize {
        self.index
    }

    /// Advances to the next slot in the ring.
    pub fn advance(&mut self) {
        self.index = (self.index + 1) % MAX_FRAMES_IN_FLIGHT;
    }
}
