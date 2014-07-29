// Copyright (c) 2014 The Intel Corporation. All rights reserved.  // Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/bind.h"
#include "base/debug/trace_event.h"
#include "base/logging.h"
#include "base/metrics/histogram.h"
#include "base/stl_util.h"
#include "base/strings/string_util.h"
#include "base/synchronization/waitable_event.h"
#include "base/threading/non_thread_safe.h"
#include "content/common/gpu/gpu_channel.h"
#include "content/common/gpu/media/vaapi_video_decode_accelerator.h"
#include "media/base/bind_to_current_loop.h"
#include "media/video/picture.h"
#include "ui/gl/gl_bindings.h"
#include "ui/gl/gl_surface_egl.h"
#include "ui/gl/scoped_binders.h"

static void ReportToUMA(
    content::VaapiH264Decoder::VAVDAH264DecoderFailure failure) {
  UMA_HISTOGRAM_ENUMERATION(
      "Media.VAVDAH264.DecoderFailure",
      failure,
      content::VaapiH264Decoder::VAVDA_H264_DECODER_FAILURES_MAX);
}

namespace content {

#define RETURN_AND_NOTIFY_ON_FAILURE(result, log, error_code, ret)  \
  do {                                                              \
    if (!(result)) {                                                \
      LOG(INFO) << log;                                              \
      NotifyError(error_code);                                      \
      return ret;                                                   \
    }                                                               \
  } while (0)

VaapiVideoDecodeAccelerator::InputBuffer::InputBuffer() : id(0), size(0) {
}

VaapiVideoDecodeAccelerator::InputBuffer::~InputBuffer() {
}

void VaapiVideoDecodeAccelerator::NotifyError(Error error) {
  if (message_loop_ != base::MessageLoop::current()) {
    DCHECK(decoder_thread_proxy_->BelongsToCurrentThread());
    message_loop_->PostTask(FROM_HERE, base::Bind(
        &VaapiVideoDecodeAccelerator::NotifyError, weak_this_, error));
    return;
  }

  // Post Cleanup() as a task so we don't recursively acquire lock_.
  message_loop_->PostTask(FROM_HERE, base::Bind(
      &VaapiVideoDecodeAccelerator::Cleanup, weak_this_));

  LOG(INFO) << "Notifying of error " << error;
  if (client_) {
    client_->NotifyError(error);
    client_ptr_factory_.reset();
  }
}

// TFPPicture allocates VAImage and binds them to textures passed
// in PictureBuffers from clients to them. TFPPictures are created as
// a consequence of receiving a set of PictureBuffers from clients and released
// at the end of decode (or when a new set of PictureBuffers is required).
//
// TFPPictures are used for output, contents of VASurfaces passed from decoder
// are put into the associated vaimage memory and upload to client.
class VaapiVideoDecodeAccelerator::TFPPicture : public base::NonThreadSafe {
 public:
  ~TFPPicture();

  static linked_ptr<TFPPicture> Create(
      const base::Callback<bool(void)>& make_context_current, //NOLINT
      wl_display* wl_display,
      VaapiWrapper* va_wrapper,
      int32 picture_buffer_id,
      uint32 texture_id,
      gfx::Size size);

  int32 picture_buffer_id() {
    return picture_buffer_id_;
  }

  gfx::Size size() {
    return size_;
  }

  // Upload vaimage data to texture. Needs to be called every frame.
  bool Upload(VASurfaceID id);

 private:
  TFPPicture(const base::Callback<bool(void)>& make_context_current, //NOLINT
             wl_display* wl_display,
             VaapiWrapper* va_wrapper,
             int32 picture_buffer_id,
             uint32 texture_id,
             gfx::Size size);

  bool Initialize();

  EGLImageKHR CreateEGLImage(EGLDisplay egl_display, VASurfaceID surface);
  bool DestroyEGLImage(EGLDisplay egl_display);

  base::Callback<bool(void)> make_context_current_; //NOLINT

  wl_display* wl_display_;
  VaapiWrapper* va_wrapper_;

  // Output id for the client.
  int32 picture_buffer_id_;
  uint32 texture_id_;

  gfx::Size size_;
  VAImage va_image_;
  EGLImageKHR egl_image_;
  VABufferInfo buffer_info_;
  unsigned int buffer_name_;
  VASurfaceID surface_id_;

