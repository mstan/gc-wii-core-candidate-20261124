// SPDX-License-Identifier: MIT
// Included by the retail uniform builder on the FIFO recording owner.
struct RetailUniformSnapshot {
  gfx::Range range{};
  u64 packet{};
  u64 material{};
  std::array<Vec4<float>, 8> textures{};
};
static_assert(sizeof(Vec4<float>) == 4u * sizeof(float));
static thread_local RetailUniformSnapshot retailUniform;
static thread_local u64 retainedUniformDraws{}, newUniformDraws{}, retainedUniformBytes{};

void reset_retail_uniforms() noexcept {
  if (newUniformDraws || retainedUniformDraws) {
    std::fprintf(stderr, "[native-gx-uniform-state] builds=%llu reuses=%llu reused-state-bytes=%llu\n",
        static_cast<unsigned long long>(newUniformDraws),
        static_cast<unsigned long long>(retainedUniformDraws),
        static_cast<unsigned long long>(retainedUniformBytes));
  }
  retailUniform = {};
  retainedUniformDraws = newUniformDraws = retainedUniformBytes = 0;
}
