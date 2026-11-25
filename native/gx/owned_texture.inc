// SPDX-License-Identifier: GPL-3.0-or-later
// Included inside pinned Aurora's gxcore namespace by adapt_window.cmake.
// The surrounding copy path is RecompCore/Aurora GPL code; dependency files
// remain immutable. All live recording transitions stay on the FIFO thread.
void set_owned_texture_lookup(OwnedTextureLookup lookup) {g_ownedTextureLookup=lookup;}

OwnedTexture capture_owned_efb_copy(const gxc::EfbCopyCommand& cmd,bool submit) {
  struct Capturing {
    Capturing() {g_capturingOwnedTexture=true;}
    ~Capturing() {g_capturingOwnedTexture=false;}
  } capturing;
  copy_efb_to_texture(cmd);
  const auto it=g_efbCopyTextures.find(cmd.dest_address);
  if(it==g_efbCopyTextures.end() || !it->second.handle)
    throw std::runtime_error("GPU texture producer allocation failed");
  // Legacy worker publication requires a submitted immutable resource. The
  // single-owner renderer can safely defer submission until a real observer.
  const auto handle=it->second.handle;
  if(submit && !gfx::segment_frame())throw std::runtime_error("GPU texture producer submission failed");
  return {handle,cmd.destination_width,cmd.destination_height,cmd.format};
}

std::vector<uint8_t> materialize_owned_texture(const OwnedTexture& copy) {
  if(!copy.texture)return {};
  const auto rgba=gfx::read_texture_rgba8(copy.texture);
  return gcnllr::encode_native_copy(copy.format,copy.width,copy.height,rgba,
      copy.texture->size.width,copy.texture->size.height);
}
void publish_owned_copy_memory(const OwnedTexture& copy,uint32_t address,const std::vector<uint8_t>& bytes) {
  const auto it=g_efbCopyTextures.find(address);
  /* A delayed predecessor must not bless a replacement GPU image with the
   * predecessor's RAM hash. Exact immutable handle identity is required. */
  if(it!=g_efbCopyTextures.end() && it->second.handle==copy.texture)
    publish_efb_copy_memory(address,bytes.data(),uint32_t(bytes.size()),copy.width,copy.height,copy.format);
}