  DISALLOW_COPY_AND_ASSIGN(TFPPicture);
};

VaapiVideoDecodeAccelerator::TFPPicture::TFPPicture(
    const base::Callback<bool(void)>& make_context_current, //NOLINT
    wl_display* wl_display,
    VaapiWrapper* va_wrapper,
    int32 picture_buffer_id,
    uint32 texture_id,
    gfx::Size size)
    : make_context_current_(make_context_current),
      wl_display_(wl_display),
      va_wrapper_(va_wrapper),
      picture_buffer_id_(picture_buffer_id),
      texture_id_(texture_id),
      size_(size) {
  DCHECK(!make_context_current_.is_null());
};

linked_ptr<VaapiVideoDecodeAccelerator::TFPPicture>
VaapiVideoDecodeAccelerator::TFPPicture::Create(
    const base::Callback<bool(void)>& make_context_current, //NOLINT
    wl_display* wl_display,
    VaapiWrapper* va_wrapper,
    int32 picture_buffer_id,
    uint32 texture_id,
    gfx::Size size) {
  linked_ptr<TFPPicture> tfp_picture(
      new TFPPicture(make_context_current, wl_display, va_wrapper,
                     picture_buffer_id, texture_id, size));

  if (!tfp_picture->Initialize())
    tfp_picture.reset();

  return tfp_picture;
}

bool VaapiVideoDecodeAccelerator::TFPPicture::Initialize() {
  LOG(INFO) << "--- " <<__FUNCTION__;
  DCHECK(CalledOnValidThread());
  if (!make_context_current_.Run())
    return false;
 /* 
  if (!va_wrapper_->CreateRGBImage(size_, &va_image_)) {
    LOG(INFO) << "Failed to create VAImage";
    return false;
  }
 */
  return true;
}

VaapiVideoDecodeAccelerator::TFPPicture::~TFPPicture() {
  DCHECK(CalledOnValidThread());
  
  if(egl_image_ != EGL_NO_IMAGE_KHR)
    DestroyEGLImage(gfx::GLSurfaceEGL::GetHardwareDisplay());

  if (va_wrapper_) {
    va_wrapper_->UnlockBuffer(surface_id_, &buffer_name_, &buffer_info_);
    va_wrapper_->DestroyImage(&va_image_);
  }
   
}

/*
bool VaapiVideoDecodeAccelerator::TFPPicture::Bind() {
  DCHECK(CalledOnValidThread());

  if (!make_context_current_.Run())
    return false;
}
*/
/*
bool VaapiVideoDecodeAccelerator::TFPPicture::Upload(VASurfaceID surface) {
  DCHECK(CalledOnValidThread());

  if (!make_context_current_.Run())
    return false;

  if (!va_wrapper_->PutSurfaceIntoImage(surface, &va_image_)) {
    DVLOG(1) << "Failed to put va surface to image";
    return false;
  }

  void* buffer = NULL;
  if (!va_wrapper_->MapImage(&va_image_, &buffer)) {
    DVLOG(1) << "Failed to map VAImage";
    return false;
  }

  gfx::ScopedTextureBinder texture_binder(GL_TEXTURE_2D, texture_id_);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

  unsigned int al = 4 * size_.width();
  if (al != va_image_.pitches[0]) {
    // Not aligned phenomenon occurs only in special size video in None-X11.
    // So re-check RGBA data alignment and realign filled video frame in need.
    unsigned char* bhandle = static_cast<unsigned char*>(buffer);
    for (int i = 0; i < size_.height(); i++) {
      memcpy(bhandle + (i * al), bhandle + (i * (va_image_.pitches[0])), al);
    }
  }
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, size_.width(), size_.height(),
               0, GL_RGBA, GL_UNSIGNED_BYTE, buffer);

  va_wrapper_->UnmapImage(&va_image_);

  return true;
}
*/

bool VaapiVideoDecodeAccelerator::TFPPicture::Upload(VASurfaceID surface) {
  DCHECK(CalledOnValidThread());
  LOG(INFO) << "--- " <<__FUNCTION__;

  if (!make_context_current_.Run())
    return false;

  surface_id_ = surface;

  if (!va_wrapper_->PutSurfaceIntoImage(surface, &va_image_)) {
    LOG(INFO) << "Failed to put va surface to vaimage";
    return false;
  }

  egl_image_ = 
      CreateEGLImage(gfx::GLSurfaceEGL::GetHardwareDisplay(), surface);
  if (egl_image_ == EGL_NO_IMAGE_KHR) {
    LOG(INFO) << "Failed to create EGL image";
    return false;
  }
  
  glBindTexture(GL_TEXTURE_EXTERNAL_OES, texture_id_);
  glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, egl_image_);

  return true;
}

