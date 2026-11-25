// SPDX-License-Identifier: MIT
namespace {
constexpr std::uint32_t kSinkStateMagic = 0x32535847u;
constexpr std::uint32_t kNoTextureSlot = UINT32_MAX;

struct SavedArrayBinding {
  std::uint32_t valid = 0, base = 0, stride = 0;
};

struct SavedTextureBinding {
  std::uint32_t address = 0, size = 0, format = 0, width = 0, height = 0;
  std::uint32_t tlut_address = 0, tlut_format = 0, tlut_entries = 0;
};

struct SavedCoreState {
  std::uint32_t magic = kSinkStateMagic;
  std::uint32_t last_texture = kNoTextureSlot;
  std::uint32_t cull_all = 0;
  std::uint32_t scope = 0, scope_left = 0, scope_part = 0, scope_indexed = 0;
  std::array<SavedArrayBinding, DOL_GX_RECOMP_CP_ARRAY_COUNT> arrays{};
  std::array<SavedTextureBinding, ar::ConsumedDraw::kMaxTexmaps> textures{};
  GxCoreState registers;
  CachedVertexAttrs cached_attrs;
};
static_assert(std::is_trivially_copyable_v<SavedCoreState>);
} // namespace

std::vector<std::uint8_t> GxCoreSink::save_state() const {
  SavedCoreState saved{};
  saved.registers = live_state_;
  saved.cached_attrs = cached_attrs_;
  saved.cull_all = consumer_.cull_all();
  saved.scope = scope_;
  saved.scope_left = scope_left_;
  saved.scope_part = scope_part_;
  saved.scope_indexed = scope_indexed_;
  const auto& last_texture = consumer_.bound_texture();
  if (last_texture.valid) saved.last_texture = last_texture.slot;
  for (std::uint32_t index = 0; index < saved.arrays.size(); ++index) {
    const auto& live = consumer_.array(index);
    const std::uint32_t valid = std::uint32_t(live.base_valid) | (std::uint32_t(live.stride_valid) << 1u);
    saved.arrays[index] = {valid, live.base, live.stride};
  }
  for (std::uint32_t index = 0; index < saved.textures.size(); ++index) {
    const auto& live = consumer_.bound_texture_at(index);
    if (live.valid)
      saved.textures[index] = {live.address, live.size, live.format, live.width, live.height,
                                live.tlut_address, live.tlut_format, live.tlut_entries};
  }
  std::vector<std::uint8_t> output(sizeof(saved));
  std::memcpy(output.data(), &saved, sizeof(saved));
  return output;
}

bool GxCoreSink::load_state(const std::uint8_t* data, std::size_t size) {
  SavedCoreState saved;
  if (!data || size != sizeof(saved)) return false;
  std::memcpy(&saved, data, sizeof(saved));
  if (saved.magic != kSinkStateMagic || saved.cull_all > 1 || saved.scope_indexed > 1 ||
      (saved.last_texture != kNoTextureSlot &&
       (saved.last_texture >= saved.textures.size() || !saved.textures[saved.last_texture].size)))
    return false;
  for (const auto& binding : saved.arrays)
    if (binding.valid > 3 || binding.stride > UINT8_MAX) return false;
  for (const auto& binding : saved.textures)
    if (binding.size && (!binding.width || !binding.height || binding.tlut_entries > UINT16_MAX))
      return false;

  consumer_.flush_assembly();
  consumer_.reset();
  std::uint64_t sequence = 0;
  const auto restore_state = [&](ar::RenderStateKind kind, std::uint32_t index, std::uint32_t value) {
    ar::RenderPacket packet{};
    packet.kind = ar::RenderPacketKind::State;
    packet.sequence = sequence++;
    packet.state = {.kind = kind, .index = index, .value = value};
    return consumer_.submit_packet(packet);
  };
  for (std::uint32_t index = 0; index < saved.arrays.size(); ++index) {
    const auto& binding = saved.arrays[index];
    if ((binding.valid & 1u) && !restore_state(ar::RenderStateKind::CpArrayBase, index, binding.base))
      return false;
    if ((binding.valid & 2u) && !restore_state(ar::RenderStateKind::CpArrayStride, index, binding.stride))
      return false;
  }
  if (!restore_state(ar::RenderStateKind::CullAll, 0, saved.cull_all)) return false;
  const auto restore_texture = [&](std::uint32_t index) {
    const auto& binding = saved.textures[index];
    if (!binding.size) return true;
    ar::RenderPacket packet{};
    packet.kind = ar::RenderPacketKind::Resource;
    packet.sequence = sequence++;
    packet.resource = {.kind = ar::RenderResourceKind::Texture, .index = index,
                       .address = binding.address, .size = binding.size, .format = binding.format,
                       .width = binding.width, .height = binding.height, .tlut_address = binding.tlut_address,
                       .tlut_format = binding.tlut_format, .tlut_entries = binding.tlut_entries};
    return consumer_.submit_packet(packet);
  };
  for (std::uint32_t index = 0; index < saved.textures.size(); ++index)
    if (index != saved.last_texture && !restore_texture(index)) return false;
  if (saved.last_texture != kNoTextureSlot && !restore_texture(saved.last_texture)) return false;
  consumer_.start_packet_stream();

  live_state_ = saved.registers;
  live_state_.renew_version();
  pending_state_ = live_state_;
  cached_attrs_ = saved.cached_attrs;
  since_draw_.clear();
  replay_overflow_ = false;
  scope_ = saved.scope;
  scope_left_ = saved.scope_left;
  scope_part_ = saved.scope_part;
  scope_indexed_ = saved.scope_indexed != 0;
  last_draw_triangles_ = false;
  return true;
}