EGLImageKHR VaapiVideoDecodeAccelerator::TFPPicture::CreateEGLImage(
    EGLDisplay egl_display, VASurfaceID surface) {
  DCHECK(CalledOnValidThread());
  //VAImage va_image;
  //VABufferInfo buffer_info;
  uint32_t drm_format;
  /*
  if (va_wrapper_->CreateVAImage(surface, &va_image)) {
    DVLOG(1) << "Failed to map VAImage";
    return NULL;
  }
  */
  if (!va_wrapper_->LockBuffer(surface, &buffer_name_, &buffer_info_)) {
    LOG(INFO) << "Failed to Get Buffer Info";
    return NULL;
  }
  
  if (!va_wrapper_->QueryDRMFormat(&va_image_, &drm_format)) {
    LOG(INFO) << "Failed to Query DRM format";
    return NULL;
  }

  GLint attribs[23], *attrib;
  attrib = attribs;
  *attrib++ = EGL_LINUX_DRM_FOURCC_EXT;
  *attrib++ = drm_format;
  *attrib++ = EGL_WIDTH;
  *attrib++ = va_image_.width;
  *attrib++ = EGL_HEIGHT;
  *attrib++ = va_image_.height;
  LOG(INFO) << "width:" << va_image_.width << ", height:" << va_image_.height
      << " handle:" << buffer_info_.handle << ", offset:" << va_image_.offsets[0]
      << " pitches:" << va_image_.pitches[0];

  LOG(INFO) << "  num_planes:" << va_image_.num_planes;
  for (unsigned int i = 0; i < va_image_.num_planes; ++i) {
    *attrib++ = EGL_DMA_BUF_PLANE0_FD_EXT + 3*i;
    *attrib++ = buffer_info_.handle;
    *attrib++ = EGL_DMA_BUF_PLANE0_OFFSET_EXT + 3*i;
    *attrib++ = va_image_.offsets[i];
    *attrib++ = EGL_DMA_BUF_PLANE0_PITCH_EXT + 3*i;
    *attrib++ = va_image_.pitches[i];
  }
  *attrib++ = EGL_NONE;
  for (int i=0; i< 23; ++i) {
    LOG(INFO) << "GL Attr " << i << " : " << attribs[i];
  }

  EGLImageKHR egl_image = eglCreateImageKHR(
      egl_display, EGL_NO_CONTEXT,
      EGL_LINUX_DMA_BUF_EXT, NULL, attribs);

  return egl_image;
}

bool VaapiVideoDecodeAccelerator::TFPPicture::DestroyEGLImage(
    EGLDisplay egl_display) {
  LOG(INFO) << "--- " <<__FUNCTION__;
  return eglDestroyImageKHR(egl_display, egl_image_);
}

VaapiVideoDecodeAccelerator::TFPPicture*
    VaapiVideoDecodeAccelerator::TFPPictureById(int32 picture_buffer_id) {
  TFPPictures::iterator it = tfp_pictures_.find(picture_buffer_id);
  if (it == tfp_pictures_.end()) {
    LOG(INFO) << "Picture id " << picture_buffer_id << " does not exist";
    return NULL;
  }

  return it->second.get();
}

VaapiVideoDecodeAccelerator::VaapiVideoDecodeAccelerator(
    const base::Callback<bool(void)>& make_context_current) //NOLINT
    : wl_display_(NULL),
      make_context_current_(make_context_current),
      state_(kUninitialized),
      input_ready_(&lock_),
      surfaces_available_(&lock_),
      message_loop_(base::MessageLoop::current()),
      decoder_thread_("VaapiDecoderThread"),
      num_frames_at_client_(0),
      num_stream_bufs_at_decoder_(0),
      finish_flush_pending_(false),
      awaiting_va_surfaces_recycle_(false),
      requested_num_pics_(0),
      weak_this_factory_(this) {
  weak_this_ = weak_this_factory_.GetWeakPtr();
  va_surface_release_cb_ = media::BindToCurrentLoop(base::Bind(
      &VaapiVideoDecodeAccelerator::RecycleVASurfaceID, weak_this_));
}

VaapiVideoDecodeAccelerator::~VaapiVideoDecodeAccelerator() {
  DCHECK_EQ(message_loop_, base::MessageLoop::current());
  if (wl_display_) {
    wl_display_flush(wl_display_);
    wl_display_disconnect(wl_display_);
    wl_display_ = NULL;
  }
}

typedef struct wayland_display {
  wl_display* display;
  wl_compositor* compositor;
  wl_shell* shell;
  wl_registry* registry;
  int event_fd;
} wayland_display;

static void registry_handle_global(
    void* data,
    wl_registry* registry,
    uint32_t id,
    const char* interface,
    uint32_t version) {
  wayland_display* display_handle =
      reinterpret_cast<wayland_display *>(data);

  if (strcmp(interface, "wl_compositor") == 0) {
    display_handle->compositor =
        reinterpret_cast<wl_compositor *>(wl_registry_bind(
            registry, id, &wl_compositor_interface, 1));
  } else if (strcmp(interface, "wl_shell") == 0) {
    display_handle->shell =
        reinterpret_cast<wl_shell *>(wl_registry_bind(
            registry, id, &wl_shell_interface, 1));
  }
}

static const struct wl_registry_listener registry_listener = {
  registry_handle_global,
  NULL,
};

bool VaapiVideoDecodeAccelerator::Initialize(media::VideoCodecProfile profile,
                                             Client* client) {
  LOG(INFO) << "--- " <<__FUNCTION__;
  DCHECK_EQ(message_loop_, base::MessageLoop::current());

  client_ptr_factory_.reset(new base::WeakPtrFactory<Client>(client));
  client_ = client_ptr_factory_->GetWeakPtr();

  base::AutoLock auto_lock(lock_);
  DCHECK_EQ(state_, kUninitialized);
  LOG(INFO) << "Initializing VAVDA, profile: " << profile;

  if (!make_context_current_.Run())
    return false;

  wayland_display d;

  d.display = wl_display_connect(NULL);
  if (!d.display)
    return false;
  wl_display_set_user_data(d.display, &d);
  d.registry = wl_display_get_registry(d.display);
  wl_registry_add_listener(d.registry, &registry_listener, &d);
  wl_display_dispatch(d.display);
  wl_display_ = d.display;

  vaapi_wrapper_ = VaapiWrapper::Create(
      profile, wl_display_,
      base::Bind(&ReportToUMA, content::VaapiH264Decoder::VAAPI_ERROR));

  if (!vaapi_wrapper_.get()) {
    LOG(INFO) << "Failed initializing VAAPI";
    return false;
  }

  decoder_.reset(
      new VaapiH264Decoder(
          vaapi_wrapper_.get(),
          media::BindToCurrentLoop(base::Bind(
              &VaapiVideoDecodeAccelerator::SurfaceReady, weak_this_)),
          base::Bind(&ReportToUMA)));

  CHECK(decoder_thread_.Start());
  decoder_thread_proxy_ = decoder_thread_.message_loop_proxy();

  state_ = kIdle;

  return true;
}

void VaapiVideoDecodeAccelerator::SurfaceReady(
    int32 input_id,
    const scoped_refptr<VASurface>& va_surface) {
  DCHECK_EQ(message_loop_, base::MessageLoop::current());
  DCHECK(!awaiting_va_surfaces_recycle_);

  // Drop any requests to output if we are resetting or being destroyed.
  if (state_ == kResetting || state_ == kDestroying)
    return;

  pending_output_cbs_.push(
      base::Bind(&VaapiVideoDecodeAccelerator::OutputPicture,
                 weak_this_, va_surface, input_id));

  TryOutputSurface();
}

void VaapiVideoDecodeAccelerator::OutputPicture(
    const scoped_refptr<VASurface>& va_surface,
    int32 input_id,
    TFPPicture* tfp_picture) {
  LOG(INFO) << "--- " <<__FUNCTION__;
  DCHECK_EQ(message_loop_, base::MessageLoop::current());

  int32 output_id  = tfp_picture->picture_buffer_id();

  TRACE_EVENT2("Video Decoder", "VAVDA::OutputSurface",
               "input_id", input_id,
               "output_id", output_id);

  LOG(INFO) << "Outputting VASurface " << va_surface->id()
           << " into pixmap bound to picture buffer id " << output_id;

  RETURN_AND_NOTIFY_ON_FAILURE(tfp_picture->Upload(va_surface->id()),
                               "Failed to upload VASurface to texture",
                               PLATFORM_FAILURE, ); //NOLINT

  // Notify the client a picture is ready to be displayed.
  ++num_frames_at_client_;
  TRACE_COUNTER1("Video Decoder", "Textures at client", num_frames_at_client_);
  LOG(INFO) << "Notifying output picture id " << output_id
           << " for input "<< input_id << " is ready";
  if (client_)
    client_->PictureReady(media::Picture(output_id, input_id));
}

void VaapiVideoDecodeAccelerator::TryOutputSurface() {
  DCHECK_EQ(message_loop_, base::MessageLoop::current());
  LOG(INFO) << "--- " <<__FUNCTION__;

  // Handle Destroy() arriving while pictures are queued for output.
  if (!client_)
    return;

  if (pending_output_cbs_.empty() || output_buffers_.empty())
    return;

  OutputCB output_cb = pending_output_cbs_.front();
  pending_output_cbs_.pop();

  TFPPicture* tfp_picture = TFPPictureById(output_buffers_.front());
  DCHECK(tfp_picture);
  output_buffers_.pop();

  output_cb.Run(tfp_picture);

  if (finish_flush_pending_ && pending_output_cbs_.empty())
    FinishFlush();
}

void VaapiVideoDecodeAccelerator::MapAndQueueNewInputBuffer(
    const media::BitstreamBuffer& bitstream_buffer) {
  LOG(INFO) << "--- " <<__FUNCTION__;
  DCHECK_EQ(message_loop_, base::MessageLoop::current());
  TRACE_EVENT1("Video Decoder", "MapAndQueueNewInputBuffer", "input_id",
      bitstream_buffer.id());

  LOG(INFO) << "Mapping new input buffer id: " << bitstream_buffer.id()
           << " size: " << static_cast<int>(bitstream_buffer.size());

  scoped_ptr<base::SharedMemory> shm(
      new base::SharedMemory(bitstream_buffer.handle(), true));
  RETURN_AND_NOTIFY_ON_FAILURE(shm->Map(bitstream_buffer.size()),
      "Failed to map input buffer", UNREADABLE_INPUT,); //NOLINT

  base::AutoLock auto_lock(lock_);

  // Set up a new input buffer and queue it for later.
  linked_ptr<InputBuffer> input_buffer(new InputBuffer());
  input_buffer->shm.reset(shm.release());
  input_buffer->id = bitstream_buffer.id();
  input_buffer->size = bitstream_buffer.size();

  ++num_stream_bufs_at_decoder_;
  TRACE_COUNTER1("Video Decoder", "Stream buffers at decoder",
                 num_stream_bufs_at_decoder_);

  input_buffers_.push(input_buffer);
  input_ready_.Signal();
}

bool VaapiVideoDecodeAccelerator::GetInputBuffer_Locked() {
  DCHECK(decoder_thread_proxy_->BelongsToCurrentThread());
  lock_.AssertAcquired();

  if (curr_input_buffer_.get())
    return true;

  // Will only wait if it is expected that in current state new buffers will
  // be queued from the client via Decode(). The state can change during wait.
  while (input_buffers_.empty() && (state_ == kDecoding || state_ == kIdle)) {
    input_ready_.Wait();
  }

  // We could have got woken up in a different state or never got to sleep
  // due to current state; check for that.
  switch (state_) {
    case kFlushing:
      // Here we are only interested in finishing up decoding buffers that are
      // already queued up. Otherwise will stop decoding.
      if (input_buffers_.empty())
        return false;
      // else fallthrough
    case kDecoding:
    case kIdle:
      DCHECK(!input_buffers_.empty());

      curr_input_buffer_ = input_buffers_.front();
      input_buffers_.pop();

      LOG(INFO) << "New current bitstream buffer, id: "
               << curr_input_buffer_->id
               << " size: " << curr_input_buffer_->size;

      decoder_->SetStream(
          static_cast<uint8*>(curr_input_buffer_->shm->memory()),
          curr_input_buffer_->size, curr_input_buffer_->id);
      return true;

    default:
      // We got woken up due to being destroyed/reset, ignore any already
      // queued inputs.
      return false;
  }
}

void VaapiVideoDecodeAccelerator::ReturnCurrInputBuffer_Locked() {
  lock_.AssertAcquired();
  DCHECK(decoder_thread_proxy_->BelongsToCurrentThread());
  DCHECK(curr_input_buffer_.get());

  int32 id = curr_input_buffer_->id;
  curr_input_buffer_.reset();
  LOG(INFO) << "End of input buffer " << id;
  message_loop_->PostTask(FROM_HERE, base::Bind(
      &Client::NotifyEndOfBitstreamBuffer, client_, id));

  --num_stream_bufs_at_decoder_;
  TRACE_COUNTER1("Video Decoder", "Stream buffers at decoder",
                 num_stream_bufs_at_decoder_);
}

bool VaapiVideoDecodeAccelerator::FeedDecoderWithOutputSurfaces_Locked() {
  lock_.AssertAcquired();
  DCHECK(decoder_thread_proxy_->BelongsToCurrentThread());

  while (available_va_surfaces_.empty() &&
        (state_ == kDecoding || state_ == kFlushing || state_ == kIdle)) {
    surfaces_available_.Wait();
  }

  if (state_ != kDecoding && state_ != kFlushing && state_ != kIdle)
    return false;

  while (!available_va_surfaces_.empty()) {
    scoped_refptr<VASurface> va_surface(
        new VASurface(available_va_surfaces_.front(), va_surface_release_cb_));
    available_va_surfaces_.pop_front();
    decoder_->ReuseSurface(va_surface);
  }

  return true;
}

void VaapiVideoDecodeAccelerator::DecodeTask() {
  DCHECK(decoder_thread_proxy_->BelongsToCurrentThread());
  TRACE_EVENT0("Video Decoder", "VAVDA::DecodeTask");
  base::AutoLock auto_lock(lock_);

  if (state_ != kDecoding)
    return;

  // Main decode task.
  LOG(INFO) << "Decode task";

  // Try to decode what stream data is (still) in the decoder until we run out
  // of it.
  while (GetInputBuffer_Locked()) {
    DCHECK(curr_input_buffer_.get());

    VaapiH264Decoder::DecResult res;
    {
      // We are OK releasing the lock here, as decoder never calls our methods
      // directly and we will reacquire the lock before looking at state again.
      // This is the main decode function of the decoder and while keeping
      // the lock for its duration would be fine, it would defeat the purpose
      // of having a separate decoder thread.
      base::AutoUnlock auto_unlock(lock_);
      res = decoder_->Decode();
    }

    switch (res) {
      case VaapiH264Decoder::kAllocateNewSurfaces:
        LOG(INFO) << "Decoder requesting a new set of surfaces";
        message_loop_->PostTask(FROM_HERE, base::Bind(
            &VaapiVideoDecodeAccelerator::InitiateSurfaceSetChange, weak_this_,
                decoder_->GetRequiredNumOfPictures(),
                decoder_->GetPicSize()));
        // We'll get rescheduled once ProvidePictureBuffers() finishes.
        return;

      case VaapiH264Decoder::kRanOutOfStreamData:
        ReturnCurrInputBuffer_Locked();
        break;

      case VaapiH264Decoder::kRanOutOfSurfaces:
        // No more output buffers in the decoder, try getting more or go to
        // sleep waiting for them.
        if (!FeedDecoderWithOutputSurfaces_Locked())
          return;

        break;

      case VaapiH264Decoder::kDecodeError:
        RETURN_AND_NOTIFY_ON_FAILURE(false, "Error decoding stream",
                                     PLATFORM_FAILURE,); //NOLINT
        return;
    }
  }
}

void VaapiVideoDecodeAccelerator::InitiateSurfaceSetChange(size_t num_pics,
                                                           gfx::Size size) {
  DCHECK_EQ(message_loop_, base::MessageLoop::current());
  DCHECK(!awaiting_va_surfaces_recycle_);

  // At this point decoder has stopped running and has already posted onto our
  // loop any remaining output request callbacks, which executed before we got
  // here. Some of them might have been pended though, because we might not
  // have had enough TFPictures to output surfaces to. Initiate a wait cycle,
  // which will wait for client to return enough PictureBuffers to us, so that
  // we can finish all pending output callbacks, releasing associated surfaces.
  LOG(INFO) << "Initiating surface set change";
  awaiting_va_surfaces_recycle_ = true;

  requested_num_pics_ = num_pics;
  requested_pic_size_ = size;

  TryFinishSurfaceSetChange();
}

void VaapiVideoDecodeAccelerator::TryFinishSurfaceSetChange() {
  DCHECK_EQ(message_loop_, base::MessageLoop::current());

  if (!awaiting_va_surfaces_recycle_)
    return;

  if (!pending_output_cbs_.empty() ||
      tfp_pictures_.size() != available_va_surfaces_.size()) {
    // Either:
    // 1. Not all pending pending output callbacks have been executed yet.
    // Wait for the client to return enough pictures and retry later.
    // 2. The above happened and all surface release callbacks have been posted
    // as the result, but not all have executed yet. Post ourselves after them
    // to let them release surfaces.
    LOG(INFO) << "Awaiting pending output/surface release callbacks to finish";
    message_loop_->PostTask(FROM_HERE, base::Bind(
        &VaapiVideoDecodeAccelerator::TryFinishSurfaceSetChange, weak_this_));
    return;
  }

  // All surfaces released, destroy them and dismiss all PictureBuffers.
  awaiting_va_surfaces_recycle_ = false;
  available_va_surfaces_.clear();
  vaapi_wrapper_->DestroySurfaces();

  for (TFPPictures::iterator iter = tfp_pictures_.begin();
       iter != tfp_pictures_.end(); ++iter) {
    LOG(INFO) << "Dismissing picture id: " << iter->first;
    if (client_)
      client_->DismissPictureBuffer(iter->first);
  }
  tfp_pictures_.clear();

  // And ask for a new set as requested.
  LOG(INFO) << "Requesting " << requested_num_pics_ << " pictures of size: "
           << requested_pic_size_.ToString();

  message_loop_->PostTask(FROM_HERE, base::Bind(
      &Client::ProvidePictureBuffers, client_,
      requested_num_pics_, requested_pic_size_, GL_TEXTURE_2D));
}

void VaapiVideoDecodeAccelerator::Decode(
    const media::BitstreamBuffer& bitstream_buffer) {
  DCHECK_EQ(message_loop_, base::MessageLoop::current());

  TRACE_EVENT1("Video Decoder", "VAVDA::Decode", "Buffer id",
               bitstream_buffer.id());

  // We got a new input buffer from the client, map it and queue for later use.
  MapAndQueueNewInputBuffer(bitstream_buffer);

  base::AutoLock auto_lock(lock_);
  switch (state_) {
    case kIdle:
      state_ = kDecoding;
      decoder_thread_proxy_->PostTask(FROM_HERE, base::Bind(
          &VaapiVideoDecodeAccelerator::DecodeTask,
          base::Unretained(this)));
      break;

    case kDecoding:
      // Decoder already running, fallthrough.
    case kResetting:
      // When resetting, allow accumulating bitstream buffers, so that
      // the client can queue after-seek-buffers while we are finishing with
      // the before-seek one.
      break;

    default:
      RETURN_AND_NOTIFY_ON_FAILURE(false,
          "Decode request from client in invalid state: " << state_,
          PLATFORM_FAILURE,); //NOLINT
      break;
  }
}

void VaapiVideoDecodeAccelerator::RecycleVASurfaceID(
    VASurfaceID va_surface_id) {
  DCHECK_EQ(message_loop_, base::MessageLoop::current());
  base::AutoLock auto_lock(lock_);

  available_va_surfaces_.push_back(va_surface_id);
  surfaces_available_.Signal();
}

void VaapiVideoDecodeAccelerator::AssignPictureBuffers(
    const std::vector<media::PictureBuffer>& buffers) {
  DCHECK_EQ(message_loop_, base::MessageLoop::current());

  base::AutoLock auto_lock(lock_);
  DCHECK(tfp_pictures_.empty());

  while (!output_buffers_.empty())
    output_buffers_.pop();

  RETURN_AND_NOTIFY_ON_FAILURE(
      buffers.size() == requested_num_pics_,
      "Got an invalid buffers. (Got " << buffers.size() << ", requested "
      << requested_num_pics_ << ")", INVALID_ARGUMENT,); //NOLINT
  DCHECK(requested_pic_size_ == buffers[0].size());

  std::vector<VASurfaceID> va_surface_ids;
  RETURN_AND_NOTIFY_ON_FAILURE(
      vaapi_wrapper_->CreateSurfaces(requested_pic_size_,
                                     buffers.size(),
                                     &va_surface_ids),
      "Failed creating VA Surfaces", PLATFORM_FAILURE,); //NOLINT
  DCHECK_EQ(va_surface_ids.size(), buffers.size());

  for (size_t i = 0; i < buffers.size(); ++i) {
    LOG(INFO) << "Assigning picture id: " << buffers[i].id()
             << " to texture id: " << buffers[i].texture_id()
             << " VASurfaceID: " << va_surface_ids[i];

    linked_ptr<TFPPicture> tfp_picture(
        TFPPicture::Create(make_context_current_, wl_display_,
                           vaapi_wrapper_.get(), buffers[i].id(),
                           buffers[i].texture_id(), requested_pic_size_));

    RETURN_AND_NOTIFY_ON_FAILURE(
        tfp_picture.get(), "Failed assigning picture buffer to a texture.",
        PLATFORM_FAILURE,); //NOLINT

    bool inserted = tfp_pictures_.insert(std::make_pair(
        buffers[i].id(), tfp_picture)).second;
    DCHECK(inserted);

    output_buffers_.push(buffers[i].id());
    available_va_surfaces_.push_back(va_surface_ids[i]);
    surfaces_available_.Signal();
  }

  state_ = kDecoding;
  decoder_thread_proxy_->PostTask(FROM_HERE, base::Bind(
      &VaapiVideoDecodeAccelerator::DecodeTask, base::Unretained(this)));
}

void VaapiVideoDecodeAccelerator::ReusePictureBuffer(int32 picture_buffer_id) {
  DCHECK_EQ(message_loop_, base::MessageLoop::current());
  TRACE_EVENT1("Video Decoder", "VAVDA::ReusePictureBuffer", "Picture id",
               picture_buffer_id);

  --num_frames_at_client_;
  TRACE_COUNTER1("Video Decoder", "Textures at client", num_frames_at_client_);

  output_buffers_.push(picture_buffer_id);
  TryOutputSurface();
}

void VaapiVideoDecodeAccelerator::FlushTask() {
  DCHECK(decoder_thread_proxy_->BelongsToCurrentThread());
  LOG(INFO) << "Flush task";

  // First flush all the pictures that haven't been outputted, notifying the
  // client to output them.
  bool res = decoder_->Flush();
  RETURN_AND_NOTIFY_ON_FAILURE(res, "Failed flushing the decoder.",
                               PLATFORM_FAILURE,); //NOLINT

  // Put the decoder in idle state, ready to resume.
  decoder_->Reset();

  message_loop_->PostTask(FROM_HERE, base::Bind(
      &VaapiVideoDecodeAccelerator::FinishFlush, weak_this_));
}

void VaapiVideoDecodeAccelerator::Flush() {
  DCHECK_EQ(message_loop_, base::MessageLoop::current());
  LOG(INFO) << "Got flush request";

  base::AutoLock auto_lock(lock_);
  state_ = kFlushing;
  // Queue a flush task after all existing decoding tasks to clean up.
  decoder_thread_proxy_->PostTask(FROM_HERE, base::Bind(
      &VaapiVideoDecodeAccelerator::FlushTask, base::Unretained(this)));

  input_ready_.Signal();
  surfaces_available_.Signal();
}

void VaapiVideoDecodeAccelerator::FinishFlush() {
  DCHECK_EQ(message_loop_, base::MessageLoop::current());

  finish_flush_pending_ = false;

  base::AutoLock auto_lock(lock_);
  if (state_ != kFlushing) {
    DCHECK_EQ(state_, kDestroying);
    return;  // We could've gotten destroyed already.
  }

  // Still waiting for textures from client to finish outputting all pending
  // frames. Try again later.
  if (!pending_output_cbs_.empty()) {
    finish_flush_pending_ = true;
    return;
  }

  state_ = kIdle;

  message_loop_->PostTask(FROM_HERE, base::Bind(
      &Client::NotifyFlushDone, client_));

  LOG(INFO) << "Flush finished";
}

void VaapiVideoDecodeAccelerator::ResetTask() {
  DCHECK(decoder_thread_proxy_->BelongsToCurrentThread());
  LOG(INFO) << "ResetTask";

  // All the decoding tasks from before the reset request from client are done
  // by now, as this task was scheduled after them and client is expected not
  // to call Decode() after Reset() and before NotifyResetDone.
  decoder_->Reset();

  base::AutoLock auto_lock(lock_);

  // Return current input buffer, if present.
  if (curr_input_buffer_.get())
    ReturnCurrInputBuffer_Locked();

  // And let client know that we are done with reset.
  message_loop_->PostTask(FROM_HERE, base::Bind(
      &VaapiVideoDecodeAccelerator::FinishReset, weak_this_));
}

void VaapiVideoDecodeAccelerator::Reset() {
  DCHECK_EQ(message_loop_, base::MessageLoop::current());
  LOG(INFO) << "Got reset request";

  // This will make any new decode tasks exit early.
  base::AutoLock auto_lock(lock_);
  state_ = kResetting;
  finish_flush_pending_ = false;

  // Drop all remaining input buffers, if present.
  while (!input_buffers_.empty()) {
    message_loop_->PostTask(FROM_HERE, base::Bind(
        &Client::NotifyEndOfBitstreamBuffer, client_,
        input_buffers_.front()->id));
    input_buffers_.pop();
  }

  decoder_thread_proxy_->PostTask(FROM_HERE, base::Bind(
      &VaapiVideoDecodeAccelerator::ResetTask, base::Unretained(this)));

  input_ready_.Signal();
  surfaces_available_.Signal();
}

void VaapiVideoDecodeAccelerator::FinishReset() {
  DCHECK_EQ(message_loop_, base::MessageLoop::current());
  LOG(INFO) << "FinishReset";
  base::AutoLock auto_lock(lock_);

  if (state_ != kResetting) {
    DCHECK(state_ == kDestroying || state_ == kUninitialized) << state_;
    return;  // We could've gotten destroyed already.
  }

  // Drop pending outputs.
  while (!pending_output_cbs_.empty())
    pending_output_cbs_.pop();

  if (awaiting_va_surfaces_recycle_) {
    // Decoder requested a new surface set while we were waiting for it to
    // finish the last DecodeTask, running at the time of Reset().
    // Let the surface set change finish first before resetting.
    message_loop_->PostTask(FROM_HERE, base::Bind(
        &VaapiVideoDecodeAccelerator::FinishReset, weak_this_));
    return;
  }

  num_stream_bufs_at_decoder_ = 0;
  state_ = kIdle;

  message_loop_->PostTask(FROM_HERE, base::Bind(
      &Client::NotifyResetDone, client_));

  // The client might have given us new buffers via Decode() while we were
  // resetting and might be waiting for our move, and not call Decode() anymore
  // until we return something. Post a DecodeTask() so that we won't
  // sleep forever waiting for Decode() in that case. Having two of them
  // in the pipe is harmless, the additional one will return as soon as it sees
  // that we are back in kDecoding state.
  if (!input_buffers_.empty()) {
    state_ = kDecoding;
    decoder_thread_proxy_->PostTask(FROM_HERE, base::Bind(
      &VaapiVideoDecodeAccelerator::DecodeTask,
      base::Unretained(this)));
  }

  LOG(INFO) << "Reset finished";
}

void VaapiVideoDecodeAccelerator::Cleanup() {
  DCHECK_EQ(message_loop_, base::MessageLoop::current());

  if (state_ == kUninitialized || state_ == kDestroying)
    return;

  LOG(INFO) << "Destroying VAVDA";
  base::AutoLock auto_lock(lock_);
  state_ = kDestroying;

  client_ptr_factory_.reset();
  weak_this_factory_.InvalidateWeakPtrs();

  {
    base::AutoUnlock auto_unlock(lock_);
    // Post a dummy task to the decoder_thread_ to ensure it is drained.
    base::WaitableEvent waiter(false, false);
    decoder_thread_proxy_->PostTask(FROM_HERE, base::Bind(
        &base::WaitableEvent::Signal, base::Unretained(&waiter)));
    input_ready_.Signal();
    surfaces_available_.Signal();
    waiter.Wait();
    decoder_thread_.Stop();
  }

  state_ = kUninitialized;
}

void VaapiVideoDecodeAccelerator::Destroy() {
  DCHECK_EQ(message_loop_, base::MessageLoop::current());
  Cleanup();
  delete this;
}

}  // namespace content
