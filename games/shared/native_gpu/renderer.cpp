// skate1 - native D3D layer: renderer (see renderer.h)

#include "renderer.h"

#include <string_view>
#include <cfloat>
#include <thread>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define XXH_INLINE_ALL
#include <xxhash.h>
#include <smolv.h>
#ifdef NATIVE_GPU_ZSTD
#include <zstd.h>
#endif

#include <plume_vulkan.h>

#include "shader_cache.h"
#include "shaders/native_shaders.h"

using namespace plume;

namespace native_gpu {

namespace {

// Bindless heap sizes.
constexpr uint32_t kTextureHeapSize = 8192;
constexpr uint32_t kSamplerHeapSize = 256;
// Largest EDRAM surface height: 10 MB of EDRAM.
constexpr uint32_t kMaxSurfaceHeight = 2048;
// Memory export: guest ranges kept in one buffer, and its size.
constexpr uint32_t kMaxExportRegions = 8;
constexpr uint32_t kExportBufferSize = 128u << 20;

// The game's interface: its shaders keep their 16:9 place on wider pictures.
constexpr uint64_t kInterfaceVertexShaders[] = {
    0x9984423845937B08ull,  // simpledraw_p_sc_uv,
    0xD61C8067CBAB6D32ull,  // simpledraw_p_n_sc,
    0xF4C9E67C53263D9Dull,  // simpledraw_p_sc_uv,
    0xBC3E27BBE36EEF1Aull,  // simpledraw_p_c_uv,
    0xD949C2119E3C383Dull,  // simpledraw_p_n_sc_uv,
    0x3EAE4C5DE646DF7Full,  // simpledraw_p_n_c_uv,
    0x0A718F34D79ED591ull,  // minimap,
    0xFE833B61BCE7FABCull,  // simpledraw_p_n_sc_uv,
    0x0190382CCA5FEA26ull,  // simpledraw_p_n_c,
    0x3654AF42C7D5A775ull,  // font,
    0xD5D9BB34F6616C4Eull,  // simpledraw_p_c,
    0x69CDC5D8FD2961B4ull,  // simpledraw_p_n_c_uv,
    0x2F42BE8AE7BE99EAull,  // simpledraw_p_sc,
    0xDED9E9293C2BE681ull,  // simpledraw_p_c_uv,
    0x1CDF21FD205A4EAAull,  // simpledraw_p_sc,
    0x26D45C70F251147Dull,  // font,
    0x63D4FB864AF8378Full,  // simpledraw_p_c,
    0x336B6CF2172DD2FFull,  // simpledraw_p_n_c,
    0x7F31D2881987B4F3ull,  // minimap,
    0x0FFD46E9EB7E10B6ull,  // simpledraw_p_n_sc
};

// XenosRecomp's shared constants (SharedConstants in Unleashed's video.cpp).
struct SharedConstants {
  uint32_t texture2DIndices[16]{};
  uint32_t texture3DIndices[16]{};
  uint32_t textureCubeIndices[16]{};
  uint32_t samplerIndices[16]{};
  uint32_t booleans{};
  uint32_t swappedTexcoords{};
  float halfPixelOffsetX{};
  float halfPixelOffsetY{};
  float alphaThreshold{};
  uint32_t loop16{};
  uint32_t loop17{};
  uint32_t pad{};
  // Guest pixels per host pixel, for the pixel position of shaders that read it.
  float posScaleX{1.0f};
  float posScaleY{1.0f};
  // Fetch constant exponent adjust, and the range of integer formats the host holds normalized.
  float texExpScale[16] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
  // Vertices in window coordinates (PA_CL_VTE_CNTL): scale and offset back into clip space.
  float screenMul[3] = {1.0f, 1.0f, 1.0f};
  float screenAdd[3] = {};
  // Memory export: the buffer's address, and the guest ranges it holds (start, end, offset in it).
  uint64_t exportMemory{};
  // For each slot, four bits: the components a fetch constant marks as unsigned biased (twice the value less one).
  uint32_t texBias[2]{};
  uint32_t exportRegions[kMaxExportRegions][4]{};
};
static_assert(offsetof(SharedConstants, posScaleX) == 288 && offsetof(SharedConstants, texExpScale) == 296 &&
              offsetof(SharedConstants, screenMul) == 360 && offsetof(SharedConstants, screenAdd) == 372 &&
              offsetof(SharedConstants, exportMemory) == 384 && offsetof(SharedConstants, exportRegions) == 400);

// The components (x in bit 0) the fetch constant's sign fields make unsigned biased.
uint32_t TextureBiasBits(const xenos::DeviceState& state, uint32_t slot) {
  const uint32_t word0 = state.U32(xenos::device::kFetchConstants + 24 * slot);
  if ((word0 & 3) != 2) return 0;
  uint32_t bits = 0;
  for (uint32_t c = 0; c < 4; ++c) bits |= uint32_t(((word0 >> (2 + 2 * c)) & 3) == 2) << c;
  return bits;
}

// The factor a fetch multiplies its result by: 2^exponent from fetch constant word 3, and for integer
// formats (num_format 1) the full range, because the host texture is normalized.
float TextureExpScale(const xenos::DeviceState& state, uint32_t slot) {
  const uint32_t word0 = state.U32(xenos::device::kFetchConstants + 24 * slot);
  if ((word0 & 3) != 2) return 1.0f;
  const uint32_t word1 = state.U32(xenos::device::kFetchConstants + 24 * slot + 4);
  const uint32_t word3 = state.U32(xenos::device::kFetchConstants + 24 * slot + 12);
  float scale = std::ldexp(1.0f, int32_t(word3 << 13) >> 26);
  if (word3 & 1) {
    const bool is_signed = ((word0 >> 2) & 0xFF) == 0x55;
    switch (word1 & 0x3F) {
      case 6: scale *= is_signed ? 127.0f : 255.0f; break;
      case 24: case 25: case 26: scale *= is_signed ? 32767.0f : 65535.0f; break;
      default: break;
    }
  }
  return scale;
}

struct DetilePushConstants {
  uint64_t source, target;
  uint32_t source_size, width_blocks, height_blocks, pitch_blocks, block_dwords, block_log2, endian, tiled;
};

struct ResolvePushConstants {
  uint32_t source_index;
  uint32_t mode;  // 0 color * scale, 1 depth packed as D24S8 in 8_8_8_8
  float scale;
  int32_t offset_x;
  int32_t offset_y;
  uint32_t stencil_index;
  int32_t step;
};

RenderStencilFaceDesc StencilFace(uint32_t func, uint32_t fail, uint32_t zpass, uint32_t zfail) {
  RenderStencilFaceDesc face;
  face.compareFunction = RenderComparisonFunction(func + 1);
  face.failOp = RenderStencilOp(fail + 1);
  face.passOp = RenderStencilOp(zpass + 1);
  face.depthFailOp = RenderStencilOp(zfail + 1);
  return face;
}

// XenosRecomp's USAGE_LOCATIONS: vertex input location of (usage, index).
int32_t UsageLocation(uint32_t usage, uint32_t index) {
  static constexpr struct {
    uint32_t usage, index, location;
  } kLocations[] = {
      {0, 0, 0},  {3, 0, 1},  {6, 0, 2},  {7, 0, 3},  {5, 0, 4},  {5, 1, 5},
      {5, 2, 6},  {5, 3, 7},  {10, 0, 8}, {2, 0, 9},  {1, 0, 10}, {10, 1, 11},
      {5, 4, 12}, {5, 5, 13}, {5, 6, 14}, {5, 7, 15}, {0, 1, 15},
  };
  for (const auto& entry : kLocations) {
    if (entry.usage == usage && entry.index == index) return int32_t(entry.location);
  }
  return -1;
}

// XenosRecomp declares normals, tangents and binormals uint4 and decodes them
// itself: packed 10_11_11 (SPEC_CONSTANT_R11G11B10_NORMAL) or float bits.
bool IsRawUsage(uint32_t usage) { return usage == 3 || usage == 6 || usage == 7; }

RenderFormat RawVertexFormat(uint32_t format) {
  using namespace xenos;
  switch (format) {
    case k_10_11_11: return RenderFormat::R32_UINT;
    case k_8_8_8_8: return RenderFormat::R32_UINT;  // packed to 10_11_11 (see Draw)
    case k_32_FLOAT: return RenderFormat::R32_UINT;
    case k_32_32_FLOAT: return RenderFormat::R32G32_UINT;
    case k_32_32_32_FLOAT: return RenderFormat::R32G32B32_UINT;
    case k_32_32_32_32_FLOAT: return RenderFormat::R32G32B32A32_UINT;
    // Half floats, converted by XenosRecomp with SPEC_CONSTANT_HALF_NORMAL.
    case k_16_16_FLOAT: return RenderFormat::R16G16_UINT;
    case k_16_16_16_16_FLOAT: return RenderFormat::R16G16B16A16_UINT;
    default: return RenderFormat::UNKNOWN;
  }
}

RenderFormat VertexFormat(const xenos::VertexFetchInstruction& fetch, uint32_t usage) {
  using namespace xenos;
  const bool s = fetch.is_signed, i = fetch.is_integer;
  switch (fetch.format) {
    case k_8_8_8_8:
      if (usage == 2) {  // BLENDINDICES: uint4
        return s ? RenderFormat::R8G8B8A8_SINT : RenderFormat::R8G8B8A8_UINT;
      }
      // Integer components feed float4 inputs as their values (scaled).
      return i ? (s ? RenderFormat::R8G8B8A8_SSCALED : RenderFormat::R8G8B8A8_USCALED)
               : (s ? RenderFormat::R8G8B8A8_SNORM : RenderFormat::R8G8B8A8_UNORM);
    case k_2_10_10_10:
      return s ? RenderFormat::A2B10G10R10_SNORM : RenderFormat::A2B10G10R10_UNORM;
    case k_16_16:
      return i ? (s ? RenderFormat::R16G16_SSCALED : RenderFormat::R16G16_USCALED)
               : (s ? RenderFormat::R16G16_SNORM : RenderFormat::R16G16_UNORM);
    case k_16_16_16_16:
      return i ? (s ? RenderFormat::R16G16B16A16_SSCALED : RenderFormat::R16G16B16A16_USCALED)
               : (s ? RenderFormat::R16G16B16A16_SNORM : RenderFormat::R16G16B16A16_UNORM);
    case k_10_11_11:  // no such host format: the buffer is narrowed to 8 bits (see Draw)
      return s ? RenderFormat::R8G8B8A8_SNORM : RenderFormat::R8G8B8A8_UNORM;
    case k_16_16_FLOAT: return RenderFormat::R16G16_FLOAT;
    case k_16_16_16_16_FLOAT: return RenderFormat::R16G16B16A16_FLOAT;
    case k_32: return s ? RenderFormat::R32_SINT : RenderFormat::R32_UINT;
    case k_32_32: return s ? RenderFormat::R32G32_SINT : RenderFormat::R32G32_UINT;
    case k_32_32_32_32: return s ? RenderFormat::R32G32B32A32_SINT : RenderFormat::R32G32B32A32_UINT;
    case k_32_FLOAT: return RenderFormat::R32_FLOAT;
    case k_32_32_FLOAT: return RenderFormat::R32G32_FLOAT;
    case k_32_32_32_32_FLOAT: return RenderFormat::R32G32B32A32_FLOAT;
    case k_32_32_32_FLOAT: return RenderFormat::R32G32B32_FLOAT;
    default: return RenderFormat::UNKNOWN;
  }
}

RenderFormat TextureFormat(const xenos::TextureFetch& t) {
  using namespace xenos;
  const bool s = t.is_signed;
  switch (t.format) {
    case kTex_8: return s ? RenderFormat::R8_SNORM : RenderFormat::R8_UNORM;
    case kTex_8_8: return s ? RenderFormat::R8G8_SNORM : RenderFormat::R8G8_UNORM;
    case kTex_8_8_8_8: return s ? RenderFormat::R8G8B8A8_SNORM : RenderFormat::R8G8B8A8_UNORM;
    case kTex_DXT1: return RenderFormat::BC1_UNORM;
    case kTex_DXT2_3: return RenderFormat::BC2_UNORM;
    case kTex_DXT4_5: return RenderFormat::BC3_UNORM;
    case kTex_16_16: return s ? RenderFormat::R16G16_SNORM : RenderFormat::R16G16_UNORM;
    case kTex_16_16_16_16: return s ? RenderFormat::R16G16B16A16_SNORM : RenderFormat::R16G16B16A16_UNORM;
    case kTex_16_16_FLOAT: return RenderFormat::R16G16_FLOAT;
    case kTex_16_16_16_16_FLOAT: return RenderFormat::R16G16B16A16_FLOAT;
    case kTex_32_FLOAT: return RenderFormat::R32_FLOAT;
    case kTex_1_5_5_5: return RenderFormat::A1R5G5B5_UNORM;
    case kTex_16: return s ? RenderFormat::R16_SNORM : RenderFormat::R16_UNORM;
    case kTex_16_FLOAT: return RenderFormat::R16_FLOAT;
    case kTex_24_8: case kTex_24_8_FLOAT: return RenderFormat::R8G8B8A8_UNORM;
    case kTex_DXT5A: return RenderFormat::BC4_UNORM;
    case kTex_DXN: return s ? RenderFormat::BC5_SNORM : RenderFormat::BC5_UNORM;
    default: return RenderFormat::UNKNOWN;
  }
}

// EDRAM color formats (RB_COLOR_INFO) as host render target formats. 16_16
// and 16_16_16_16 hold -32..32 in EDRAM, kept as floats here.
// The _AS_ formats are other views of the same EDRAM bits: draws in one are resolved in the other.
uint32_t EdramFormat(uint32_t color_format) {
  return color_format == 10 ? 2 : color_format == 12 ? 3 : color_format;
}

// What EDRAM holds after a clear (RB_COLOR_CLEAR, the raw 32-bit sample: red in the low byte or 16 bits), read
// in an EDRAM color format. The 16_16 formats keep -32..32 as 1/1024 steps of the 16-bit integer.
RenderColor ClearWordColor(uint32_t colorFormat, uint32_t word) {
  const auto fixed = [](uint32_t bits) { return float(int16_t(bits & 0xFFFF)) / 1024.0f; };
  switch (EdramFormat(colorFormat)) {
    case 4: return RenderColor(fixed(word), fixed(word >> 16), 0.0f, 0.0f);
    case 5: return RenderColor(fixed(word), fixed(word >> 16), fixed(word), fixed(word >> 16));
    default:
      return RenderColor(float(word & 0xFF) / 255.0f, float((word >> 8) & 0xFF) / 255.0f,
                         float((word >> 16) & 0xFF) / 255.0f, float(word >> 24) / 255.0f);
  }
}

// Tile rectangles in row order: the strips of each row joined, then the rows of equal strips.
void AddTileRect(std::vector<RenderRect>& rects, int32_t x, int32_t y, int32_t right, int32_t bottom) {
  if (!rects.empty() && rects.back().right == x && rects.back().top == y) {
    rects.back().right = right;
  } else {
    rects.emplace_back(x, y, right, bottom);
  }
}

void JoinTileRows(std::vector<RenderRect>& rects) {
  size_t out = 0;
  for (size_t i = 0; i < rects.size(); ++i) {
    if (out && rects[out - 1].left == rects[i].left && rects[out - 1].right == rects[i].right &&
        rects[out - 1].bottom == rects[i].top) {
      rects[out - 1].bottom = rects[i].bottom;
    } else {
      rects[out++] = rects[i];
    }
  }
  rects.resize(out);
}

RenderFormat SurfaceFormat(uint32_t color_format) {
  switch (color_format) {
    case 0: case 1: return RenderFormat::R8G8B8A8_UNORM;  // 8_8_8_8 (_GAMMA)
    case 4: case 6: return RenderFormat::R16G16_FLOAT;
    case 5: case 7: case 2: case 3: case 10: case 12: return RenderFormat::R16G16B16A16_FLOAT;
    case 14: return RenderFormat::R32_FLOAT;
    case 15: return RenderFormat::R32G32_FLOAT;
    default: return RenderFormat::R8G8B8A8_UNORM;
  }
}

// The fetch constant's swizzle (3 bits per destination component: 0-3 source
// component, 4 zero, 5 one) as a view component mapping.
RenderComponentMapping SwizzleMapping(uint32_t swizzle) {
  RenderSwizzle components[4];
  for (uint32_t c = 0; c < 4; ++c) {
    const uint32_t select = (swizzle >> (3 * c)) & 7;
    components[c] = select < 4 ? RenderSwizzle(uint32_t(RenderSwizzle::R) + select)
                               : select == 4 ? RenderSwizzle::ZERO : RenderSwizzle::ONE;
  }
  return RenderComponentMapping(components[0], components[1], components[2], components[3]);
}

RenderBlend BlendFactor(uint32_t factor) {
  switch (factor) {
    case 0: return RenderBlend::ZERO;
    case 1: return RenderBlend::ONE;
    case 4: return RenderBlend::SRC_COLOR;
    case 5: return RenderBlend::INV_SRC_COLOR;
    case 6: return RenderBlend::SRC_ALPHA;
    case 7: return RenderBlend::INV_SRC_ALPHA;
    case 8: return RenderBlend::DEST_COLOR;
    case 9: return RenderBlend::INV_DEST_COLOR;
    case 10: return RenderBlend::DEST_ALPHA;
    case 11: return RenderBlend::INV_DEST_ALPHA;
    case 12: case 14: return RenderBlend::BLEND_FACTOR;
    case 13: case 15: return RenderBlend::INV_BLEND_FACTOR;
    case 16: return RenderBlend::SRC_ALPHA_SAT;
    default: return RenderBlend::ONE;
  }
}

RenderBlendOperation BlendOp(uint32_t op) {
  switch (op) {
    case 1: return RenderBlendOperation::SUBTRACT;
    case 2: return RenderBlendOperation::MIN;
    case 3: return RenderBlendOperation::MAX;
    case 4: return RenderBlendOperation::REV_SUBTRACT;
    default: return RenderBlendOperation::ADD;
  }
}


// A texel's bytes in the order the guest's endian mode stores them.
template <uint32_t Bytes>
inline void SwapTexel(uint8_t* texel, uint32_t bytes, uint32_t endian) {
  if constexpr (Bytes == 2) {
    if (endian == 1) std::swap(texel[0], texel[1]);
  } else if constexpr (Bytes == 4) {
    uint32_t v;
    std::memcpy(&v, texel, 4);
    if (endian == 1) v = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
    else if (endian == 2) v = __builtin_bswap32(v);
    else if (endian == 3) v = (v << 16) | (v >> 16);
    std::memcpy(texel, &v, 4);
  } else if constexpr (Bytes != 1) {
    xenos::SwapEndian(texel, bytes, endian);
  }
}

// What a read back resolve writes into guest memory: its texels converted one by one (`texel_of` fills
// the guest texel from the host's and returns false for one to leave alone), swapped to the guest's byte
// order and put where the tiled layout has them. Bytes is the guest texel size, 0 when only known at run time.
struct ScatterRect {
  uint32_t x, y, width, height, host_bytes, bytes, endian, pitch, log2;
  // The host texels this rect held last time, updated as rows are converted. With `compare`, rows that
  // are the same are left alone (the guest memory still holds what they gave), unless their tile row is
  // marked in `overwritten`. The tile rows converted are marked in `written`.
  uint8_t* previous;
  bool compare;
  const uint8_t* overwritten;
  uint8_t* written;
};
template <uint32_t Bytes, typename Convert>
bool ScatterTexels(uint8_t* guest, const uint8_t* data, const ScatterRect& rect, Convert&& texel_of) {
  const uint32_t bytes = Bytes ? Bytes : rect.bytes;
  const size_t rowBytes = size_t(rect.width) * rect.host_bytes;
  bool converted = false;
  for (uint32_t y = 0; y < rect.height; ++y) {
    const uint8_t* row = data + size_t(y) * rowBytes;
    if (rect.previous) {
      const uint32_t tile_row = ((rect.y + y) >> 5) - (rect.y >> 5);
      uint8_t* last = rect.previous + size_t(y) * rowBytes;
      if (rect.compare && !rect.overwritten[tile_row] && !std::memcmp(last, row, rowBytes)) continue;
      std::memcpy(last, row, rowBytes);
      rect.written[tile_row] = 1;
    }
    converted = true;
    for (uint32_t x = 0; x < rect.width; ++x) {
      uint8_t texel[8];
      if (!texel_of(row + size_t(x) * rect.host_bytes, texel)) continue;
      SwapTexel<Bytes>(texel, bytes, rect.endian);
      uint8_t* at = guest + xenos::TiledOffset2D(int32_t(rect.x + x), int32_t(rect.y + y), rect.pitch, rect.log2);
      if constexpr (Bytes != 0) std::memcpy(at, texel, Bytes);
      else std::memcpy(at, texel, bytes);
    }
  }
  return converted;
}

// The integer a page table half float holds, rounded (the host holds 11 as 10.99), by the half's bits.
const uint8_t* PageTableChannels() {
  static const std::array<uint8_t, 65536> table = [] {
    std::array<uint8_t, 65536> t{};
    for (uint32_t h = 0; h < 65536; ++h) {
      const uint32_t exponent = (h >> 10) & 31;
      t[h] = uint8_t(exponent ? uint32_t(((uint64_t(1024 | (h & 1023)) << exponent) + (1u << 24)) >> 25) : 0u);
    }
    return t;
  }();
  return table.data();
}

void CopySwapped32(uint8_t* dst, const uint8_t* src, size_t bytes) {
  for (size_t i = 0; i < bytes; i += 4) {
    const uint32_t value = xenos::LoadBE32(src + i);
    std::memcpy(dst + i, &value, 4);
  }
}

}  // namespace

bool Renderer::Initialize(RenderDevice* device, const uint8_t* physical,
                          std::function<const uint8_t*(uint64_t key)> container, WriteWatcher* watcher) {
  device_ = device;
  watcher_ = watcher;
  physical_ = physical;
  container_ = std::move(container);
  // The game's cache is stored uncompressed (XENOS_RECOMP_NO_ZSTD, no zstd in
  // the Steam Deck build); zstd only where the replay tool links it.
  if (g_spirvCacheCompressedSize == g_spirvCacheDecompressedSize) {
    spirv_.assign(g_compressedSpirvCache, g_compressedSpirvCache + g_spirvCacheCompressedSize);
  } else {
#ifdef NATIVE_GPU_ZSTD
    spirv_.resize(g_spirvCacheDecompressedSize);
    if (ZSTD_decompress(spirv_.data(), spirv_.size(), g_compressedSpirvCache,
                        g_spirvCacheCompressedSize) != spirv_.size()) {
      return false;
    }
#else
    return false;
#endif
  }
  queue_ = device_->createCommandQueue(RenderCommandListType::DIRECT);
  for (Frame& frame : frames_) {
    frame.commands = queue_->createCommandList();
    frame.fence = device_->createCommandFence();
    VkQueryPoolCreateInfo queries{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    queries.queryType = VK_QUERY_TYPE_TIMESTAMP;
    queries.queryCount = 2;
    VkQueryPool pool = VK_NULL_HANDLE;
    if (vkCreateQueryPool(static_cast<VulkanDevice*>(device_)->vk, &queries, nullptr, &pool) == VK_SUCCESS) {
      frame.queries = pool;
    }
    frame.upload = device_->createBuffer(RenderBufferDesc::UploadBuffer(
        kUploadSize, RenderBufferFlag::CONSTANT | RenderBufferFlag::VERTEX | RenderBufferFlag::INDEX));
  }
  commands_ = frames_[0].commands.get();
  UseUploadBuffer(frames_[0].upload.get(), kUploadSize);

  // Bindless heaps and pipeline layout as Unleashed sets them up: 2D, 3D and
  // cube texture heaps, a sampler heap, and 24 bytes of push constants (the
  // vertex, pixel and shared constant buffer addresses).
  RenderDescriptorSetBuilder textures;
  textures.begin();
  textures.addTexture(0, kTextureHeapSize);
  textures.end(true, kTextureHeapSize);
  RenderDescriptorSetBuilder samplers;
  samplers.begin();
  samplers.addSampler(0, kSamplerHeapSize);
  samplers.end(true, kSamplerHeapSize);
  texture_2d_set_ = textures.create(device_);
  texture_3d_set_ = textures.create(device_);
  texture_cube_set_ = textures.create(device_);
  sampler_set_ = samplers.create(device_);
  RenderPipelineLayoutBuilder layout;
  layout.begin(false, true);
  layout.addDescriptorSet(textures);
  layout.addDescriptorSet(textures);
  layout.addDescriptorSet(textures);
  layout.addDescriptorSet(samplers);
  layout.addPushConstant(0, 4, 24, RenderShaderStageFlag::VERTEX | RenderShaderStageFlag::PIXEL);
  layout.end();
  layout_ = layout.create(device_);
  RenderPipelineLayoutBuilder resolveLayout;
  resolveLayout.begin(false, false);
  resolveLayout.addDescriptorSet(textures);
  resolveLayout.addPushConstant(0, 0, sizeof(ResolvePushConstants), RenderShaderStageFlag::PIXEL);
  resolveLayout.end();
  resolve_layout_ = resolveLayout.create(device_);
  resolve_vs_ = device_->createShader(k_resolve_vs, sizeof(k_resolve_vs), "VSMain", RenderShaderFormat::SPIRV);
  resolve_ps_ = device_->createShader(k_resolve_ps, sizeof(k_resolve_ps), "PSMain", RenderShaderFormat::SPIRV);
  pack_ps_ = device_->createShader(k_pack_ps, sizeof(k_pack_ps), "PSPack", RenderShaderFormat::SPIRV);

  // Memory export: the compute shader that lays a texture out of the buffer shaders write guest memory into.
  RenderPipelineLayoutBuilder detileLayout;
  detileLayout.begin(false, false);
  detileLayout.addPushConstant(0, 0, sizeof(DetilePushConstants), RenderShaderStageFlag::COMPUTE);
  detileLayout.end();
  detile_layout_ = detileLayout.create(device_);
  detile_cs_ = device_->createShader(k_detile_cs, sizeof(k_detile_cs), "CSMain", RenderShaderFormat::SPIRV);
  detile_pipeline_ = device_->createComputePipeline(RenderComputePipelineDesc(detile_layout_.get(), detile_cs_.get(), 8, 8, 1));


  // Index 0 of each heap: 1x1 white textures; sampler 0: linear wrap.
  null_2d_ = device_->createTexture(RenderTextureDesc::Texture2D(1, 1, 1, RenderFormat::R8G8B8A8_UNORM));
  null_3d_ = device_->createTexture(RenderTextureDesc::Texture3D(1, 1, 1, 1, RenderFormat::R8G8B8A8_UNORM));
  null_cube_ = device_->createTexture(RenderTextureDesc::Texture(
      RenderTextureDimension::TEXTURE_2D, 1, 1, 1, 1, 6, RenderFormat::R8G8B8A8_UNORM, RenderTextureFlag::CUBE));
  RenderTextureViewDesc cubeView;
  cubeView.format = RenderFormat::R8G8B8A8_UNORM;
  cubeView.dimension = RenderTextureViewDimension::TEXTURE_CUBE;
  cubeView.mipLevels = 1;
  null_cube_view_ = null_cube_->createTextureView(cubeView);
  samplers_.push_back(device_->createSampler(RenderSamplerDesc()));
  sampler_set_->setSampler(0, samplers_.back().get());

  commands_->begin();
  const uint32_t white = 0xFFFFFFFF;
  const uint64_t whiteOffset = Allocate(&white, 4, 512);
  for (RenderTexture* texture : {null_2d_.get(), null_3d_.get(), null_cube_.get()}) {
    Transition(texture, RenderTextureLayout::COPY_DEST);
    for (uint32_t slice = 0; slice < (texture == null_cube_.get() ? 6u : 1u); ++slice) {
      commands_->copyTextureRegion(
          RenderTextureCopyLocation::Subresource(texture, 0, slice),
          RenderTextureCopyLocation::PlacedFootprint(upload_, RenderFormat::R8G8B8A8_UNORM, 1, 1, 1,
                                                     64, whiteOffset));
    }
    Transition(texture, RenderTextureLayout::SHADER_READ);
  }
  texture_2d_set_->setTexture(0, null_2d_.get(), RenderTextureLayout::SHADER_READ);
  texture_3d_set_->setTexture(0, null_3d_.get(), RenderTextureLayout::SHADER_READ);
  texture_cube_set_->setTexture(0, null_cube_.get(), RenderTextureLayout::SHADER_READ, null_cube_view_.get());
  commands_->end();
  const RenderCommandList* lists[] = {commands_};
  queue_->executeCommandLists(lists, 1, nullptr, 0, nullptr, 0, frames_[0].fence.get());
  queue_->waitForCommandFence(frames_[0].fence.get());
  upload_offset_ = 0;
  return true;
}

void Renderer::UseUploadBuffer(RenderBuffer* buffer, uint64_t size) {
  upload_ = buffer;
  upload_memory_ = static_cast<uint8_t*>(buffer->map());  // persistently mapped
  upload_address_ = buffer->getDeviceAddress();
  upload_size_ = size;
  upload_offset_ = 0;
}

void Renderer::WaitFrame(Frame& frame) {
  if (!frame.pending) return;
  const auto wait_start = std::chrono::steady_clock::now();
  queue_->waitForCommandFence(frame.fence.get());
  timings_.gpu_wait_ms +=
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wait_start).count();
  frame.pending = false;
  if (frame.timed) {
    frame.timed = false;
    uint64_t stamps[2];
    if (vkGetQueryPoolResults(static_cast<VulkanDevice*>(device_)->vk, VkQueryPool(frame.queries), 0, 2, sizeof(stamps), stamps,
                              sizeof(uint64_t[1]), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS) {
      timings_.gpu_ms += double(stamps[1] - stamps[0]) *
                         double(static_cast<VulkanDevice*>(device_)->physicalDeviceProperties.limits.timestampPeriod) * 1e-6;
    }
  }
  const auto read_back_start = std::chrono::steady_clock::now();
  // Frames repeat most of what they resolve: rows the host holds as last time stay as they were in guest
  // memory, as long as nothing wrote there since (the pages of the rect's tile rows are watched). Each rect
  // keeps its last texels in a record, found here, with the place of its last entry in this frame.
  std::vector<ReadBackRecord*> records;
  if (watcher_) {
    ++read_back_pass_;
    for (size_t position = 0; position < frame.read_backs.size(); ++position) {
      const Frame::ReadBack& entry = frame.read_backs[position];
      const uint64_t key = XXH3_64bits_withSeed(&entry.target, sizeof(entry.target), entry.address) ^
                           (uint64_t(entry.width) << 40) ^ (uint64_t(entry.height) << 52);
      ReadBackRecord& record = read_back_records_[key];
      record.pass = read_back_pass_;
      record.last_position = position;
      if (record.spans.empty()) {
        const uint32_t tile = 32 * 32 * entry.target.bytes;
        const uint32_t pitch = std::max(entry.target.pitch, entry.target.x + entry.width), tiles = (pitch + 31) / 32;
        for (uint32_t row = entry.target.y >> 5; row <= (entry.target.y + entry.height - 1) >> 5; ++row) {
          record.spans.emplace_back(entry.address + (row * tiles + (entry.target.x >> 5)) * tile,
                                    entry.address + (row * tiles + ((entry.target.x + entry.width - 1) >> 5) + 1) * tile);
        }
      }
      record.overwritten.resize(record.spans.size());
      record.written.resize(record.spans.size());
      records.push_back(&record);
    }
  }
  for (size_t position = 0; position < frame.read_backs.size(); ++position) {
    Frame::ReadBack& entry = frame.read_backs[position];
    // Into guest memory as a resolve lays it out: tiled, in the guest's byte order.
    const uint8_t* data = static_cast<const uint8_t*>(entry.buffer->map());
    const uint32_t bytes = entry.target.bytes;
    uint32_t log2 = 0;
    while ((1u << log2) < bytes) ++log2;
    auto* guest = const_cast<uint8_t*>(physical_) + entry.address;
    const uint32_t pitch = std::max(entry.target.pitch, entry.target.x + entry.width);
    const uint32_t hostBytes = entry.host_bytes;
    const bool swapRB = entry.target.swap_red_blue;
    ReadBackRecord* record = watcher_ ? records[position] : nullptr;
    bool compare = false;
    uint32_t recordAddress = 0, recordSize = 0;
    if (record) {
      const uint32_t band = ((pitch + 31) & ~31u) * 32 * bytes;
      recordAddress = entry.address + (entry.target.y >> 5) * band;
      recordSize = (((entry.target.y + entry.height - 1) >> 5) + 1) * band - (entry.target.y >> 5) * band;
      const size_t size = size_t(entry.width) * entry.height * hostBytes;
      compare = record->texels.size() == size && !watcher_->Dirty(recordAddress, recordSize, record->watch_stamp);
      if (record->texels.size() != size) record->texels.resize(size);
    }
    uint8_t none = 0;
    if (record) std::fill(record->written.begin(), record->written.end(), uint8_t(0));
    const ScatterRect rect{entry.target.x, entry.target.y, entry.width, entry.height, hostBytes, bytes,
                           entry.target.endian, pitch, log2, record ? record->texels.data() : nullptr, compare,
                           record ? record->overwritten.data() : &none, record ? record->written.data() : &none};
    bool converted = false;
    if (entry.target.format == 2 && hostBytes == 4 && bytes == 1) {
      converted = ScatterTexels<1>(guest, data, rect, [&](const uint8_t* host, uint8_t* texel) {
        texel[0] = host[swapRB ? 0 : 2];
        return true;
      });
    } else if (entry.target.format == 3 && hostBytes == 8 && bytes == 2) {
      // The page table: half floats holding the source bytes as integers, written as 1_5_5_5.
      const uint8_t* channels = PageTableChannels();
      converted = ScatterTexels<2>(guest, data, rect, [&](const uint8_t* host, uint8_t* texel) {
        uint16_t h[4];
        std::memcpy(h, host, 8);
        const uint32_t red = channels[h[swapRB ? 0 : 2]], blue = channels[h[swapRB ? 2 : 0]];
        const uint16_t value = uint16_t((channels[h[3]] & 1) << 15 | (red & 31) << 10 | (channels[h[1]] & 31) << 5 | (blue & 31));
        std::memcpy(texel, &value, 2);
        return value != 0;
      });
    } else if (entry.target.format == 3 && hostBytes == 4 && bytes == 2) {
      // 8_8_8_8 in memory order (blue first) to 1_5_5_5.
      converted = ScatterTexels<2>(guest, data, rect, [&](const uint8_t* host, uint8_t* texel) {
        const uint32_t red = host[swapRB ? 0 : 2], blue = host[swapRB ? 2 : 0];
        const uint16_t value = uint16_t((host[3] >> 7) << 15 | (red >> 3) << 10 | (host[1] >> 3) << 5 | (blue >> 3));
        // Frames render late: an empty entry here may predate what memory export wrote since.
        std::memcpy(texel, &value, 2);
        return value != 0;
      });
    } else {
      // 8_8_8_8 resolves come out blue first (undone by the fetch's swizzle); memory holds red first unless the copy swaps.
      const bool swapRed = entry.target.format == xenos::kTex_8_8_8_8 && hostBytes == 4 && !swapRB;
      const auto copy = [&](const uint8_t* host, uint8_t* texel) {
        std::memcpy(texel, host, std::min(bytes, hostBytes));
        if (swapRed) std::swap(texel[0], texel[2]);
        return true;
      };
      switch (bytes) {
        case 1: converted = ScatterTexels<1>(guest, data, rect, copy); break;
        case 2: converted = ScatterTexels<2>(guest, data, rect, copy); break;
        case 4: converted = ScatterTexels<4>(guest, data, rect, copy); break;
        case 8: converted = ScatterTexels<8>(guest, data, rect, copy); break;
        default: converted = ScatterTexels<0>(guest, data, rect, copy); break;
      }
    }
    entry.buffer->unmap();
    if (read_back_buffers_.size() < 128) {
      read_back_buffers_.emplace(uint64_t(entry.width) * entry.height * entry.host_bytes, std::move(entry.buffer));
    }
    if (record && (converted || !compare)) {
      record->watch_stamp = watcher_->Watch(recordAddress, recordSize);
      // What this wrote may be where the texels of a rect written after it are: that one goes again over
      // those tile rows, as it did before rows were left alone. (One written before it was overwritten, as
      // it was then.)
      for (auto& [other_key, other] : read_back_records_) {
        if (&other == record || other.pass != read_back_pass_ || other.last_position <= position) continue;
        for (size_t a = 0, b = 0; a < record->spans.size() && b < other.spans.size();) {
          if (record->written[a] && record->spans[a].first < other.spans[b].second &&
              other.spans[b].first < record->spans[a].second) {
            other.overwritten[b] = 1;
          }
          (record->spans[a].second < other.spans[b].second ? a : b)++;
        }
      }
    }
    // Written over, and this rect went again over it: nothing is left to redo.
    if (record) std::fill(record->overwritten.begin(), record->overwritten.end(), uint8_t(0));
    auto it = resolved_.find(entry.address);
    if (watcher_ && it != resolved_.end()) {
      it->second.watch_stamp = watcher_->Watch(entry.address, uint32_t(size_t(entry.target.y + entry.height) * pitch * bytes));
    }
  }
  timings_.read_back_ms +=
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - read_back_start).count();
  frame.read_backs.clear();
  frame.retired_framebuffers.clear();
  frame.retired_buffers.clear();
  frame.retired_views.clear();
  frame.retired_textures.clear();
  for (const auto& [dimension, index] : frame.retired_slots) free_slots_[dimension - 1].push_back(index);
  frame.retired_slots.clear();
}

uint32_t Renderer::AllocateSlot(uint32_t dimension) {
  std::vector<uint32_t>& free = free_slots_[dimension - 1];
  if (!free.empty()) {
    const uint32_t index = free.back();
    free.pop_back();
    return index;
  }
  uint32_t& next = dimension == 2 ? next_3d_ : dimension == 3 ? next_cube_ : next_2d_;
  if (next >= kTextureHeapSize) {
    ++stats_["texture heap full"];
    return 0;  // the null texture
  }
  return next++;
}

void Renderer::WaitIdle() {
  for (Frame& frame : frames_) WaitFrame(frame);
}

uint64_t Renderer::Allocate(const void* data, uint64_t size, uint64_t alignment) {
  upload_offset_ = (upload_offset_ + alignment - 1) & ~(alignment - 1);
  if (upload_offset_ + size > upload_size_) {
    // Full (texture streaming): continue in an overflow buffer that lives
    // until this frame completes.
    ++stats_["upload overflow buffers"];
    const uint64_t overflowSize = std::max<uint64_t>(kUploadSize, size + alignment);
    auto overflow = device_->createBuffer(RenderBufferDesc::UploadBuffer(
        overflowSize, RenderBufferFlag::CONSTANT | RenderBufferFlag::VERTEX | RenderBufferFlag::INDEX));
    UseUploadBuffer(overflow.get(), overflowSize);
    retired_buffers_.push_back(std::move(overflow));
  }
  const uint64_t at = upload_offset_;
  if (data) std::memcpy(upload_memory_ + at, data, size);
  upload_offset_ += size;
  return at;
}

void Renderer::Transition(RenderTexture* texture, RenderTextureLayout layout) {
  RenderTextureLayout& current = layouts_[texture];
  if (current == layout) return;
  current = layout;
  RenderTextureBarrier barrier(texture, layout);
  commands_->barriers(RenderBarrierStage::GRAPHICS | RenderBarrierStage::COPY, nullptr, 0, &barrier, 1);
}

void Renderer::SetTargets(RenderTexture* color, RenderTexture* depth, RenderTexture* color1) {
  Transition(color, RenderTextureLayout::COLOR_WRITE);
  if (color1) Transition(color1, RenderTextureLayout::COLOR_WRITE);
  if (depth) Transition(depth, RenderTextureLayout::DEPTH_WRITE);
  if (current_targets_ == std::make_pair(color, depth) && current_color1_ == color1) return;
  current_color1_ = color1;
  if (color1) {
    auto& pair = mrt_framebuffers_[{color, color1, depth}];
    if (!pair) {
      const RenderTexture* attachments[] = {color, color1};
      pair = device_->createFramebuffer(RenderFramebufferDesc(attachments, 2, depth));
    }
    commands_->setFramebuffer(pair.get());
    current_targets_ = {color, depth};
    return;
  }
  auto& framebuffer = framebuffers_[{color, depth}];
  if (!framebuffer) {
    const RenderTexture* attachments[] = {color};
    framebuffer = device_->createFramebuffer(RenderFramebufferDesc(attachments, 1, depth));
  }
  commands_->setFramebuffer(framebuffer.get());
  current_targets_ = {color, depth};
}

void Renderer::SetMainLayout() {
  if (main_layout_bound_) return;
  bound_pipeline_ = nullptr;  // other passes bind their own state
  has_bound_viewport_ = has_bound_bias_ = false;
  commands_->setGraphicsPipelineLayout(layout_.get());
  commands_->setGraphicsDescriptorSet(texture_2d_set_.get(), 0);
  commands_->setGraphicsDescriptorSet(texture_3d_set_.get(), 1);
  commands_->setGraphicsDescriptorSet(texture_cube_set_.get(), 2);
  commands_->setGraphicsDescriptorSet(sampler_set_.get(), 3);
  main_layout_bound_ = true;
}

void Renderer::BeginFrame() {
  ++frame_number_;
  ++texture_generation_;  // what the game wrote is looked at once a frame
  world_draws_last_frame_ = std::exchange(world_draws_, 0);
  interface_cover_ = false;
  if (render_scale_ != requested_scale_ || render_wide_ != requested_wide_) {
    render_scale_ = requested_scale_;
    render_wide_ = requested_wide_;
    last_color_ = last_depth_ = nullptr;
  }
  last_edram_use_ = {};
  vs_constants_ = ps_constants_ = {};  // pointed into the last frame's snapshot
  Frame& frame = frames_[frame_];
  WaitFrame(frame);
  commands_ = frame.commands.get();
  UseUploadBuffer(frame.upload.get(), kUploadSize);
  commands_->begin();
  if (frame.queries) {
    VkCommandBuffer vk = static_cast<VulkanCommandList*>(commands_)->vk;
    vkCmdResetQueryPool(vk, VkQueryPool(frame.queries), 0, 2);
    vkCmdWriteTimestamp(vk, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VkQueryPool(frame.queries), 0);
    frame.timed = true;
  }
  current_targets_ = {};
  current_color1_ = nullptr;
  main_layout_bound_ = false;
}

void Renderer::EndFrame(uint32_t present_address) {
  Frame& frame = frames_[frame_];
  frame.present_ready = false;
  auto front = resolved_.find(present_address & 0xFFFFF000);
  if (present_address && front != resolved_.end() && front->second.texture) {
    // Pack the front buffer as A2B10G10R10 and read it back.
    Texture& source = front->second;
    if (gamma_ramp_dirty_) {
      gamma_ramp_dirty_ = false;
      if (gamma_texture_) {
        layouts_.erase(gamma_texture_.get());
        retired_textures_.push_back(std::move(gamma_texture_));
        retired_views_.push_back(std::move(gamma_view_));
      }
      gamma_texture_ = device_->createTexture(RenderTextureDesc::Texture(
          RenderTextureDimension::TEXTURE_2D, 256, 1, 1, 1, 1, RenderFormat::R32_UINT, RenderTextureFlag::NONE));
      Transition(gamma_texture_.get(), RenderTextureLayout::COPY_DEST);
      const uint64_t at = Allocate(gamma_ramp_.data(), sizeof(gamma_ramp_), 512);
      commands_->copyTextureRegion(RenderTextureCopyLocation::Subresource(gamma_texture_.get(), 0, 0),
                                   RenderTextureCopyLocation::PlacedFootprint(upload_, RenderFormat::R32_UINT, 256, 1,
                                                                              1, 256, at));
      Transition(gamma_texture_.get(), RenderTextureLayout::SHADER_READ);
      RenderTextureViewDesc view;
      view.format = RenderFormat::R32_UINT;
      view.dimension = RenderTextureViewDimension::TEXTURE_2D;
      view.mipLevels = 1;
      gamma_view_ = gamma_texture_->createTextureView(view);
      if (!gamma_index_) gamma_index_ = AllocateSlot(1);
      texture_2d_set_->setTexture(gamma_index_, gamma_texture_.get(), RenderTextureLayout::SHADER_READ,
                                  gamma_view_.get());
    }
    if (!present_texture_ || present_width_ != source.width || present_height_ != source.height) {
      present_width_ = source.width;
      present_height_ = source.height;
      if (present_texture_) {
        for (auto it = framebuffers_.begin(); it != framebuffers_.end();) {
          if (it->first.first == present_texture_.get()) {
            retired_framebuffers_.push_back(std::move(it->second));
            it = framebuffers_.erase(it);
          } else {
            ++it;
          }
        }
        layouts_.erase(present_texture_.get());
        retired_textures_.push_back(std::move(present_texture_));
      }
      present_texture_ = device_->createTexture(
          RenderTextureDesc::ColorTarget(present_width_, present_height_, RenderFormat::R32_UINT));
    }
    if (frame.present_width != present_width_ || frame.present_height != present_height_) {
      if (frame.present_readback) frame.retired_buffers.push_back(std::move(frame.present_readback));
      frame.present_width = present_width_;
      frame.present_height = present_height_;
      frame.present_readback = device_->createBuffer(
          RenderBufferDesc::ReadbackBuffer(uint64_t(present_width_) * present_height_ * 4));
      frame.present_pixels = static_cast<const uint32_t*>(frame.present_readback->map());
    }
    Transition(source.texture.get(), RenderTextureLayout::SHADER_READ);
    SetTargets(present_texture_.get(), nullptr);
    auto& pipeline = pipelines_[uint64_t(RenderFormat::R32_UINT) | 1ull << 61];
    if (!pipeline) {
      RenderGraphicsPipelineDesc desc;
      desc.pipelineLayout = resolve_layout_.get();
      desc.vertexShader = resolve_vs_.get();
      desc.pixelShader = pack_ps_.get();
      desc.renderTargetFormat[0] = RenderFormat::R32_UINT;
      desc.renderTargetBlend[0] = RenderBlendDesc::Copy();
      desc.renderTargetCount = 1;
      pipeline = device_->createGraphicsPipeline(desc);
    }
    commands_->setGraphicsPipelineLayout(resolve_layout_.get());
    commands_->setGraphicsDescriptorSet(texture_2d_set_.get(), 0);
    main_layout_bound_ = false;
    commands_->setPipeline(pipeline.get());
    ResolvePushConstants push{source.index, 0, 1.0f, 0, 0, gamma_index_};
    commands_->setGraphicsPushConstants(0, &push);
    RenderViewport viewport(0.0f, 0.0f, float(present_width_), float(present_height_));
    RenderRect scissor(0, 0, int32_t(present_width_), int32_t(present_height_));
    commands_->setViewports(&viewport, 1);
    commands_->setScissors(&scissor, 1);
    commands_->drawInstanced(3, 1, 0, 0);
    Transition(present_texture_.get(), RenderTextureLayout::COPY_SOURCE);
    // plume has no texture-to-buffer copy.
    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {present_width_, present_height_, 1};
    vkCmdCopyImageToBuffer(static_cast<VulkanCommandList*>(commands_)->vk,
                           static_cast<VulkanTexture*>(present_texture_.get())->vk,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           static_cast<VulkanBuffer*>(frame.present_readback.get())->vk, 1, &region);
    frame.present_ready = true;
  }
  for (const ReadBackTarget& target : read_back_) {
    auto it = resolved_.find(target.address);
    if (it == resolved_.end() || !it->second.texture || !target.width || !target.height) continue;
    Texture& texture = it->second;
    Frame::ReadBack entry;
    entry.address = target.address;
    entry.width = target.width;
    entry.height = target.height;
    entry.host_bytes = RenderFormatSize(texture.format);
    entry.target = target;
    const uint64_t bytes = uint64_t(entry.width) * entry.height * entry.host_bytes;
    // The same rects come back every frame: their buffers are kept, not created again.
    if (const auto spare = read_back_buffers_.find(bytes); spare != read_back_buffers_.end()) {
      entry.buffer = std::move(spare->second);
      read_back_buffers_.erase(spare);
    } else {
      entry.buffer = device_->createBuffer(RenderBufferDesc::ReadbackBuffer(bytes));
    }
    Transition(texture.texture.get(), RenderTextureLayout::COPY_SOURCE);
    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {int32_t(target.x), int32_t(target.y), 0};
    region.imageExtent = {entry.width, entry.height, 1};
    vkCmdCopyImageToBuffer(static_cast<VulkanCommandList*>(commands_)->vk,
                           static_cast<VulkanTexture*>(texture.texture.get())->vk,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           static_cast<VulkanBuffer*>(entry.buffer.get())->vk, 1, &region);
    Transition(texture.texture.get(), RenderTextureLayout::SHADER_READ);
    frame.read_backs.push_back(std::move(entry));
  }
  read_back_.clear();
  if (frame.timed) {
    vkCmdWriteTimestamp(static_cast<VulkanCommandList*>(commands_)->vk, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                        VkQueryPool(frame.queries), 1);
  }
  commands_->end();
  const RenderCommandList* lists[] = {commands_};
  const auto submit_start = std::chrono::steady_clock::now();
  queue_->executeCommandLists(lists, 1, nullptr, 0, nullptr, 0, frame.fence.get());
  const double submit_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - submit_start).count();
  timings_.submit_ms += submit_ms;
  timings_.worst_submit_ms = std::max(timings_.worst_submit_ms, submit_ms);
  frame.pending = true;
  // What this frame's commands use is released once it has completed.
  for (auto& b : retired_buffers_) frame.retired_buffers.push_back(std::move(b));
  for (auto& t : retired_textures_) frame.retired_textures.push_back(std::move(t));
  for (auto& v : retired_views_) frame.retired_views.push_back(std::move(v));
  frame.retired_slots.insert(frame.retired_slots.end(), retired_slots_.begin(), retired_slots_.end());
  retired_slots_.clear();
  for (auto& f : retired_framebuffers_) frame.retired_framebuffers.push_back(std::move(f));
  retired_buffers_.clear();
  retired_textures_.clear();
  retired_views_.clear();
  retired_framebuffers_.clear();
  // Let the GPU run this frame while the game records the next one: only
  // the previous frame has to be finished (its slot records next).
  frame_ = (frame_ + 1) % kFramesInFlight;
  timings_.worst_texture_frame_ms = std::max(timings_.worst_texture_frame_ms, texture_frame_ms_);
  timings_.worst_buffer_frame_ms = std::max(timings_.worst_buffer_frame_ms, buffer_frame_ms_);
  texture_frame_ms_ = buffer_frame_ms_ = 0.0;
  WaitFrame(frames_[frame_]);
  presented_ = &frames_[frame_];
}

void Renderer::SetGammaRamp(const uint32_t* entries) {
  if (std::memcmp(entries, gamma_ramp_.data(), sizeof(gamma_ramp_))) {
    std::memcpy(gamma_ramp_.data(), entries, sizeof(gamma_ramp_));
    gamma_ramp_dirty_ = true;
  }
}

void Renderer::LoadPipelineCache(const std::string& path) {
  pipeline_cache_path_ = path;
  std::vector<char> data;
  if (FILE* file = std::fopen(path.c_str(), "rb")) {
    std::fseek(file, 0, SEEK_END);
    data.resize(size_t(std::max(std::ftell(file), 0L)));
    std::fseek(file, 0, SEEK_SET);
    data.resize(std::fread(data.data(), 1, data.size(), file));
    std::fclose(file);
  }
  auto* device = static_cast<VulkanDevice*>(device_);
  VkPipelineCacheCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
  // The driver ignores data from another GPU or version.
  info.initialDataSize = data.size();
  info.pInitialData = data.data();
  if (vkCreatePipelineCache(device->vk, &info, nullptr, &device->pipelineCache) != VK_SUCCESS) {
    info.initialDataSize = 0;
    info.pInitialData = nullptr;
    vkCreatePipelineCache(device->vk, &info, nullptr, &device->pipelineCache);
  }
}

void Renderer::SavePipelineCache() {
  auto* device = static_cast<VulkanDevice*>(device_);
  if (!unsaved_pipelines_ || !device->pipelineCache || pipeline_cache_saving_.exchange(true)) return;
  unsaved_pipelines_ = 0;
  std::thread([this, device] {
    size_t size = 0;
    vkGetPipelineCacheData(device->vk, device->pipelineCache, &size, nullptr);
    std::vector<char> data(size);
    if (vkGetPipelineCacheData(device->vk, device->pipelineCache, &size, data.data()) == VK_SUCCESS) {
      const std::string temporary = pipeline_cache_path_ + ".tmp";
      if (FILE* file = std::fopen(temporary.c_str(), "wb")) {
        const bool written = std::fwrite(data.data(), 1, size, file) == size;
        std::fclose(file);
        if (written) std::rename(temporary.c_str(), pipeline_cache_path_.c_str());
      }
    }
    pipeline_cache_saving_ = false;
  }).detach();
}

Renderer::Submitted Renderer::submitted() const {
  const Frame& frame = frames_[(frame_ + kFramesInFlight - 1) % kFramesInFlight];
  if (!frame.present_ready) return {};
  return {frame.fence.get(), frame.present_pixels, frame.present_width, frame.present_height};
}

void Renderer::WaitSubmitted(const Submitted& submitted) {
  // Not waitForCommandFence, which resets the fence the renderer thread will wait on.
  auto* fence = static_cast<const VulkanCommandFence*>(static_cast<const RenderCommandFence*>(submitted.fence));
  vkWaitForFences(fence->device->vk, 1, &fence->vk, VK_TRUE, UINT64_MAX);
}

Renderer::Shader* Renderer::GetShader(uint64_t key) {
  auto [it, inserted] = shaders_.try_emplace(key);
  Shader& info = it->second;
  if (inserted) {
    const ShaderCacheEntry* begin = g_shaderCacheEntries;
    const ShaderCacheEntry* end = begin + g_shaderCacheEntryCount;
    const ShaderCacheEntry* entry = std::lower_bound(
        begin, end, key, [](const ShaderCacheEntry& lhs, uint64_t rhs) { return lhs.hash < rhs; });
    if (entry == end || entry->hash != key) return nullptr;
    info.entry = entry;
    // The cache's SPIR-V is smol-v encoded.
    const uint8_t* encoded = &spirv_[entry->spirvOffset];
    std::vector<uint8_t> decoded(smolv::GetDecodedBufferSize(encoded, entry->spirvSize));
    smolv::Decode(encoded, entry->spirvSize, decoded.data(), decoded.size());
    info.shader = device_->createShader(decoded.data(), decoded.size(), "main", RenderShaderFormat::SPIRV);
    if (const uint8_t* data = container_ ? container_(key) : nullptr) {
      const uint32_t shader = xenos::LoadBE32(data + 0x18);
      info.microcode_offset = xenos::LoadBE32(data + shader);
      if (xenos::LoadBE32(data) & 1) {
        // VertexShader: field18 at +0x18 and the element count at +0x1C of
        // the Shader struct; elements after field18 dwords from +0x24.
        const uint32_t field18 = xenos::LoadBE32(data + shader + 0x18);
        const uint32_t count = xenos::LoadBE32(data + shader + 0x1C);
        for (uint32_t i = 0; i < count; ++i) {
          const uint32_t value = xenos::LoadBE32(data + shader + 0x24 + 4 * (field18 + i));
          const uint32_t address = value & 0xFFF;
          // The original vfetch's destination register (from the container's
          // microcode): what the recompiled shader reads the element from.
          const uint32_t virtualSize = xenos::LoadBE32(data + 4);
          const uint8_t* instruction = data + virtualSize + info.microcode_offset + address * 12;
          info.elements.push_back({address, (value >> 12) & 0xF, (value >> 16) & 0xF,
                                   (xenos::LoadBE32(instruction) >> 12) & 0x3F,
                                   xenos::LoadBE32(instruction + 4) & 0xFFF});
        }
      }
    }
  }
  return info.entry ? &info : nullptr;
}

uint32_t Renderer::ResolvedIndex(Texture& texture, uint32_t swizzle) {
  if (swizzle == 0x688) return texture.index;  // xyzw (0, 1, 2, 3)
  auto& [view, index] = texture.swizzled_views[swizzle];
  if (!view) {
    RenderTextureViewDesc desc;
    desc.format = texture.format;
    desc.dimension = RenderTextureViewDimension::TEXTURE_2D;
    desc.mipLevels = 1;
    desc.componentMapping = SwizzleMapping(swizzle);
    view = texture.texture->createTextureView(desc);
    if (!index) index = AllocateSlot(1);
    texture_2d_set_->setTexture(index, texture.texture.get(), RenderTextureLayout::SHADER_READ, view.get());
  }
  return index;
}

const RenderBuffer* Renderer::GetGuestBuffer(uint32_t address, uint32_t size, uint64_t layout,
                                             const std::function<void(uint8_t*)>& convert) {
  const uint64_t key = XXH3_64bits_withSeed(&address, 4, layout ^ uint64_t(size) << 32);
  GuestBuffer& cached = guest_buffers_[key];
  if (cached.buffer && !(watcher_ && watcher_->Dirty(address, size, cached.watch_stamp))) {
    static const bool verify = getenv("NATIVE_GPU_VERIFY_TEXTURES") != nullptr;  // debugging
    if (verify) {
      const uint64_t hash = XXH3_64bits(physical_ + address, size);
      if (hash != cached.content_hash) {
        fprintf(stderr, "STALE buffer %08X size %X stamp %u\n", address, size, cached.watch_stamp);
        cached.content_hash = hash;
      }
    }
    return cached.buffer.get();
  }
  struct BufferTimer {
    Renderer* renderer;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    ~BufferTimer() {
      const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
      renderer->timings_.buffer_ms += ms;
      renderer->buffer_frame_ms_ += ms;
    }
  } buffer_timer{this};
  if (cached.buffer) {
    // A write nearby or of the same data (pages are watched as a whole): keep it.
    const uint64_t hash = XXH3_64bits(physical_ + address, size);
    if (hash == cached.content_hash) {
      cached.watch_stamp = watcher_->Watch(address, size);
      return cached.buffer.get();
    }
    // Written to by the game: the frame may still use the old copy.
    retired_buffers_.push_back(std::move(cached.buffer));
    ++stats_["buffers re-uploaded"];
  } else {
    ++stats_["buffers"];
  }
  if (watcher_) cached.watch_stamp = watcher_->Watch(address, size);
  cached.content_hash = XXH3_64bits(physical_ + address, size);
  cached.buffer = device_->createBuffer(
      RenderBufferDesc::UploadBuffer(std::max(size, 4u), RenderBufferFlag::VERTEX | RenderBufferFlag::INDEX));
  // Converted in cached memory: reading the mapped buffer back is uncached, and
  // that alone took 5 ms a frame on the Deck.
  convert_buffer_.assign(physical_ + address, physical_ + address + size);
  convert(convert_buffer_.data());
  uint8_t* data = static_cast<uint8_t*>(cached.buffer->map());
  std::memcpy(data, convert_buffer_.data(), size);
  cached.buffer->unmap();
  return cached.buffer.get();
}

uint32_t Renderer::GetSampler(const xenos::TextureFetch& t) {
  RenderSamplerDesc desc;
  auto address = [](uint32_t clamp) {
    switch (clamp) {
      case 0: return RenderTextureAddressMode::WRAP;
      case 1: return RenderTextureAddressMode::MIRROR;
      case 3: return RenderTextureAddressMode::MIRROR_ONCE;
      case 6: case 7: return RenderTextureAddressMode::BORDER;
      default: return RenderTextureAddressMode::CLAMP;
    }
  };
  desc.addressU = address(t.clamp[0]);
  desc.addressV = address(t.clamp[1]);
  desc.addressW = address(t.clamp[2]);
  desc.magFilter = t.mag_filter == 1 ? RenderFilter::LINEAR : RenderFilter::NEAREST;
  desc.minFilter = t.min_filter == 1 ? RenderFilter::LINEAR : RenderFilter::NEAREST;
  // Mip filter: 0 point, 1 linear, 2 base map only.
  desc.mipmapMode = t.mip_filter == 1 ? RenderMipmapMode::LINEAR : RenderMipmapMode::NEAREST;
  desc.maxLOD = t.mip_filter == 2 ? 0.0f : 16.0f;
  // The table's levels are read by number (setTexLOD).
  if (t.format == xenos::kTex_1_5_5_5 && t.width == 2048) {
    desc.maxLOD = 16.0f;
    desc.mipmapMode = RenderMipmapMode::NEAREST;
  }
  desc.mipLODBias = t.lod_bias;
  // 0 disabled, 1 to 5: 1:1 to 16:1.
  if (t.aniso_filter >= 2 && t.aniso_filter <= 5) {
    desc.anisotropyEnabled = true;
    desc.maxAnisotropy = 1u << (t.aniso_filter - 1);
  }
  const uint32_t fields[] = {uint32_t(desc.addressU), uint32_t(desc.addressV), uint32_t(desc.addressW),
                             uint32_t(desc.magFilter), uint32_t(desc.minFilter), uint32_t(desc.mipmapMode),
                             uint32_t(desc.maxLOD), uint32_t(t.aniso_filter), uint32_t(int32_t(t.lod_bias * 32.0f))};
  auto [it, inserted] = sampler_indices_.try_emplace(XXH3_64bits(fields, sizeof(fields)), 0);
  if (inserted && samplers_.size() < kSamplerHeapSize) {
    it->second = uint32_t(samplers_.size());
    samplers_.push_back(device_->createSampler(desc));
    sampler_set_->setSampler(it->second, samplers_.back().get());
  }
  return it->second;
}

uint64_t Renderer::TextureContentHash(const xenos::TextureFetch& t, uint32_t byte_size) const {
  uint64_t hash = XXH3_64bits(physical_ + (t.address & 0x1FFFFFFF), byte_size);
  if (t.mip_address) hash ^= XXH3_64bits(physical_ + (t.mip_address & 0x1FFFFFFF), byte_size);
  return hash;
}

Renderer::ExportRegion* Renderer::FindExportRegion(uint32_t address) {
  address &= 0x1FFFFFFF;
  for (ExportRegion& region : export_regions_) {
    if (address >= region.begin && address < region.end) return &region;
  }
  return nullptr;
}

void Renderer::ExportBarrier() {
  RenderBufferBarrier barrier(export_buffer_.get(), RenderBufferAccess::READ | RenderBufferAccess::WRITE);
  commands_->barriers(RenderBarrierStage::ALL, &barrier, 1, nullptr, 0);
}

// Fills the region from guest memory, as the game left it.
void Renderer::InitializeExportRegion(const ExportRegion& region) {
  const uint32_t size = region.end - region.begin;
  // The draws recorded so far may still be writing here.
  ExportBarrier();
  const uint8_t* source = physical_ + region.begin;
  bool empty = true;
  for (uint32_t i = 0; i + 8 <= size && empty; i += 8) {
    uint64_t word;
    std::memcpy(&word, source + i, 8);
    empty = word == 0;
  }
  if (empty) {
    static_cast<VulkanCommandList*>(commands_)->endActiveRenderPass();
    vkCmdFillBuffer(static_cast<VulkanCommandList*>(commands_)->vk, static_cast<VulkanBuffer*>(export_buffer_.get())->vk,
                    region.offset, size, 0);
  } else {
    auto staging = device_->createBuffer(RenderBufferDesc::UploadBuffer(size));
    std::memcpy(staging->map(), source, size);
    staging->unmap();
    commands_->copyBufferRegion(export_buffer_->at(region.offset), staging->at(0), size);
    retired_buffers_.push_back(std::move(staging));
  }
  ExportBarrier();
}

// Makes room for what a draw's memory export writes: pages of guest memory, in the export buffer.
void Renderer::AddExportRange(uint32_t address, uint32_t size) {
  const uint32_t begin = address & 0x1FFFF000;
  const uint32_t end = uint32_t(std::min<uint64_t>(0x20000000, (uint64_t(address & 0x1FFFFFFF) + size + 0xFFF) & ~0xFFFull));
  if (end <= begin) return;
  if (!export_buffer_) {
    // Only for titles that use memory export.
    export_buffer_ = device_->createBuffer(RenderBufferDesc::DefaultBuffer(
        kExportBufferSize, RenderBufferFlag::CONSTANT | RenderBufferFlag::STORAGE | RenderBufferFlag::VERTEX));
  }
  ExportRegion* inside = nullptr;
  ExportRegion merged;
  merged.begin = begin;
  merged.end = end;
  std::vector<ExportRegion> covered;
  for (ExportRegion& region : export_regions_) {
    if (region.begin <= begin && end <= region.end) inside = &region;
    if (region.begin <= end && begin <= region.end) {
      merged.begin = std::min(merged.begin, region.begin);
      merged.end = std::max(merged.end, region.end);
      covered.push_back(region);
    }
  }
  if (inside) return;
  const uint32_t aligned = (export_used_ + 255) & ~255u;
  if (uint64_t(aligned) + (merged.end - merged.begin) > kExportBufferSize ||
      export_regions_.size() - covered.size() >= kMaxExportRegions) {
    ++stats_["memory export range skipped: no room"];
    return;
  }
  merged.offset = aligned;
  export_used_ = aligned + (merged.end - merged.begin);
  merged.generation = ++export_generation_;
  InitializeExportRegion(merged);
  // What the smaller regions held goes over the guest memory it was filled with.
  for (const ExportRegion& old : covered) {
    commands_->copyBufferRegion(export_buffer_->at(merged.offset + (old.begin - merged.begin)),
                                export_buffer_->at(old.offset), old.end - old.begin);
    export_regions_.erase(std::find_if(export_regions_.begin(), export_regions_.end(),
                                       [&](const ExportRegion& region) { return region.begin == old.begin; }));
  }
  if (!covered.empty()) ExportBarrier();
  export_regions_.push_back(merged);
  ++stats_["memory export regions"];
}

// A texture a draw fetches from memory export's region: laid out by a compute shader, in place.
Renderer::Texture* Renderer::GetExportTexture(const xenos::TextureFetch& t, const ExportRegion& region, Texture& guest,
                                              bool inserted) {
  const RenderFormat format = TextureFormat(t);
  const xenos::TextureBlock block = xenos::GetTextureBlock(t.format);
  if (t.dimension != 1 || format == RenderFormat::UNKNOWN || block.bytes < 4 || xenos::GetMipLevels(t).size() != 1) {
    return nullptr;
  }
  if (guest.texture && guest.export_generation == region.generation) return &guest;
  const uint32_t blocksX = (t.width + block.size - 1) / block.size;
  const uint32_t blocksY = (t.height + block.size - 1) / block.size;
  const uint64_t linearSize = uint64_t(blocksX) * blocksY * block.bytes;
  if (!export_scratch_ || export_scratch_size_ < linearSize) {
    if (export_scratch_) retired_buffers_.push_back(std::move(export_scratch_));
    export_scratch_size_ = std::max<uint64_t>(linearSize, 16ull << 20);
    export_scratch_ = device_->createBuffer(
        RenderBufferDesc::DefaultBuffer(export_scratch_size_, RenderBufferFlag::CONSTANT | RenderBufferFlag::STORAGE));
  }
  uint32_t log2 = 0;
  while ((1u << log2) < block.bytes) ++log2;
  const uint32_t address = t.address & 0x1FFFFFFF;
  DetilePushConstants push{};
  push.source = export_buffer_->getDeviceAddress() + region.offset + (address - region.begin);
  push.target = export_scratch_->getDeviceAddress();
  push.source_size = region.end - address;
  push.width_blocks = blocksX;
  push.height_blocks = blocksY;
  push.pitch_blocks = std::max(t.pitch, t.width) / block.size;
  push.block_dwords = block.bytes / 4;
  push.block_log2 = log2;
  push.endian = t.endian;
  push.tiled = t.tiled;
  // The export draws wrote the buffer; the copy of the last texture is done with the scratch.
  RenderBufferBarrier barriers[] = {
      {export_buffer_.get(), RenderBufferAccess::READ | RenderBufferAccess::WRITE},
      {export_scratch_.get(), RenderBufferAccess::READ | RenderBufferAccess::WRITE}};
  commands_->barriers(RenderBarrierStage::ALL, barriers, 2, nullptr, 0);
  commands_->setComputePipelineLayout(detile_layout_.get());
  commands_->setPipeline(detile_pipeline_.get());
  commands_->setComputePushConstants(0, &push);
  commands_->dispatch((blocksX + 7) / 8, (blocksY + 7) / 8, 1);
  commands_->barriers(RenderBarrierStage::ALL, &barriers[1], 1, nullptr, 0);
  if (!guest.texture) {
    guest.format = format;
    guest.width = t.width;
    guest.height = t.height;
    guest.dimension = t.dimension;
    guest.texture = device_->createTexture(
        RenderTextureDesc::Texture(RenderTextureDimension::TEXTURE_2D, t.width, t.height, 1, 1, 1, format, RenderTextureFlag::NONE));
    RenderTextureViewDesc view;
    view.format = format;
    view.dimension = RenderTextureViewDimension::TEXTURE_2D;
    view.mipLevels = 1;
    view.componentMapping = SwizzleMapping(t.swizzle);
    guest.view = guest.texture->createTextureView(view);
    if (!guest.index) guest.index = AllocateSlot(1);
    texture_2d_set_->setTexture(guest.index, guest.texture.get(), RenderTextureLayout::SHADER_READ, guest.view.get());
    ++texture_generation_;
  }
  Transition(guest.texture.get(), RenderTextureLayout::COPY_DEST);
  commands_->copyTextureRegion(
      RenderTextureCopyLocation::Subresource(guest.texture.get()),
      RenderTextureCopyLocation::PlacedFootprint(export_scratch_.get(), format, t.width, t.height, 1, blocksX * block.size, 0));
  Transition(guest.texture.get(), RenderTextureLayout::SHADER_READ);
  guest.export_generation = region.generation;
  ++stats_[inserted ? "textures from memory export" : "textures from memory export again"];
  return &guest;
}

Renderer::Texture* Renderer::GetGuestTexture(const xenos::TextureFetch& t) {
  const uint32_t fields[] = {t.format, t.endian, t.tiled, t.pitch, t.address, t.mip_address, t.dimension,
                             t.width, t.height, t.depth, t.swizzle, t.is_signed};
  auto [it, inserted] = guest_textures_.try_emplace(XXH3_64bits(fields, sizeof(fields)));
  Texture& guest = it->second;
  if (t.format == xenos::kTex_1_5_5_5 && t.width == 2048 && t.mip_address) {
    // The virtual texture table: the game rewrites it block by block through resolves and reads
    // it back in the same frame, so it lives on the host (see CopyResolveToTable).
    if (guest.texture) return &guest;
    table_levels_ = xenos::GetMipLevels(t);
    // The fetch is integer (the shaders scale by 1/255): raw values, which unorm cannot hold.
    guest.format = RenderFormat::R16G16B16A16_FLOAT;
    guest.width = t.width;
    guest.height = t.height;
    guest.dimension = t.dimension;
    guest.texture = device_->createTexture(RenderTextureDesc::Texture(
        RenderTextureDimension::TEXTURE_2D, t.width, t.height, 1, uint32_t(table_levels_.size()), 1, guest.format,
        RenderTextureFlag::NONE));
    Transition(guest.texture.get(), RenderTextureLayout::COPY_DEST);
    for (uint32_t level = 0; level < table_levels_.size(); ++level) {
      const xenos::MipLevel& m = table_levels_[level];
      const uint64_t size = uint64_t(m.width) * m.height * 8;
      if (size > 256 * 256 * 8) {
        // Too much for the frame's upload buffer in one go: left undefined would read as garbage.
        const uint32_t rows = m.height;
        for (uint32_t y = 0; y < rows; y += 128) {
          const uint32_t count = std::min(128u, rows - y);
          const uint64_t part = uint64_t(m.width) * count * 8;
          const uint64_t at = Allocate(nullptr, part, 512);
          std::memset(upload_memory_ + at, 0, part);
          commands_->copyTextureRegion(RenderTextureCopyLocation::Subresource(guest.texture.get(), level, 0),
                                       RenderTextureCopyLocation::PlacedFootprint(upload_, guest.format, m.width, count, 1, m.width, at),
                                       0, y, 0);
        }
        continue;
      }
      const uint64_t at = Allocate(nullptr, size, 512);
      std::memset(upload_memory_ + at, 0, size);
      commands_->copyTextureRegion(RenderTextureCopyLocation::Subresource(guest.texture.get(), level, 0),
                                   RenderTextureCopyLocation::PlacedFootprint(upload_, guest.format, m.width, m.height, 1, m.width, at));
    }
    Transition(guest.texture.get(), RenderTextureLayout::SHADER_READ);
    RenderTextureViewDesc view;
    view.format = guest.format;
    view.dimension = RenderTextureViewDimension::TEXTURE_2D;
    view.mipLevels = uint32_t(table_levels_.size());
    // No swizzle: the resolve's red/blue swap and the fetch's undo each other.
    guest.view = guest.texture->createTextureView(view);
    guest.index = AllocateSlot(1);
    texture_2d_set_->setTexture(guest.index, guest.texture.get(), RenderTextureLayout::SHADER_READ, guest.view.get());
    table_texture_ = guest.texture.get();
    ++texture_generation_;
    return &guest;
  }
  const uint32_t byteSize = xenos::TextureByteSize(t);
  if (const ExportRegion* region = FindExportRegion(t.address)) {
    if (Texture* texture = GetExportTexture(t, *region, guest, inserted)) return texture;
  }
  static const bool verifyTextures = getenv("NATIVE_GPU_VERIFY_TEXTURES") != nullptr;  // debugging
  if (!inserted && !verifyTextures && guest.texture &&
      !(watcher_ && (watcher_->Dirty(t.address & 0x1FFFFFFF, byteSize, guest.watch_stamp) ||
                     (t.mip_address && watcher_->Dirty(t.mip_address & 0x1FFFFFFF, byteSize, guest.watch_stamp))))) {
    return &guest;
  }
  struct UploadTimer {
    Renderer* renderer;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    double elapsed() const {
      return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }
    ~UploadTimer() {
      const double ms = elapsed();
      renderer->timings_.texture_ms += ms;
      renderer->texture_frame_ms_ += ms;
    }
  } upload_timer{this};
  if (!inserted) {
    if (verifyTextures && guest.texture) {
      const uint64_t hash = TextureContentHash(t, byteSize);
      if (hash != guest.content_hash &&
          !(watcher_ && watcher_->Dirty(t.address & 0x1FFFFFFF, byteSize, guest.watch_stamp))) {
        fprintf(stderr, "STALE %08X %ux%u fmt %u size %X stamp %u\n", t.address, t.width, t.height, t.format,
                byteSize, guest.watch_stamp);
        guest.content_hash = hash;
      }
    }
    if (!guest.texture ||
        !(watcher_ && (watcher_->Dirty(t.address & 0x1FFFFFFF, byteSize, guest.watch_stamp) ||
                       (t.mip_address && watcher_->Dirty(t.mip_address & 0x1FFFFFFF, byteSize, guest.watch_stamp))))) {
      return guest.texture ? &guest : nullptr;
    }
    // Streaming rewrites many textures at once: past the frame's budget, keep
    // the old one for now rather than stall the frame. It stays dirty.
    constexpr double kUpdateBudgetMs = 5.0;
    if (guest.texture && texture_frame_ms_ + upload_timer.elapsed() > kUpdateBudgetMs) {
      ++timings_.textures_deferred;
      return &guest;
    }
    // A write nearby or of the same data (pages are watched as a whole): keep it.
    if (guest.texture) {
      const uint64_t hash = TextureContentHash(t, byteSize);
      if (hash == guest.content_hash) {
        guest.watch_stamp = watcher_->Watch(t.address & 0x1FFFFFFF, byteSize);
        if (t.mip_address) watcher_->Watch(t.mip_address & 0x1FFFFFFF, byteSize);
        ++timings_.textures_unchanged;
        return &guest;
      }
    }
    // Written to by the game: upload again into a new texture, same slot.
    layouts_.erase(guest.texture.get());
    retired_textures_.push_back(std::move(guest.texture));
    retired_views_.push_back(std::move(guest.view));
    if (guest.index) retired_slots_.emplace_back(guest.dimension, guest.index);
    guest.index = 0;
    ++stats_["textures re-uploaded"];
  }
  ++timings_.textures;
  ++texture_generation_;
  // The mip chain lives elsewhere and fits in the size of level 0.
  if (watcher_) {
    const auto watch_start = std::chrono::steady_clock::now();
    guest.watch_stamp = watcher_->Watch(t.address & 0x1FFFFFFF, byteSize);
    if (t.mip_address) watcher_->Watch(t.mip_address & 0x1FFFFFFF, byteSize);
    timings_.texture_watch_ms +=
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - watch_start).count();
  }
  guest.content_hash = TextureContentHash(t, byteSize);
  guest.dimension = t.dimension;
  const RenderFormat format = TextureFormat(t);
  if (t.dimension == 2 && format != RenderFormat::UNKNOWN) {
    // Volume (color grading LUTs).
    const uint64_t size = uint64_t(t.width) * t.height * t.depth * xenos::GetTextureBlock(t.format).bytes;
    const uint64_t at = Allocate(nullptr, size, 512);
    convert_buffer_.resize(size);
    if (!xenos::UntileTexture3D(physical_, t, convert_buffer_.data())) {
      ++stats_["texture skipped: linear or compressed 3D"];
      return nullptr;
    }
    std::memcpy(upload_memory_ + at, convert_buffer_.data(), size);
    guest.format = format;
    guest.width = t.width;
    guest.height = t.height;
    guest.texture = device_->createTexture(RenderTextureDesc::Texture3D(t.width, t.height, t.depth, 1, format));
    Transition(guest.texture.get(), RenderTextureLayout::COPY_DEST);
    commands_->copyTextureRegion(
        RenderTextureCopyLocation::Subresource(guest.texture.get()),
        RenderTextureCopyLocation::PlacedFootprint(upload_, format, t.width, t.height, t.depth, t.width, at));
    Transition(guest.texture.get(), RenderTextureLayout::SHADER_READ);
    RenderTextureViewDesc view;
    view.format = format;
    view.dimension = RenderTextureViewDimension::TEXTURE_3D;
    view.mipLevels = 1;
    view.componentMapping = SwizzleMapping(t.swizzle);
    guest.view = guest.texture->createTextureView(view);
    if (!guest.index) guest.index = AllocateSlot(2);
    texture_3d_set_->setTexture(guest.index, guest.texture.get(), RenderTextureLayout::SHADER_READ, guest.view.get());
    if (inserted) ++stats_["textures (3D)"];
    return &guest;
  }
  if (t.dimension > 3 || t.dimension == 2 || format == RenderFormat::UNKNOWN) {
    ++stats_["texture skipped: dimension " + std::to_string(t.dimension) + " format " +
             std::to_string(t.format)];
    return nullptr;
  }
  const bool cube = t.dimension == 3;
  const std::vector<xenos::MipLevel> levels = xenos::GetMipLevels(t);
  guest.format = format;
  guest.width = t.width;
  guest.height = t.height;
  const auto create_start = std::chrono::steady_clock::now();
  guest.texture = device_->createTexture(RenderTextureDesc::Texture(
      RenderTextureDimension::TEXTURE_2D, t.width, t.height, 1, uint32_t(levels.size()), cube ? 6 : 1, format,
      cube ? RenderTextureFlag::CUBE : RenderTextureFlag::NONE));
  timings_.texture_create_ms +=
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - create_start).count();
  Transition(guest.texture.get(), RenderTextureLayout::COPY_DEST);
  const xenos::TextureBlock block = xenos::GetTextureBlock(t.format);
  for (uint32_t level = 0; level < levels.size(); ++level) {
    const xenos::MipLevel& m = levels[level];
    const uint32_t blocksX = (m.width + block.size - 1) / block.size;
    const uint32_t blocksY = (m.height + block.size - 1) / block.size;
    for (uint32_t slice = 0; slice < (cube ? 6u : 1u); ++slice) {
      // In cached memory: the upload buffer is slow to write out of order and to
      // read back (the byte swap), which froze the game at each new zone.
      const uint64_t levelSize = uint64_t(blocksX) * blocksY * block.bytes;
      const uint64_t at = Allocate(nullptr, levelSize, 512);
      convert_buffer_.resize(levelSize);
      xenos::UntileLevel(physical_, t, m, slice, convert_buffer_.data());
      std::memcpy(upload_memory_ + at, convert_buffer_.data(), levelSize);
      commands_->copyTextureRegion(
          RenderTextureCopyLocation::Subresource(guest.texture.get(), level, slice),
          RenderTextureCopyLocation::PlacedFootprint(upload_, format, m.width, m.height, 1,
                                                     blocksX * block.size, at));
    }
  }
  Transition(guest.texture.get(), RenderTextureLayout::SHADER_READ);
  if (inserted) ++stats_["texture levels " + std::to_string(levels.size())];
  RenderTextureViewDesc view;
  view.format = format;
  view.dimension = cube ? RenderTextureViewDimension::TEXTURE_CUBE : RenderTextureViewDimension::TEXTURE_2D;
  view.mipLevels = uint32_t(levels.size());
  view.componentMapping = SwizzleMapping(t.swizzle);
  guest.view = guest.texture->createTextureView(view);
  if (!guest.index) guest.index = AllocateSlot(cube ? 3 : 1);
  (cube ? texture_cube_set_ : texture_2d_set_)
      ->setTexture(guest.index, guest.texture.get(), RenderTextureLayout::SHADER_READ, guest.view.get());
  if (inserted) ++stats_[cube ? "textures (cube)" : "textures (2D)"];
  return &guest;
}

Renderer::Texture& Renderer::CreateSurface(uint64_t key, uint32_t width, uint32_t height, RenderFormat format,
                                           bool depth) {
  Texture& surface = surfaces_[key];
  if (surface.texture) return surface;
  surface.format = format;
  surface.width = width;
  surface.height = height;
  surface.texture = device_->createTexture(depth ? RenderTextureDesc::DepthTarget(width, height, format)
                                                 : RenderTextureDesc::ColorTarget(width, height, format));
  RenderTextureViewDesc view;
  view.format = format;
  view.dimension = RenderTextureViewDimension::TEXTURE_2D;
  view.mipLevels = 1;
  surface.view = surface.texture->createTextureView(view);
  surface.index = AllocateSlot(1);
  texture_2d_set_->setTexture(surface.index, surface.texture.get(), RenderTextureLayout::SHADER_READ,
                              surface.view.get());
  if (depth) {
    // plume views a depth target's depth aspect only: swap in a stencil
    // aspect view under a plume view object.
    surface.stencil_view = surface.texture->createTextureView(view);
    auto* vulkanView = static_cast<VulkanTextureView*>(surface.stencil_view.get());
    VkDevice vkDevice = static_cast<VulkanDevice*>(device_)->vk;
    vkDestroyImageView(vkDevice, vulkanView->vk, nullptr);
    VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    info.image = static_cast<VulkanTexture*>(surface.texture.get())->vk;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = VK_FORMAT_D32_SFLOAT_S8_UINT;
    info.subresourceRange = {VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
    vkCreateImageView(vkDevice, &info, nullptr, &vulkanView->vk);
    surface.stencil_index = AllocateSlot(1);
    texture_2d_set_->setTexture(surface.stencil_index, surface.texture.get(), RenderTextureLayout::SHADER_READ,
                                surface.stencil_view.get());
  }
  return surface;
}

// The lowest row the D3D viewport covers.
static uint32_t ViewportBottom(const xenos::DeviceState& state) {
  const float bottom = state.RegF(xenos::reg::PA_CL_VPORT_YOFFSET) +
                       std::fabs(state.RegF(xenos::reg::PA_CL_VPORT_YSCALE));
  return bottom > 0.0f ? uint32_t(std::ceil(bottom)) : 0;
}

// The scene and what works at its size; the small passes stay as they are.
float Renderer::SurfaceScale(uint32_t pitch) const { return pitch >= 1024 && pitch <= 1280 ? render_scale_ : 1.0f; }
float Renderer::SurfaceWide(uint32_t pitch) const { return pitch >= 1024 && pitch <= 1280 ? render_wide_ : 1.0f; }

Renderer::Texture& Renderer::GetColorSurface(const xenos::DeviceState& state) {
  const uint32_t surface = state.Reg(xenos::reg::RB_SURFACE_INFO);
  const uint32_t color = state.Reg(xenos::reg::RB_COLOR_INFO);
  const uint32_t pitch = std::max(surface & 0x3FFF, 1u);
  const uint32_t format = EdramFormat((color >> 16) & 0xF);
  const float scale = SurfaceScale(pitch);
  const uint64_t key = uint64_t(color & 0xFFF) | uint64_t(format) << 12 | uint64_t(pitch) << 16 |
                       uint64_t(scale * 4.0f) << 40 | uint64_t(SurfaceWide(pitch) * 16.0f) << 48;
  if (key != last_color_key_ || !last_color_) {
    last_color_ = &CreateSurface(key, uint32_t(pitch * scale * SurfaceWide(pitch)),
                                 uint32_t(kMaxSurfaceHeight * scale), SurfaceFormat(format), false);
    last_color_->scale = scale;
    last_color_->scale_x = scale * SurfaceWide(pitch);
    last_color_key_ = key;
  }
  return *last_color_;
}

Renderer::Texture& Renderer::GetColor1Surface(const xenos::DeviceState& state) {
  const uint32_t surface = state.Reg(xenos::reg::RB_SURFACE_INFO);
  const uint32_t color = state.Reg(0x2003);  // RB_COLOR1_INFO
  const uint32_t pitch = std::max(surface & 0x3FFF, 1u);
  const uint32_t format = EdramFormat((color >> 16) & 0xF);
  const float scale = SurfaceScale(pitch);
  const uint64_t key = uint64_t(color & 0xFFF) | uint64_t(format) << 12 | uint64_t(pitch) << 16 |
                       uint64_t(scale * 4.0f) << 40 | uint64_t(SurfaceWide(pitch) * 16.0f) << 48;
  Texture& target = CreateSurface(key, uint32_t(pitch * scale * SurfaceWide(pitch)),
                                  uint32_t(kMaxSurfaceHeight * scale), SurfaceFormat(format), false);
  target.scale = scale;
  target.scale_x = scale * SurfaceWide(pitch);
  return target;
}

Renderer::Texture& Renderer::GetDepthSurface(const xenos::DeviceState& state) {
  const uint32_t surface = state.Reg(xenos::reg::RB_SURFACE_INFO);
  const uint32_t depth = state.Reg(xenos::reg::RB_DEPTH_INFO);
  const uint32_t pitch = std::max(surface & 0x3FFF, 1u);
  const float scale = SurfaceScale(pitch);
  const uint64_t key = uint64_t(depth & 0xFFF) | uint64_t(pitch) << 16 | 1ull << 63 | uint64_t(scale * 4.0f) << 40 |
                       uint64_t(SurfaceWide(pitch) * 16.0f) << 48;
  if (key != last_depth_key_ || !last_depth_) {
    last_depth_ = &CreateSurface(key, uint32_t(pitch * scale * SurfaceWide(pitch)),
                                 uint32_t(kMaxSurfaceHeight * scale), RenderFormat::D32_FLOAT_S8_UINT, true);
    last_depth_->scale = scale;
    last_depth_->scale_x = scale * SurfaceWide(pitch);
    last_depth_key_ = key;
  }
  return *last_depth_;
}

void Renderer::TrackEdram(const xenos::DeviceState& state, Texture& color, Texture& depth, bool colorWritten,
                          bool depthWritten, uint32_t height, bool check, std::optional<uint32_t> clear,
                          std::optional<uint32_t> depthClear, bool readOnly) {
  const uint32_t surfaceInfo = state.Reg(xenos::reg::RB_SURFACE_INFO);
  // Draws in a row on the same targets: every tile is already theirs.
  const EdramUse use{&color, &depth, surfaceInfo, state.Reg(xenos::reg::RB_COLOR_INFO),
                     state.Reg(xenos::reg::RB_DEPTH_INFO), height, colorWritten, depthWritten};
  if (!readOnly && check && !std::memcmp(&use, &last_edram_use_, sizeof(use))) return;
  if (!readOnly) last_edram_use_ = use;
  const uint32_t pitch = std::max(surfaceInfo & 0x3FFF, 1u);
  const uint32_t msaa = (surfaceInfo >> 16) & 3;
  height = std::min(height, kMaxSurfaceHeight);
  // A tile is 80x16 samples of 32 bits (40x16 of 64); 2x MSAA doubles the
  // rows, 4x the columns too.
  const auto tiles = [&](uint32_t info, bool wide, auto&& visit) {
    const uint32_t tileWidth = (wide ? 40 : 80) / (msaa >= 2 ? 2 : 1);
    const uint32_t tileHeight = 16 / (msaa >= 1 ? 2 : 1);
    const uint32_t columns = (pitch + tileWidth - 1) / tileWidth;
    const uint32_t rows = (height + tileHeight - 1) / tileHeight;
    for (uint32_t row = 0; row < rows; ++row) {
      for (uint32_t column = 0; column < columns; ++column) {
        visit((info + row * columns + column) & 2047, column, row, tileWidth, tileHeight);
      }
    }
  };
  if (colorWritten) {
    const uint32_t info = state.Reg(xenos::reg::RB_COLOR_INFO);
    const uint32_t format = (info >> 16) & 0xF;
    const bool wide = format == 5 || format == 7 || format == 15;  // 64 bits per sample
    if (check) {
      // On the GPU those tiles now hold the other surface's data, here this
      // surface's last contents; passes that keep what is in EDRAM (Skate's
      // untiled scene path clears depth only, and foliage writes no alpha)
      // must not get an old frame. Clear them, as the tiled path does.
      // The word a clear left in the tile, read in this surface's format; tiles no clear reached read as zero.
      std::map<uint32_t, std::vector<RenderRect>> stale;
      tiles(info & 0xFFF, wide, [&](uint32_t tile, uint32_t column, uint32_t row, uint32_t w, uint32_t h) {
        const Texture* owner = edram_owners_[tile];
        if (!owner || owner == &color || owner == &depth) return;
        const int32_t x = int32_t(column * w * color.scale_x), y = int32_t(row * h * color.scale);
        const int32_t right = int32_t((column + 1) * w * color.scale_x), bottom = int32_t((row + 1) * h * color.scale);
        AddTileRect(stale[edram_clears_[tile].value_or(0)], x, y, right, bottom);
      });
      for (auto& [word, rects] : stale) {
        JoinTileRows(rects);
        // A resolve clears the whole surface when one word covers it: on RADV the rows below the first 256 of
        // a rectangle clear in the resolve's own pass read back as zero.
        const bool whole = readOnly && stale.size() == 1;
        commands_->clearColor(0, ClearWordColor(format, word), whole ? nullptr : rects.data(), whole ? 0 : uint32_t(rects.size()));
        ++stats_["EDRAM: color target overwritten by another surface"];
      }
    }
    if (!readOnly) {
      tiles(info & 0xFFF, wide, [&](uint32_t tile, uint32_t, uint32_t, uint32_t, uint32_t) {
        edram_owners_[tile] = &color;
        edram_clears_[tile] = clear;
        edram_depth_clears_[tile].reset();
      });
    }
  }
  if (depthWritten) {
    // Tiles another depth surface's clear last reached hold that clear's value: the pass keeps no clear of its
    // own (the feedback pass's rectangles cover a corner, the resolve of the pass before cleared every tile).
    std::map<uint32_t, std::vector<RenderRect>> stale;
    if (check) {
      tiles(state.Reg(xenos::reg::RB_DEPTH_INFO) & 0xFFF, false,
            [&](uint32_t tile, uint32_t column, uint32_t row, uint32_t w, uint32_t h) {
              if (!edram_depth_clears_[tile] || edram_owners_[tile] == &depth) return;
              const int32_t x = int32_t(column * w * depth.scale_x), y = int32_t(row * h * depth.scale);
              const int32_t right = int32_t((column + 1) * w * depth.scale_x), bottom = int32_t((row + 1) * h * depth.scale);
              AddTileRect(stale[*edram_depth_clears_[tile]], x, y, right, bottom);
            });
    }
    for (auto& [word, rects] : stale) {
      JoinTileRows(rects);
      commands_->clearDepthStencil(true, true, float(word >> 8) / 16777215.0f, word & 0xFF, rects.data(), uint32_t(rects.size()));
      ++stats_["EDRAM: depth target overwritten by another surface"];
    }
    tiles(state.Reg(xenos::reg::RB_DEPTH_INFO) & 0xFFF, false, [&](uint32_t tile, uint32_t, uint32_t, uint32_t, uint32_t) {
      edram_owners_[tile] = &depth;
      edram_depth_clears_[tile] = depthClear;
    });
  }
}

void Renderer::Clear(const ClearCall& call) {
  Texture& color = GetColorSurface(call.state);
  Texture& depth = GetDepthSurface(call.state);
  SetTargets(color.texture.get(), depth.texture.get());
  RenderRect rect(0, 0, int32_t(color.width), int32_t(color.height));
  if (call.has_rect) {
    rect = RenderRect(int32_t(call.rect[0] * color.scale_x), int32_t(call.rect[1] * color.scale),
                      int32_t(call.rect[2] * color.scale_x), int32_t(call.rect[3] * color.scale));
  }
  TrackEdram(call.state, color, depth, call.flags & 0xF, call.flags & 0x30,
             call.has_rect ? uint32_t(std::max(call.rect[3], 0)) : ViewportBottom(call.state), false,
             call.raw_color ? std::optional<uint32_t>(call.state.Reg(0x231E)) : std::nullopt,
             call.raw_color ? std::optional<uint32_t>(call.state.Reg(0x231D)) : std::nullopt);
  if (call.flags & 0xF) {
    commands_->clearColor(0, call.raw_color ? ClearWordColor((call.state.Reg(xenos::reg::RB_COLOR_INFO) >> 16) & 0xF, call.state.Reg(0x231E))
                                            : RenderColor(call.color[0], call.color[1], call.color[2], call.color[3]),
                          &rect, 1);
  }
  if (call.flags & 0x30) {
    // The Z and stencil arguments: RB_DEPTH_CLEAR in the shadow only follows
    // resolve clears.
    commands_->clearDepthStencil(call.flags & 0x10, call.flags & 0x20, call.depth, call.stencil & 0xFF,
                                 &rect, 1);
  }
  ++stats_["clears"];
}

void Renderer::RetireResolved(Texture& dest) {
  // Still referenced by recorded commands: keep it until the frame ends.
  for (auto it = framebuffers_.begin(); it != framebuffers_.end();) {
    if (it->first.first == dest.texture.get()) {
      retired_framebuffers_.push_back(std::move(it->second));
      it = framebuffers_.erase(it);
    } else {
      ++it;
    }
  }
  if (current_targets_.first == dest.texture.get()) current_targets_ = {};
  current_color1_ = nullptr;
  layouts_.erase(dest.texture.get());
  retired_textures_.push_back(std::move(dest.texture));
  retired_views_.push_back(std::move(dest.view));
  for (auto& [swizzle, entry] : dest.swizzled_views) retired_views_.push_back(std::move(entry.first));
  RetireVolume(dest);
}

// The texture heap slots of a retired resolve texture, free once the frame that may sample it is done.
void Renderer::RetireSpares(uint32_t address) {
  for (auto it = resolved_spares_.lower_bound(uint64_t(address) << 32);
       it != resolved_spares_.end() && (it->first >> 32) == address; it = resolved_spares_.erase(it)) {
    RetireResolved(it->second);
    RetireResolvedSlots(it->second);
  }
}

void Renderer::RetireResolvedSlots(Texture& dest) {
  if (dest.index) retired_slots_.emplace_back(1, dest.index);
  dest.index = 0;
  for (auto& [swizzle, entry] : dest.swizzled_views) {
    if (entry.second) retired_slots_.emplace_back(1, entry.second);
  }
  dest.swizzled_views.clear();
  if (dest.volume_index) retired_slots_.emplace_back(2, dest.volume_index);
  dest.volume_index = 0;
}

void Renderer::RetireVolume(Texture& dest) {
  if (!dest.volume) return;
  layouts_.erase(dest.volume.get());
  retired_textures_.push_back(std::move(dest.volume));
  retired_views_.push_back(std::move(dest.volume_view));
}

// A resolve fetched as a stack of 2D layers by a 3D fetch (the colour grading LUT: 16 layers of
// 32x32 side by side) is kept as a volume, copied again after each resolve.
void Renderer::RefreshVolume(Texture& dest) {
  const uint32_t width = dest.width / dest.layers;
  if (!dest.volume) {
    dest.volume = device_->createTexture(RenderTextureDesc::Texture3D(width, dest.height, dest.layers, 1, dest.format));
    RenderTextureViewDesc view;
    view.format = dest.format;
    view.dimension = RenderTextureViewDimension::TEXTURE_3D;
    view.mipLevels = 1;
    dest.volume_view = dest.volume->createTextureView(view);
    if (!dest.volume_index) dest.volume_index = AllocateSlot(2);
    texture_3d_set_->setTexture(dest.volume_index, dest.volume.get(), RenderTextureLayout::SHADER_READ,
                                dest.volume_view.get());
    ++texture_generation_;
  }
  Transition(dest.texture.get(), RenderTextureLayout::COPY_SOURCE);
  Transition(dest.volume.get(), RenderTextureLayout::COPY_DEST);
  for (uint32_t layer = 0; layer < dest.layers; ++layer) {
    const RenderBox box(int32_t(layer * width), 0, int32_t((layer + 1) * width), int32_t(dest.height));
    commands_->copyTextureRegion(RenderTextureCopyLocation::Subresource(dest.volume.get(), 0, 0),
                                 RenderTextureCopyLocation::Subresource(dest.texture.get(), 0, 0), 0, 0, layer, &box);
  }
  Transition(dest.volume.get(), RenderTextureLayout::SHADER_READ);
  Transition(dest.texture.get(), RenderTextureLayout::SHADER_READ);
}

// A resolve into the virtual texture table: finds the level and block its address means and
// copies there, on the GPU and in command order. The block sits at (source_x, source_y) of the
// resolve's texture: a resolve just below an earlier one at the same pitch joins its texture.
void Renderer::CopyResolveToTable(Texture& source, uint32_t address, uint32_t source_x, uint32_t source_y, uint32_t width,
                                  uint32_t height) {
  int32_t found = -1;
  uint32_t best = 0;
  for (uint32_t level = 0; level < table_levels_.size(); ++level) {
    const xenos::MipLevel& m = table_levels_[level];
    if (address < m.address) continue;
    if (found < 0 || m.address > best || (m.address == best && m.width == width)) {
      found = int32_t(level);
      best = m.address;
    }
  }
  if (found < 0) {
    ++stats_["table: resolve outside the table"];
    return;
  }
  const xenos::MipLevel& m = table_levels_[uint32_t(found)];
  const uint32_t offset = address - m.address;
  uint32_t x = m.x_blocks, y = m.y_blocks;
  if (offset) {
    bool located = false;
    for (uint32_t row = 0; row < m.height && !located; row += 32) {
      for (uint32_t column = 0; column < m.width; column += 32) {
        if (uint32_t(xenos::TiledOffset2D(int32_t(column), int32_t(row), m.pitch_blocks, 1)) == offset) {
          x = column;
          y = row;
          located = true;
          break;
        }
      }
    }
    if (!located) {
      ++stats_["table: block not located"];
      static int logged = 0;
      if (logged++ < 12) {
        fprintf(stderr, "table: %08X level %d at %08X offset %X not located\n", address, found, m.address, offset);
      }
      return;
    }
  }
  width = std::min(width, m.width - std::min(x, m.width));
  height = std::min(height, m.height - std::min(y, m.height));
  if (!width || !height) return;
  ++stats_["table: blocks copied, level " + std::to_string(found)];
  {
    static std::map<uint32_t, int> seen;
    if (seen.size() < 40 && !seen.count(address)) {
      seen[address] = found;
      fprintf(stderr, "table: %08X %ux%u -> level %d (%ux%u at %08X pitch %u) at %u,%u\n", address, width, height, found, m.width, m.height, m.address, m.pitch_blocks, x, y);
    }
  }
  Transition(source.texture.get(), RenderTextureLayout::COPY_SOURCE);
  Transition(table_texture_, RenderTextureLayout::COPY_DEST);
  const RenderBox box(int32_t(source_x), int32_t(source_y), int32_t(source_x + width), int32_t(source_y + height));
  commands_->copyTextureRegion(RenderTextureCopyLocation::Subresource(table_texture_, uint32_t(found), 0),
                               RenderTextureCopyLocation::Subresource(source.texture.get(), 0, 0), x, y, 0, &box);
  Transition(table_texture_, RenderTextureLayout::SHADER_READ);
  Transition(source.texture.get(), RenderTextureLayout::SHADER_READ);
}

// Bytes per texel of the formats a resolve writes, 0 for the rest.
static uint32_t ResolveBytes(uint32_t guestFormat) {
  switch (guestFormat) {
    case 2: return 1;
    case 3: case 24: case 30: return 2;
    case 6: case 7: case 22: case 23: case 25: case 31: case 36: return 4;
    case 26: case 32: return 8;
    default: return 0;
  }
}

void Renderer::Resolve(const ResolveCall& call) {
  const bool depth = call.flags & 4;
  Texture& source = depth ? GetDepthSurface(call.state) : GetColorSurface(call.state);
  // A colour resolve in another format than the one that wrote the tiles (the 16_16 shadow pass read as
  // 8_8_8_8 right after its clearing resolve) reads what the tiles hold: the clear word. The second target's
  // tiles are not tracked (draws only claim the first's), and the g-buffer's normals sit there.
  if (!depth && !call.second_target) {
    Texture& depthSurface = GetDepthSurface(call.state);
    SetTargets(source.texture.get(), nullptr);
    TrackEdram(call.state, source, depthSurface, true, false,
               call.has_rect ? uint32_t(std::max(call.rect[3], 0)) : ViewportBottom(call.state), true, std::nullopt, std::nullopt, true);
  }
  uint32_t address = call.state.Reg(0x2319) & 0xFFFFF000;  // RB_COPY_DEST_BASE
  const uint32_t guestAddress = address;
  const uint32_t destPitch = call.state.Reg(0x231A) & 0x3FFF;      // RB_COPY_DEST_PITCH
  const uint32_t destHeight = (call.state.Reg(0x231A) >> 16) & 0x3FFF;
  int32_t rect[4] = {0, 0, int32_t(destPitch), int32_t(destHeight)};
  if (call.has_rect) std::copy(call.rect, call.rect + 4, rect);
  // The source rect is in surface coordinates; the destination point (or
  // the rect's origin) is where it lands. RB_COPY_DEST_PITCH's height counts
  // from that point (predicated tiling resolves the bands one by one).
  const int32_t x = call.has_dest_point ? call.dest_x : rect[0];
  const int32_t y = call.has_dest_point ? call.dest_y : rect[1];
  int32_t band = 0;
  // A band of an earlier resolve's texture (the shadow atlas is resolved in bands, each at a
  // tile row of the one texture the game fetches at its base): it lands in that texture.
  if (const uint32_t bytes = ResolveBytes((call.state.Reg(0x231B) >> 7) & 0x3F); bytes && destPitch && !resolved_.count(address)) {
    const uint32_t rowBytes = destPitch * bytes;
    for (const auto& [base, parent] : resolved_) {
      const uint64_t parentBytes = uint64_t(parent.width / parent.scale_x) * uint64_t(parent.height / parent.scale) * bytes;
      if (!parent.texture || parent.texel_bytes != bytes || address <= base || address - base >= parentBytes || (address - base) % (32 * rowBytes)) continue;
      if (uint32_t((parent.width / parent.scale_x) + 0.5f) != destPitch) continue;
      band = int32_t((address - base) / rowBytes);
      address = base;
      break;
    }
  }
  // Textures are sampled with normalized coordinates, so the host texture has
  // the size the game fetches it at: the destination pitch, unless that is
  // just the width padded to 32 texels.
  const uint32_t rectWidth = uint32_t(rect[2] - rect[0]);
  const uint32_t width = (rect[0] == 0 && destPitch == ((rectWidth + 31) & ~31u)) ? rectWidth : destPitch;
  const int32_t destY = y + band;  // the row the rect lands on in the texture
  const uint32_t height = uint32_t(destY) + destHeight;
  RenderFormat format = source.format;
  const float scale = source.scale, scaleX = source.scale_x;
  const uint32_t hostWidth = uint32_t(width * scaleX), hostHeight = uint32_t(height * scale);
  const int32_t step = call.source_scale;
  ResolvePushConstants push{source.index, 0, 1.0f, int32_t((rect[0] - x) * step * scaleX),
                            int32_t((rect[1] - destY) * step * scale), source.stencil_index, step};
  if (depth) {
    // Fetched as 24_8_FLOAT: the depth itself in x, unlike Skate's stencil tags in packed bytes.
    format = RenderFormat::R32_FLOAT;
    push.mode = 6;
  } else if (format == RenderFormat::R8G8B8A8_UNORM) {
    const uint32_t copyInfo = call.state.Reg(0x231B);
    if (((copyInfo >> 7) & 0x3F) == 3 && ((copyInfo >> 16) & 0x3F) == 8) {
      push.mode = 5;
      format = RenderFormat::R16G16B16A16_FLOAT;
    } else if (const uint32_t destFormat = (copyInfo >> 7) & 0x3F; destFormat == xenos::kTex_8 || destFormat == xenos::kTex_8_8) {
      // One or two components keep the shader's order: only four-component 8888 comes out blue first.
      push.mode = 0;
    } else {
      push.mode = 3;
    }
  } else if (format == RenderFormat::R16G16_FLOAT || format == RenderFormat::R16G16B16A16_FLOAT) {
    // A resolve converts: the EDRAM value (fixed point, -32..32) is written
    // in the destination format, here the unsigned normalized 16_16 the
    // game fetches it as (shadow depth and mask in 0..1). With an integer
    // number format the biased value is truncated to the integer itself
    // (the occlusion pyramid is read back on the CPU as signed 16_16).
    const uint32_t copyInfo = call.state.Reg(0x231B);
    const uint32_t destFormat = (copyInfo >> 7) & 0x3F, number = (copyInfo >> 13) & 7;
    const bool two = format == RenderFormat::R16G16_FLOAT;
    if (destFormat == (two ? 25u : 26u) && (number == 2 || number == 3)) {
      push.mode = number == 3 ? 8 : 7;
      format = number == 3 ? (two ? RenderFormat::R16G16_SNORM : RenderFormat::R16G16B16A16_SNORM)
                           : (two ? RenderFormat::R16G16_UNORM : RenderFormat::R16G16B16A16_UNORM);
    } else if (two && destFormat != xenos::kTex_16_FLOAT && destFormat != xenos::kTex_16_16_FLOAT &&
               destFormat != xenos::kTex_16_16_16_16_FLOAT) {
      // Half float destinations (the exposure, read on the CPU as k_16_FLOAT) keep the float.
      format = RenderFormat::R16G16_UNORM;
    }
  }
  if (!depth) {
    // RB_COPY_DEST_INFO exponent bias (signed, bits 16..21): the value is multiplied by 2^bias.
    // Mode 5 applies its own.
    const int32_t bias = int32_t(call.state.Reg(0x231B) << 10) >> 26;
    if (push.mode != 5) push.scale = std::ldexp(1.0f, bias);
  }

  Texture& dest = resolved_[address];
  dest.texel_bytes = ResolveBytes((call.state.Reg(0x231B) >> 7) & 0x3F);
  if (!dest.texture || dest.format != format || dest.width != hostWidth || dest.height < hostHeight) {
    ++texture_generation_;
    // Scratch addresses are resolved at several sizes and formats in turn (the post processing chain, the
    // page table's smallest levels): each keeps its texture, put aside while another is current.
    const auto spareKey = [&](uint32_t width, RenderFormat spareFormat) {
      return uint64_t(address) << 32 | uint64_t(width) << 8 | uint64_t(spareFormat);
    };
    if (dest.texture && resolved_spares_.size() < 64) {
      auto old = resolved_spares_.find(spareKey(dest.width, dest.format));
      if (old != resolved_spares_.end()) {
        RetireResolved(old->second);
        RetireResolvedSlots(old->second);
        resolved_spares_.erase(old);
      }
      resolved_spares_[spareKey(dest.width, dest.format)] = std::move(dest);
      dest = Texture{};
      dest.texel_bytes = ResolveBytes((call.state.Reg(0x231B) >> 7) & 0x3F);
    }
    if (const auto spare = resolved_spares_.find(spareKey(hostWidth, format));
        !dest.texture && spare != resolved_spares_.end() && spare->second.height >= hostHeight) {
      dest = std::move(spare->second);
      dest.texel_bytes = ResolveBytes((call.state.Reg(0x231B) >> 7) & 0x3F);
      dest.watch_stamp = 0;
      resolved_spares_.erase(spare);
    }
  }
  if (!dest.texture || dest.format != format || dest.width != hostWidth || dest.height < hostHeight) {
    if (dest.texture) {
      // The draws recorded so far still sample the old texture through its slots: the new one gets its own.
      RetireResolved(dest);
      RetireResolvedSlots(dest);
    }
    dest.texture = device_->createTexture(RenderTextureDesc::ColorTarget(hostWidth, hostHeight, format));
    dest.format = format;
    dest.width = hostWidth;
    dest.height = hostHeight;
    RenderTextureViewDesc view;
    view.format = format;
    view.dimension = RenderTextureViewDimension::TEXTURE_2D;
    view.mipLevels = 1;
    dest.view = dest.texture->createTextureView(view);
    if (!dest.index) dest.index = AllocateSlot(1);
    texture_2d_set_->setTexture(dest.index, dest.texture.get(), RenderTextureLayout::SHADER_READ,
                                dest.view.get());
  }

  // Draw the source into the destination with the resolve shader.
  Transition(source.texture.get(), RenderTextureLayout::SHADER_READ);
  SetTargets(dest.texture.get(), nullptr);
  const uint64_t key = uint64_t(format) | 1ull << 62;
  auto& pipeline = pipelines_[key];
  if (!pipeline) {
    RenderGraphicsPipelineDesc desc;
    desc.pipelineLayout = resolve_layout_.get();
    desc.vertexShader = resolve_vs_.get();
    desc.pixelShader = resolve_ps_.get();
    desc.renderTargetFormat[0] = format;
    desc.renderTargetBlend[0] = RenderBlendDesc::Copy();
    desc.renderTargetCount = 1;
    pipeline = device_->createGraphicsPipeline(desc);
  }
  commands_->setGraphicsPipelineLayout(resolve_layout_.get());
  commands_->setGraphicsDescriptorSet(texture_2d_set_.get(), 0);
  main_layout_bound_ = false;
  commands_->setPipeline(pipeline.get());
  commands_->setGraphicsPushConstants(0, &push);
  dest.scale = scale;
  dest.scale_x = scaleX;
  RenderViewport viewport(float(x) * scaleX, float(destY) * scale, float(rect[2] - rect[0]) * scaleX,
                          float(rect[3] - rect[1]) * scale);
  RenderRect scissor(int32_t(x * scaleX), int32_t(destY * scale), int32_t((x + rect[2] - rect[0]) * scaleX),
                     int32_t((destY + rect[3] - rect[1]) * scale));
  // The full-screen triangle covers the viewport; the shader reads by pixel.
  viewport = RenderViewport(0.0f, 0.0f, float(dest.width), float(dest.height));
  commands_->setViewports(&viewport, 1);
  commands_->setScissors(&scissor, 1);
  commands_->drawInstanced(3, 1, 0, 0);
  Transition(dest.texture.get(), RenderTextureLayout::SHADER_READ);
  if (dest.layers) RefreshVolume(dest);
  ++stats_[depth ? "resolves (depth)" : "resolves (color)"];
  if (call.trace_index) {
    TraceDump dump;
    dump.index = call.trace_index;
    dump.address = address;
    dump.width = dest.width;
    dump.height = dest.height;
    dump.format = uint32_t(dest.format);
    dump.bytes = RenderFormatSize(dest.format);
    dump.buffer = device_->createBuffer(RenderBufferDesc::ReadbackBuffer(uint64_t(dest.width) * dest.height * dump.bytes));
    Transition(dest.texture.get(), RenderTextureLayout::COPY_SOURCE);
    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {dest.width, dest.height, 1};
    vkCmdCopyImageToBuffer(static_cast<VulkanCommandList*>(commands_)->vk, static_cast<VulkanTexture*>(dest.texture.get())->vk,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, static_cast<VulkanBuffer*>(dump.buffer.get())->vk, 1, &region);
    Transition(dest.texture.get(), RenderTextureLayout::SHADER_READ);
    trace_dumps_.push_back(std::move(dump));
  }
  // The game reads small resolves on the CPU: exposure, the page requests of
  // the virtual textures.
  {
    const uint32_t info = call.state.Reg(0x231B);
    const uint32_t guestFormat = (info >> 7) & 0x3F;
    if (guestFormat == 3 && !depth) {
      // The first writes come before any draw has read the table.
      for (uint32_t slot = 0; slot < 32 && !table_texture_; ++slot) {
        const xenos::TextureFetch t = xenos::DecodeTextureFetch(call.state, slot);
        if (t.valid && t.format == xenos::kTex_1_5_5_5 && t.width == 2048 && t.mip_address) GetGuestTexture(t);
      }
      if (table_texture_) {
        CopyResolveToTable(dest, guestAddress, uint32_t(x), uint32_t(destY), uint32_t(rect[2] - rect[0]),
                           uint32_t(rect[3] - rect[1]));
      } else {
        ++stats_["table: resolve before the table exists"];
      }
    }
    const uint32_t guestBytes = ResolveBytes(guestFormat);
    // Only what this resolve wrote: tables are updated a few pages at a time.
    const uint32_t rectWidth2 = uint32_t(std::max(rect[2] - rect[0], 0));
    const uint32_t rectHeight2 = uint32_t(std::max(rect[3] - rect[1], 0));
    if (guestBytes && rectWidth2 && rectHeight2 &&
        uint64_t(rectWidth2) * rectHeight2 * guestBytes <= 256 * 1024) {
      ReadBackTarget target{guestBytes, destPitch, info & 7};
      target.x = uint32_t(x);
      target.y = uint32_t(destY);
      target.width = std::min(rectWidth2, dest.width - std::min(uint32_t(x), dest.width));
      target.height = std::min(rectHeight2, dest.height - std::min(uint32_t(destY), dest.height));
      target.address = address;
      target.format = guestFormat;
      target.swap_red_blue = (info >> 24) & 1;
      read_back_.push_back(target);
    }
  }

  // Clears that ride on the resolve, limited to the resolved rect (with
  // predicated tiling that is the tile; the other bands are still to resolve).
  if (call.flags & 0x300) {
    ClearCall clear;
    clear.state = call.state;
    clear.has_rect = true;
    std::copy(rect, rect + 4, clear.rect);
    clear.flags = ((call.flags & 0x100) ? 0x1 : 0) | ((call.flags & 0x200) ? 0x10 : 0);
    const uint32_t color = call.state.Reg(0x231E);  // RB_COLOR_CLEAR (8_8_8_8)
    for (int c = 0; c < 4; ++c) clear.color[c] = float((color >> (8 * c)) & 0xFF) / 255.0f;
    clear.depth = call.clear_depth;
    clear.flags |= (call.flags & 0x200) ? 0x20 : 0;
    Clear(clear);
  }
}

void Renderer::Draw(const DrawCall& call) {
  const xenos::DeviceState& state = call.state;
  // Kept up with every draw, also those skipped below: what changed since the last upload or binding.
  const auto addDirty = [](ConstantUpload& last, uint32_t begin, uint32_t end) {
    if (begin >= end) return;
    if (last.dirty_begin >= last.dirty_end) {
      last.dirty_begin = begin;
      last.dirty_end = end;
    } else {
      last.dirty_begin = std::min(last.dirty_begin, begin);
      last.dirty_end = std::max(last.dirty_end, end);
    }
  };
  addDirty(vs_constants_, call.vs_dirty_begin, call.vs_dirty_end);
  addDirty(ps_constants_, call.ps_dirty_begin, call.ps_dirty_end);
  fetch_dirty_ |= call.fetch_changed;
  RenderPrimitiveTopology topology;
  bool quads = false, rects = false;
  switch (call.primitive) {
    case 1: topology = RenderPrimitiveTopology::POINT_LIST; break;
    case 2: topology = RenderPrimitiveTopology::LINE_LIST; break;
    case 3: topology = RenderPrimitiveTopology::LINE_STRIP; break;
    case 4: topology = RenderPrimitiveTopology::TRIANGLE_LIST; break;
    case 6: topology = RenderPrimitiveTopology::TRIANGLE_STRIP; break;
    case 8:  // rectangle list: three corners each, the fourth worked out below
      topology = RenderPrimitiveTopology::TRIANGLE_LIST;
      rects = true;
      break;
    case 13:  // quad list: expanded to triangles below
      topology = RenderPrimitiveTopology::TRIANGLE_LIST;
      quads = true;
      break;
    default:
      ++stats_["draw skipped: primitive " + std::to_string(call.primitive)];
      return;
  }
  Shader* vs = GetShader(call.vs_key);
  Shader* ps = GetShader(call.ps_key);
  if (!vs || !ps) {
    ++stats_["draw skipped: shader not in cache"];
    return;
  }

  for (uint32_t i = 0; i < call.export_count; ++i) AddExportRange(call.exports[i].address, call.exports[i].size);
  if (call.export_count) ++stats_["memory export draws"];
  // Memory export draws write the buffer the draws before them wrote and read.
  if (call.export_count) ExportBarrier();

  // Vertex input from the patched vfetch instructions, decoded once for each
  // shader and set of fetches.
  // 16-bit elements per slot (byte offset, dwords): Xenos keeps the first
  // component in the high half of each dword, so after the 32-bit swap their
  // halves are put back in order.
  // (VertexLayout::half_swaps; narrowed holds the 10_11_11 elements fed to float
  // inputs per slot, byte offset and signed, packed the 8_8_8_8 normals.)
  uint32_t lastStride = 0, lastSlot = 0;
  // What the GPU loads is the object's physical part as D3D patched it.
  static const bool traceSkips = getenv("NATIVE_GPU_TRACE_SKIPS") != nullptr;  // debugging
  const auto traceSkip = [&](const char* why) {
    if (traceSkips) {
      fprintf(stderr, "SKIP %s vs %016llX ps %016llX prim %u count %u\n", why, (unsigned long long)call.vs_key,
              (unsigned long long)call.ps_key, call.primitive, call.count);
    }
  };
  if (!call.vs_physical) {
    ++stats_["draw skipped: no vertex microcode"];
    return;
  }
  const uint8_t* microcode = call.vs_physical + vs->microcode_offset;
  const size_t microcodeSize = call.vs_physical_size - std::min<size_t>(call.vs_physical_size, vs->microcode_offset);
  size_t fetchesEnd = 0;
  for (const Shader::Element& element : vs->elements) fetchesEnd = std::max<size_t>(fetchesEnd, (element.address + 1) * 12);
  const uint64_t layoutKey = XXH3_64bits_withSeed(microcode, std::min(fetchesEnd, microcodeSize), call.vs_key);
  VertexLayout& vertexLayout = vertex_layouts_[layoutKey];
  const bool freshLayout = !vertexLayout.valid;
  if (freshLayout) vertexLayout = VertexLayout();
  std::vector<RenderInputElement>& inputElements = vertexLayout.elements;
  std::vector<RenderInputSlot>& inputSlots = vertexLayout.slots;
  std::vector<uint32_t>& slotFetch = vertexLayout.slot_fetch;
  auto& halfSwaps = vertexLayout.half_swaps;
  auto& narrowed = vertexLayout.narrowed;
  auto& packed = vertexLayout.packed;
  bool& packedNormals = vertexLayout.packed_normals;
  bool& halfNormals = vertexLayout.half_normals;
  if (freshLayout) for (const Shader::Element& element : vs->elements) {
    // D3D relinks the vertex fetches it loads: registers may move between
    // fetch instructions, and swizzles change (x and y swapped for its 16-bit
    // pairs). Use the fetch writing the element's original destination register
    // and components, among the elements' fetch slots.
    const auto written = [](uint32_t swizzle) {
      uint32_t mask = 0;
      for (uint32_t c = 0; c < 4; ++c) mask |= uint32_t(((swizzle >> (3 * c)) & 7) != 7) << c;
      return mask;
    };
    size_t at = element.address * 12;
    for (const Shader::Element& other : vs->elements) {
      const size_t candidate = other.address * 12;
      if (candidate + 12 <= microcodeSize &&
          ((xenos::LoadBE32(microcode + candidate) >> 12) & 0x3F) == element.dst_register &&
          written(xenos::LoadBE32(microcode + candidate + 4) & 0xFFF) == written(element.dst_swizzle) &&
          (xenos::LoadBE32(microcode + candidate) & 0x1F) == 0) {
        at = candidate;
        break;
      }
    }
    if (at + 12 > microcodeSize) {
      ++stats_["draw skipped: vertex fetch outside microcode"];
      return;
    }
    xenos::VertexFetchInstruction fetch = xenos::DecodeVertexFetchInstruction(microcode + at);
    if (!fetch.is_vfetch) {
      ++stats_["draw skipped: vertex element is not a vfetch"];
      traceSkip("notvfetch");
      return;
    }
    if (fetch.is_mini) {
      // A mini fetch takes the stride and slot of the full fetch before it
      // in the microcode.
      fetch.stride = lastStride;
      fetch.fetch_slot = lastSlot;
      for (size_t back = at; back >= 12;) {
        back -= 12;
        const xenos::VertexFetchInstruction full = xenos::DecodeVertexFetchInstruction(microcode + back);
        if (full.is_vfetch && !full.is_mini) {
          fetch.stride = full.stride;
          fetch.fetch_slot = full.fetch_slot;
          break;
        }
      }
    } else {
      lastStride = fetch.stride;
      lastSlot = fetch.fetch_slot;
    }
    if (fetch.exp_adjust) {
      ++stats_["vertex exp_adjust " + std::to_string(fetch.exp_adjust) + ": usage " +
               std::to_string(element.usage) + "." + std::to_string(element.usage_index) + " format " +
               std::to_string(fetch.format)];
    }
    // A fetch of constants only (0 and 1 in its swizzle, no vertex data): vertices
    // without this attribute, such as the wires' normals. Left unbound, it reads 0.
    bool fetchesData = false;
    for (uint32_t c = 0; c < 4; ++c) {
      fetchesData |= ((xenos::LoadBE32(microcode + at + 4) >> (3 * c)) & 7) < 4;
    }
    if (!fetchesData) continue;
    const bool raw = IsRawUsage(element.usage);
    const RenderFormat format = raw ? RawVertexFormat(fetch.format) : VertexFormat(fetch, element.usage);
    if (raw && (fetch.format == xenos::k_10_11_11 || fetch.format == xenos::k_8_8_8_8)) packedNormals = true;
    if (raw && (fetch.format == xenos::k_16_16_FLOAT || fetch.format == xenos::k_16_16_16_16_FLOAT)) {
      halfNormals = true;
    }
    const int32_t location = UsageLocation(element.usage, element.usage_index);
    if (format == RenderFormat::UNKNOWN || location < 0) {
      ++stats_[(location < 0 ? "vertex element without a location: usage " : "vertex element format: usage ") +
               std::to_string(element.usage) + "." + std::to_string(element.usage_index) + " format " +
               std::to_string(fetch.format) + (fetch.is_signed ? " signed" : "") +
               (fetch.is_integer ? " integer" : "")];
      continue;
    }
    uint32_t slot = 0;
    while (slot < slotFetch.size() && slotFetch[slot] != fetch.fetch_slot) ++slot;
    if (slot == slotFetch.size()) {
      slotFetch.push_back(fetch.fetch_slot);
      inputSlots.emplace_back(slot, fetch.stride * 4);
      halfSwaps.emplace_back();
      narrowed.emplace_back();
      packed.emplace_back();
    }
    if (raw && fetch.format == xenos::k_8_8_8_8) {
      packed[slot].emplace_back(uint32_t(fetch.offset * 4), uint32_t(fetch.is_signed));
    }
    if (fetch.format == xenos::k_10_11_11 && !raw) {
      narrowed[slot].emplace_back(uint32_t(fetch.offset * 4), uint32_t(fetch.is_signed));
    }
    // Swapped only where D3D patched the fetch: the container's swizzle then reads the pairs in natural
    // order. One that already holds the patched swizzle (yxw) reads them as the 32-bit swap leaves them.
    const bool natural = (xenos::LoadBE32(microcode + at + 4) & 0xFFF) != element.dst_swizzle;
    if (natural && (fetch.format == xenos::k_16_16 || fetch.format == xenos::k_16_16_FLOAT)) {
      halfSwaps[slot].emplace_back(uint32_t(fetch.offset * 4), 1);
    } else if (natural && (fetch.format == xenos::k_16_16_16_16 || fetch.format == xenos::k_16_16_16_16_FLOAT)) {
      halfSwaps[slot].emplace_back(uint32_t(fetch.offset * 4), 2);
    }
    inputElements.emplace_back("", 0, uint32_t(location), format, slot, uint32_t(fetch.offset * 4));
  }
  vertexLayout.valid = true;
  vertexLayout.slot_layouts.resize(slotFetch.size());
  std::vector<RenderVertexBufferView>& vertexViews = vertex_views_;
  vertexViews.clear();
  // Rectangle lists: the vertex of each corner, in draw order, whether the draw is indexed, inline or neither.
  // Each rectangle is expanded on the CPU to two triangles below.
  uint32_t rectCount = rects ? call.count / 3 : 0;
  std::vector<uint32_t> rectVertices;
  if (rects && call.indices_size) {
    std::vector<uint8_t> guest(call.indices_size);
    std::memcpy(guest.data(), physical_ + (call.indices_address & 0x1FFFFFFF), guest.size());
    xenos::SwapEndian(guest.data(), guest.size(), call.indices_endian);
    const uint32_t bytes = call.indices_32bit ? 4 : 2;
    const uint32_t available = uint32_t(guest.size() / bytes);
    rectCount = call.start >= available ? 0 : std::min(rectCount, (available - call.start) / 3);
    for (uint32_t i = 0; i < rectCount * 3; ++i) {
      uint32_t index = 0;
      std::memcpy(&index, &guest[size_t(call.start + i) * bytes], bytes);
      rectVertices.push_back(index + call.base_vertex);
    }
  } else if (rects) {
    for (uint32_t i = 0; i < rectCount * 3; ++i) rectVertices.push_back((call.inline_vertices ? 0 : call.start) + i);
  }
  if (rects && !rectCount) {
    ++stats_["draw skipped: empty rectangle list"];
    return;
  }
  for (uint32_t slot = 0; slot < slotFetch.size(); ++slot) {
    const xenos::VertexFetch fetch = xenos::DecodeVertexFetch(state, slotFetch[slot]);
    const uint32_t stride = inputSlots[slot].stride;
    auto convert = [&](uint8_t* data, uint32_t size, uint32_t endian) {
      xenos::SwapEndian(data, size, endian);
      for (const auto& [offset, is_signed] : narrowed[slot]) {
        for (uint32_t vertex = 0; stride && (vertex + 1) * stride <= size; ++vertex) {
          uint8_t* p = data + vertex * stride + offset;
          uint32_t v;
          std::memcpy(&v, p, 4);
          // x and y are 11 bits, z 10, in that order from the low bit.
          p[0] = uint8_t((v >> 3) & 0xFF);
          p[1] = uint8_t((v >> 14) & 0xFF);
          p[2] = uint8_t(v >> 24);
          p[3] = is_signed ? 127 : 255;
        }
      }
      // 8_8_8_8 normals, in the 10_11_11 layout the shaders' normal fetch decodes.
      for (const auto& [offset, is_signed] : packed[slot]) {
        const auto scaled = [is_signed = is_signed](uint32_t byte, int32_t one, uint32_t mask) {
          const float value = is_signed ? std::max(float(int8_t(byte)) / 127.0f, -1.0f) : float(byte) / 255.0f;
          return uint32_t(std::clamp(int32_t(std::lround(value * float(one))), -one, one - 1)) & mask;
        };
        for (uint32_t vertex = 0; stride && (vertex + 1) * stride <= size; ++vertex) {
          uint8_t* p = data + vertex * stride + offset;
          const uint32_t v = scaled(p[0], 1024, 0x7FF) | scaled(p[1], 1024, 0x7FF) << 11 | scaled(p[2], 512, 0x3FF) << 22;
          std::memcpy(p, &v, 4);
        }
      }
      for (const auto& [offset, dwords] : halfSwaps[slot]) {
        for (uint32_t vertex = 0; stride && (vertex + 1) * stride <= size; ++vertex) {
          uint8_t* p = data + vertex * stride + offset;
          for (uint32_t d = 0; d < dwords; ++d, p += 4) {
            std::swap(p[0], p[2]);
            std::swap(p[1], p[3]);
          }
        }
      }
    };
    if (call.inline_vertices && !rects) {
      // Inline vertices live in the command buffer: converted every time.
      convert_buffer_.assign(call.inline_vertices, call.inline_vertices + call.inline_size);
      convert(convert_buffer_.data(), call.inline_size, 2);
      const uint64_t at = Allocate(convert_buffer_.data(), call.inline_size, 16);
      vertexViews.push_back({upload_->at(at), call.inline_size});
      continue;
    }
    // Guest vertex buffers are converted once and kept until written to.
    if (freshLayout) {
      uint64_t layout = XXH3_64bits_withSeed(halfSwaps[slot].data(),
                                             halfSwaps[slot].size() * sizeof(halfSwaps[slot][0]), stride);
      layout ^= XXH3_64bits(narrowed[slot].data(), narrowed[slot].size() * sizeof(narrowed[slot][0]));
      layout ^= XXH3_64bits_withSeed(packed[slot].data(), packed[slot].size() * sizeof(packed[slot][0]), 1);
      vertexLayout.slot_layouts[slot] = layout;
    }
    const uint32_t address = fetch.address & 0x1FFFFFFF;
    if (rects && stride) {
      // Each rectangle becomes two triangles. The fourth corner is the sum of the two ends
      // of the diagonal, the longest side, less the third, in every float element.
      const uint32_t size = rectCount * 3 * stride;
      convert_buffer_.assign(size, 0);
      for (uint32_t i = 0; i < rectCount * 3; ++i) {
        const uint64_t at = uint64_t(rectVertices[i]) * stride;
        if (call.inline_vertices) {
          if (at + stride <= call.inline_size) std::memcpy(&convert_buffer_[size_t(i) * stride], call.inline_vertices + at, stride);
        } else if (at + stride <= fetch.size) {
          std::memcpy(&convert_buffer_[size_t(i) * stride], physical_ + address + at, stride);
        }
      }
      convert(convert_buffer_.data(), size, call.inline_vertices ? 2 : fetch.endian);
      std::vector<uint8_t> expanded(size_t(rectCount) * 6 * stride);
      for (uint32_t r = 0; r < rectCount; ++r) {
        const uint8_t* v = convert_buffer_.data() + size_t(r) * 3 * stride;
        const auto corner = [&](uint32_t i) {
          float xy[2];
          std::memcpy(xy, v + i * stride, 8);
          return std::array<float, 2>{xy[0], xy[1]};
        };
        const auto distance = [&](uint32_t a, uint32_t b) {
          const auto p = corner(a), q = corner(b);
          return (p[0] - q[0]) * (p[0] - q[0]) + (p[1] - q[1]) * (p[1] - q[1]);
        };
        const float d01 = distance(0, 1), d12 = distance(1, 2), d02 = distance(0, 2);
        uint32_t a = 0, b = 2, c = 1;
        if (d01 >= d12 && d01 >= d02) {
          a = 0, b = 1, c = 2;
        } else if (d12 >= d01 && d12 >= d02) {
          a = 1, b = 2, c = 0;
        }
        uint8_t* out = expanded.data() + size_t(r) * 6 * stride;
        const uint32_t order[5] = {a, c, b, a, b};
        for (uint32_t i = 0; i < 5; ++i) std::memcpy(out + i * stride, v + order[i] * stride, stride);
        uint8_t* fourth = out + 5 * stride;
        std::memcpy(fourth, v + a * stride, stride);
        for (const RenderInputElement& element : inputElements) {
          if (element.slotIndex != slot) continue;
          uint32_t components = 0;
          switch (element.format) {
            case RenderFormat::R32_FLOAT: components = 1; break;
            case RenderFormat::R32G32_FLOAT: components = 2; break;
            case RenderFormat::R32G32B32_FLOAT: components = 3; break;
            case RenderFormat::R32G32B32A32_FLOAT: components = 4; break;
            default: break;
          }
          for (uint32_t k = 0; k < components; ++k) {
            const uint32_t at = element.alignedByteOffset + 4 * k;
            float x, y, z;
            std::memcpy(&x, v + a * stride + at, 4);
            std::memcpy(&y, v + b * stride + at, 4);
            std::memcpy(&z, v + c * stride + at, 4);
            const float w = x + y - z;
            std::memcpy(fourth + at, &w, 4);
          }
        }
      }
      const uint64_t at = Allocate(expanded.data(), expanded.size(), 16);
      vertexViews.push_back({upload_->at(at), uint32_t(expanded.size())});
      continue;
    }
    const auto fill = [&](uint8_t* data) { convert(data, fetch.size, fetch.endian); };
    const RenderBuffer* buffer =
        GetGuestBuffer(address, fetch.size, vertexLayout.slot_layouts[slot] ^ fetch.endian, std::ref(fill));
    vertexViews.push_back({buffer, fetch.size});
  }

  // Textures (resolved ones by address first) and samplers.
  SharedConstants shared;
  // The same fetch constants as the last draw's (none of the draws since changed them), with no texture
  // changed since, bind the same.
  TextureBindings& last = last_textures_;
  if (!fetch_dirty_ && last.generation == texture_generation_) {
    std::memcpy(shared.texture2DIndices, last.indices[0], sizeof(shared.texture2DIndices));
    std::memcpy(shared.texture3DIndices, last.indices[1], sizeof(shared.texture3DIndices));
    std::memcpy(shared.textureCubeIndices, last.indices[2], sizeof(shared.textureCubeIndices));
    std::memcpy(shared.samplerIndices, last.indices[3], sizeof(shared.samplerIndices));
    std::memcpy(shared.texExpScale, last.exp_scale, sizeof(shared.texExpScale));
    std::memcpy(shared.texBias, last.bias, sizeof(shared.texBias));
  } else {
    const uint32_t generation = texture_generation_;
    for (uint32_t slot = 0; slot < 16; ++slot) {
      // A fetch constant already seen, with no texture changed since.
      const uint8_t* fetchConstant = state.data + xenos::device::kFetchConstants + 24 * slot;
      uint32_t words[3];
      std::memcpy(words, fetchConstant, 12);
      SlotBinding& binding = bindings_[((words[0] * 2654435761u) ^ (words[1] * 40503u) ^ words[2]) >> 22];
      if (binding.generation == texture_generation_ && !std::memcmp(binding.fetch, fetchConstant, 24)) {
        shared.texExpScale[slot] = binding.exp_scale;
        shared.texBias[slot >> 3] |= binding.bias << ((slot & 7) * 4);
        shared.texture2DIndices[slot] = binding.texture_2d;
        shared.texture3DIndices[slot] = binding.texture_3d;
        shared.textureCubeIndices[slot] = binding.texture_cube;
        shared.samplerIndices[slot] = binding.sampler;
        continue;
      }
      binding = SlotBinding{};
      std::memcpy(binding.fetch, fetchConstant, 24);
      binding.generation = texture_generation_;
      binding.exp_scale = shared.texExpScale[slot] = TextureExpScale(state, slot);
      binding.bias = TextureBiasBits(state, slot);
      shared.texBias[slot >> 3] |= binding.bias << ((slot & 7) * 4);
      const xenos::TextureFetch t = xenos::DecodeTextureFetch(state, slot);
      if (!t.valid) continue;  // the binding stays empty: the same fetch constant is not decoded again
      // A block resolved at the table's base address is part of the table, not a texture.
      const bool table = t.format == xenos::kTex_1_5_5_5 && t.width == 2048 && t.mip_address;
      auto resolved = table ? resolved_.end() : resolved_.find(t.address);
      if (resolved != resolved_.end() && watcher_ &&
          watcher_->Dirty(t.address & 0x1FFFFFFF, resolved->second.resolved_size,
                          resolved->second.watch_stamp)) {
        // Written by the game since it was resolved: the guest texture now.
        RetireResolved(resolved->second);
        RetireResolvedSlots(resolved->second);
        RetireSpares(t.address & 0xFFFFF000);
        resolved_.erase(resolved);
        resolved = resolved_.end();
        ++texture_generation_;
        ++stats_["resolved textures overwritten by the game"];
      }
      if (resolved != resolved_.end()) {
        shared.texture2DIndices[slot] = ResolvedIndex(resolved->second, t.swizzle);
        // A stack of 2D layers (size field bits 26+) that the shader fetches as a volume.
        const uint32_t layers = (state.U32(xenos::device::kFetchConstants + 24 * slot + 8) >> 26) + 1;
        Texture& stack = resolved->second;
        if (t.dimension == 1 && layers > 1 && stack.width % layers == 0) {
          if (stack.layers != layers || !stack.volume) {
            stack.layers = layers;
            RetireVolume(stack);
            RefreshVolume(stack);
          }
          shared.texture3DIndices[slot] = stack.volume_index;
        }
      } else if (Texture* texture = GetGuestTexture(t)) {
        (texture->dimension == 3   ? shared.textureCubeIndices
         : texture->dimension == 2 ? shared.texture3DIndices
                                   : shared.texture2DIndices)[slot] = texture->index;
      }
      shared.samplerIndices[slot] = GetSampler(t);
      binding.generation = texture_generation_;  // possibly bumped above
      binding.texture_2d = shared.texture2DIndices[slot];
      binding.texture_3d = shared.texture3DIndices[slot];
      binding.texture_cube = shared.textureCubeIndices[slot];
      binding.sampler = shared.samplerIndices[slot];
    }
    if (generation == texture_generation_) {
      std::memcpy(last.indices[0], shared.texture2DIndices, sizeof(last.indices[0]));
      std::memcpy(last.indices[1], shared.texture3DIndices, sizeof(last.indices[1]));
      std::memcpy(last.indices[2], shared.textureCubeIndices, sizeof(last.indices[2]));
      std::memcpy(last.indices[3], shared.samplerIndices, sizeof(last.indices[3]));
      std::memcpy(last.exp_scale, shared.texExpScale, sizeof(last.exp_scale));
      std::memcpy(last.bias, shared.texBias, sizeof(last.bias));
      last.generation = generation;
      fetch_dirty_ = false;
    } else {
      last.generation = 0;
    }
  }

  // Targets.
  Texture& color = GetColorSurface(state);
  Texture& depth = GetDepthSurface(state);
  const uint32_t colorMaskAll = state.Reg(xenos::reg::RB_COLOR_MASK);
  Texture* color1 = (colorMaskAll >> 4) & 0xF ? &GetColor1Surface(state) : nullptr;
  // Skate never writes two targets, so the last clear's surface cache stays valid.
  SetTargets(color.texture.get(), depth.texture.get(), color1 ? color1->texture.get() : nullptr);
  SetMainLayout();

  // Pipeline. Draws that agree on what it is built from (shaders, vertex layout, targets and the registers
  // below) share one: it is found from those values, and only a new combination builds its description.
  const uint32_t depthControl = state.Reg(xenos::reg::RB_DEPTHCONTROL);
  const uint32_t blendControl = state.Reg(xenos::reg::RB_BLENDCONTROL0);
  const uint32_t colorControl = state.Reg(xenos::reg::RB_COLORCONTROL);
  const bool depthEnabled = (depthControl >> 1) & 1, depthWriteEnabled = (depthControl >> 2) & 1;
  const bool stencilEnabled = depthControl & 1;
  const uint32_t stencilRefMask = stencilEnabled ? state.Reg(0x210D) : 0;
  const uint32_t colorWriteMask = colorMaskAll & 0xF;
  const uint32_t inputs[] = {uint32_t(call.vs_key),          uint32_t(call.vs_key >> 32),
                             uint32_t(call.ps_key),          uint32_t(call.ps_key >> 32),
                             uint32_t(layoutKey),            uint32_t(layoutKey >> 32),
                             depthControl,                   blendControl,
                             (colorControl >> 3) & 1,        stencilRefMask,
                             state.Reg(xenos::reg::PA_SU_SC_MODE_CNTL) & 7,
                             uint32_t(color.format),         uint32_t(depth.format),
                             color1 ? uint32_t(color1->format) : 0, colorMaskAll & 0xFF,
                             uint32_t(topology) | rects << 8};
  const uint64_t inputsKey = XXH3_64bits(inputs, sizeof(inputs));
  RenderPipeline* pipeline = nullptr;
  if (const auto found = pipeline_inputs_.find(inputsKey); found != pipeline_inputs_.end()) pipeline = found->second;
  if (!pipeline) {
    RenderGraphicsPipelineDesc desc;
    desc.pipelineLayout = layout_.get();
    desc.vertexShader = vs->shader.get();
    desc.pixelShader = ps->shader.get();
    desc.depthEnabled = depthEnabled;
    desc.depthWriteEnabled = depthWriteEnabled;
    desc.depthFunction = RenderComparisonFunction(((depthControl >> 4) & 7) + 1);
    // Stencil (RB_DEPTHCONTROL bit 0; front func/fail/zpass/zfail at 8/11/14/17,
    // back at 20/23/26/29 when bit 7; RB_STENCILREFMASK ref/mask/writemask).
    desc.stencilEnabled = stencilEnabled;
    if (desc.stencilEnabled) {
      const uint32_t refMask = stencilRefMask;
      desc.stencilReference = refMask & 0xFF;
      desc.stencilReadMask = (refMask >> 8) & 0xFF;
      desc.stencilWriteMask = (refMask >> 16) & 0xFF;
      desc.stencilFrontFace = StencilFace((depthControl >> 8) & 7, (depthControl >> 11) & 7,
                                          (depthControl >> 14) & 7, (depthControl >> 17) & 7);
      desc.stencilBackFace = (depthControl >> 7) & 1
                                 ? StencilFace((depthControl >> 20) & 7, (depthControl >> 23) & 7,
                                               (depthControl >> 26) & 7, (depthControl >> 29) & 7)
                                 : desc.stencilFrontFace;
      // The host's front faces are the clockwise ones, the console's are the counter-clockwise ones when
      // PA_SU_SC_MODE_CNTL bit 2 is clear.
      if (!(state.Reg(xenos::reg::PA_SU_SC_MODE_CNTL) & 4)) std::swap(desc.stencilFrontFace, desc.stencilBackFace);
    }
    desc.depthClipEnabled = true;
    desc.dynamicDepthBiasEnabled = true;
    desc.primitiveTopology = topology;
    // PA_SU_SC_MODE_CNTL: cull front, cull back, and the front face (clockwise when
    // set), all seen through the flipped viewport plume is always clockwise for.
    const uint32_t cullBits = state.Reg(xenos::reg::PA_SU_SC_MODE_CNTL) & 3;
    const bool frontClockwise = state.Reg(xenos::reg::PA_SU_SC_MODE_CNTL) & 4;
    // Rectangle lists are not culled: the corners come in any winding.
    if (cullBits == 0 || cullBits == 3 || rects) {
      desc.cullMode = RenderCullMode::NONE;
    } else {
      desc.cullMode = (cullBits == 1) == frontClockwise ? RenderCullMode::FRONT : RenderCullMode::BACK;
    }
    desc.renderTargetFormat[0] = color.format;
    desc.renderTargetCount = 1;
    if (color1) {
      desc.renderTargetFormat[1] = color1->format;
      desc.renderTargetCount = 2;
      desc.renderTargetBlend[1] = RenderBlendDesc::Copy();
      desc.renderTargetBlend[1].renderTargetWriteMask = uint8_t((colorMaskAll >> 4) & 0xF);
    }
    desc.depthTargetFormat = depth.format;
    RenderBlendDesc& blend = desc.renderTargetBlend[0];
    blend.srcBlend = BlendFactor(blendControl & 0x1F);
    blend.blendOp = BlendOp((blendControl >> 5) & 7);
    blend.dstBlend = BlendFactor((blendControl >> 8) & 0x1F);
    blend.srcBlendAlpha = BlendFactor((blendControl >> 16) & 0x1F);
    blend.blendOpAlpha = BlendOp((blendControl >> 21) & 7);
    blend.dstBlendAlpha = BlendFactor((blendControl >> 24) & 0x1F);
    blend.blendEnabled = !(blend.srcBlend == RenderBlend::ONE && blend.dstBlend == RenderBlend::ZERO &&
                           blend.blendOp == RenderBlendOperation::ADD &&
                           blend.srcBlendAlpha == RenderBlend::ONE && blend.dstBlendAlpha == RenderBlend::ZERO &&
                           blend.blendOpAlpha == RenderBlendOperation::ADD);
    blend.renderTargetWriteMask = uint8_t(colorWriteMask);
    uint32_t specConstants = packedNormals ? 1 : 0;  // SPEC_CONSTANT_R11G11B10_NORMAL
    if (halfNormals) specConstants |= 1 << 5;          // SPEC_CONSTANT_HALF_NORMAL
    if ((colorControl >> 3) & 1) specConstants |= 2;  // SPEC_CONSTANT_ALPHA_TEST
    RenderSpecConstant spec(0, specConstants);
    desc.specConstants = &spec;
    desc.specConstantsCount = 1;
    desc.inputSlots = inputSlots.data();
    desc.inputSlotsCount = uint32_t(inputSlots.size());
    desc.inputElements = inputElements.data();
    desc.inputElementsCount = uint32_t(inputElements.size());
    uint64_t key = XXH3_64bits(&desc, offsetof(RenderGraphicsPipelineDesc, inputSlots));
    key = XXH3_64bits_withSeed(inputSlots.data(), inputSlots.size() * sizeof(RenderInputSlot), key);
    for (const RenderInputElement& element : inputElements) {
      const uint32_t fields[] = {element.location, uint32_t(element.format), element.slotIndex,
                                 element.alignedByteOffset};
      key = XXH3_64bits_withSeed(fields, sizeof(fields), key);
    }
    key = XXH3_64bits_withSeed(&specConstants, 4, key);
    auto& created = pipelines_[key];
    if (!created) {
      const auto start = std::chrono::steady_clock::now();
      created = device_->createGraphicsPipeline(desc);
      ++pipelines_created_;
      ++unsaved_pipelines_;
      pipeline_ms_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }
    if (!created) {
      ++stats_["draw skipped: pipeline creation"];
      static int logged = 0;
      if (logged++ < 40) {
        fprintf(stderr, "pipeline failed: vs %016llX ps %016llX elements %zu slots %zu spec %u colorfmt %d\n",
                (unsigned long long)call.vs_key, (unsigned long long)call.ps_key, inputElements.size(),
                inputSlots.size(), specConstants, int(desc.renderTargetFormat[0]));
      }
      return;
    }
    pipeline = created.get();
    pipeline_inputs_[inputsKey] = pipeline;
  }

  // Constants.
  // Draws in a row often share them: keep the upload.
  const auto uploadConstants = [&](ConstantUpload& last, const uint8_t* source, uint32_t count) {
    const uint32_t bytes = count * 16;
    if (last.upload != upload_ || last.bytes < bytes || last.dirty_begin < std::min(last.dirty_end, count)) {
      last.offset = Allocate(source, bytes, 16);
      last.upload = upload_;
      last.bytes = bytes;
      last.dirty_begin = last.dirty_end = 0;
    }
    return last.offset;
  };
  const bool worldDraw = color.scale_x > color.scale && depthEnabled &&
                         std::find(std::begin(kInterfaceVertexShaders), std::end(kInterfaceVertexShaders),
                                   call.vs_key) == std::end(kInterfaceVertexShaders);
  // The interface keeps its 16:9 shape, in the middle of a wider picture.
  bool interfaceDraw = std::find(std::begin(kInterfaceVertexShaders), std::end(kInterfaceVertexShaders),
                                 call.vs_key) != std::end(kInterfaceVertexShaders);
  // Except what covers the whole screen, like the dimming behind the pause menu.
  if (interfaceDraw && call.inline_vertices && !inputSlots.empty() && call.count <= 6) {
    const uint32_t stride = inputSlots[0].stride;
    float left = FLT_MAX, right = -FLT_MAX;
    for (uint32_t vertex = 0; stride && vertex < call.count && (vertex + 1) * stride <= call.inline_size; ++vertex) {
      const float x = xenos::LoadBEFloat(call.inline_vertices + vertex * stride);
      left = std::min(left, x);
      right = std::max(right, x);
    }
    if (right - left >= 1200.0f) {
      interfaceDraw = false;
      interface_cover_ = true;
    }
  }
  const float xScale = interfaceDraw ? color.scale : color.scale_x;
  const float scaleXy = color.scale;
  const uint64_t vsConstants = uploadConstants(vs_constants_, state.data + xenos::device::kVertexConstants, call.vs_constants);
  const uint64_t psConstants = uploadConstants(ps_constants_, state.data + xenos::device::kPixelConstants, call.ps_constants);
  shared.booleans = (state.U32(xenos::device::kVertexBoolConstants) & 0xFF) |
                    ((state.U32(xenos::device::kPixelBoolConstants) & 0xFF) << 16);
  shared.alphaThreshold = state.RegF(xenos::reg::RB_ALPHA_REF);
  shared.loop16 = state.U32(0x33F0);
  shared.loop17 = state.U32(0x33F4);
  shared.posScaleX = 1.0f / xScale;
  shared.posScaleY = 1.0f / scaleXy;
  // The clears Fusion draws as rect and point lists carry window coordinates: the viewport transform
  // below, inverted, with the half pixel it adds taken out.
  if (const uint32_t vte = state.Reg(xenos::reg::PA_CL_VTE_CNTL); (vte & 0x101) == 0x100) {
    const float pixelCenter = (state.Reg(xenos::reg::PA_SU_VTX_CNTL) & 1) ? 0.0f : 0.5f;
    const float scales[2] = {state.RegF(xenos::reg::PA_CL_VPORT_XSCALE), state.RegF(xenos::reg::PA_CL_VPORT_YSCALE)};
    const float offsets[2] = {state.RegF(xenos::reg::PA_CL_VPORT_XOFFSET), state.RegF(xenos::reg::PA_CL_VPORT_YOFFSET)};
    const float hostScale[2] = {xScale, scaleXy};
    for (int axis = 0; axis < 2; ++axis) {
      if (scales[axis] == 0.0f) continue;
      shared.screenMul[axis] = (call.surface_4x ? 2.0f : 1.0f) / scales[axis];
      shared.screenAdd[axis] = -(offsets[axis] + pixelCenter / hostScale[axis]) / scales[axis];
    }
  }
  if (const uint32_t vte = state.Reg(xenos::reg::PA_CL_VTE_CNTL); (vte & 0x210) == 0x200) {
    if (const float zScale = state.RegF(xenos::reg::PA_CL_VPORT_ZSCALE); zScale != 0.0f) {
      shared.screenMul[2] = 1.0f / zScale;
      shared.screenAdd[2] = -state.RegF(xenos::reg::PA_CL_VPORT_ZOFFSET) / zScale;
    }
  }
  if (export_buffer_) shared.exportMemory = export_buffer_->getDeviceAddress();
  for (uint32_t i = 0; i < export_regions_.size(); ++i) {
    const ExportRegion& region = export_regions_[i];
    shared.exportRegions[i][0] = region.begin;
    shared.exportRegions[i][1] = region.end;
    shared.exportRegions[i][2] = region.offset;
  }
  const uint64_t sharedConstants = Allocate(&shared, sizeof(shared));
  const uint64_t pushConstants[] = {upload_address_ + vsConstants, upload_address_ + psConstants,
                                    upload_address_ + sharedConstants};

  TrackEdram(state, color, depth, colorWriteMask != 0,
             depthWriteEnabled || (stencilEnabled && ((stencilRefMask >> 16) & 0xFF)), ViewportBottom(state),
             true);

  // Viewport from the D3D viewport transform (the shaders invert Y).
  // The spare width beside the 16:9 area. Over the 3D world, the interface's
  // sides follow the screen's sides: the transform in c4 gives the position on a 1280 wide screen.
  // (Menus stay together in the middle: without the world, or after a cover over it.)
  float xOffset = 0.0f;
  if (interfaceDraw && color.scale_x > color.scale) {
    const float spare = (color.scale_x - color.scale) * float(color.width / color.scale_x);
    float transform[4];  // c4, in host order
    std::memcpy(transform, state.data + xenos::device::kVertexConstants + 4 * 16, sizeof(transform));
    float screenX = transform[3];
    if (call.inline_vertices && call.inline_size >= 8) {
      screenX += transform[0] * xenos::LoadBEFloat(call.inline_vertices) +
                 transform[1] * xenos::LoadBEFloat(call.inline_vertices + 4);
    }
    constexpr uint32_t kWorldFrame = 50;
    const bool world = world_draws_last_frame_ >= kWorldFrame && !interface_cover_;
    // The minimap's shader has no such position: it lives in the top right corner.
    const bool minimap = call.vs_key == 0x0A718F34D79ED591ull || call.vs_key == 0x7F31D2881987B4F3ull;
    xOffset = !world ? spare * 0.5f : minimap || screenX > 880.0f ? spare : screenX < 400.0f ? 0.0f : spare * 0.5f;
  }
  world_draws_ += worldDraw;
  const float sx = state.RegF(xenos::reg::PA_CL_VPORT_XSCALE) * xScale;
  // Direct3D 9 pixel centers (pix_center 0): half a pixel, or every full-screen
  // pass samples between texels and blurs the frame.
  const float half = (state.Reg(xenos::reg::PA_SU_VTX_CNTL) & 1) ? 0.0f : 0.5f;
  const float ox = state.RegF(xenos::reg::PA_CL_VPORT_XOFFSET) * xScale + xOffset + half;
  const float sy = state.RegF(xenos::reg::PA_CL_VPORT_YSCALE) * scaleXy;
  const float oy = state.RegF(xenos::reg::PA_CL_VPORT_YOFFSET) * scaleXy + half;
  const float sz = state.RegF(xenos::reg::PA_CL_VPORT_ZSCALE);
  const float oz = state.RegF(xenos::reg::PA_CL_VPORT_ZOFFSET);
  RenderViewport viewport(ox - sx, oy + sy, 2.0f * sx, -2.0f * sy, oz, oz + sz);
  // The window scissor: the lighting pass draws its full-screen quad in strips, each with its own lights.
  RenderRect scissor(0, 0, int32_t(color.width), int32_t(color.height));
  const uint32_t scissorTL = call.scissor_tl, scissorBR = call.scissor_br;
  if (!interfaceDraw && (scissorBR & 0x7FFF) && ((scissorBR >> 16) & 0x7FFF)) {
    scissor.left = std::min(int32_t((scissorTL & 0x7FFF) * xScale), scissor.right);
    scissor.top = std::min(int32_t(((scissorTL >> 16) & 0x7FFF) * scaleXy), scissor.bottom);
    scissor.right = std::min(int32_t((scissorBR & 0x7FFF) * xScale), scissor.right);
    scissor.bottom = std::min(int32_t(((scissorBR >> 16) & 0x7FFF) * scaleXy), scissor.bottom);
  }
  const float boundViewport[] = {ox, oy, sx, sy, oz, sz, float(color.width), float(color.height),
                                 float(scissor.left), float(scissor.top), float(scissor.right), float(scissor.bottom)};
  if (!has_bound_viewport_ || std::memcmp(bound_viewport_, boundViewport, sizeof(boundViewport))) {
    commands_->setViewports(&viewport, 1);
    commands_->setScissors(&scissor, 1);
    std::memcpy(bound_viewport_, boundViewport, sizeof(boundViewport));
    has_bound_viewport_ = true;
  }

  if (bound_pipeline_ != pipeline) {
    commands_->setPipeline(pipeline);
    bound_pipeline_ = pipeline;
  }
  // Polygon offset, or coplanar layers (decals, ground) fight in the distance.
  // Enabled per face; the game sets both alike. The offset is in depth units,
  // and a D32F constant unit is 2^-24 at the far depths where this matters.
  const uint32_t mode = state.Reg(xenos::reg::PA_SU_SC_MODE_CNTL);
  const bool front_offset = mode & (1u << 11), back_offset = mode & (1u << 12);
  if (front_offset || back_offset) {
    const uint32_t base = front_offset ? xenos::reg::PA_SU_POLY_OFFSET_FRONT_SCALE
                                       : xenos::reg::PA_SU_POLY_OFFSET_BACK_SCALE;
    const float bias[2] = {state.RegF(base + 1) * 16777216.0f, state.RegF(base) / 16.0f};
    if (!has_bound_bias_ || std::memcmp(bound_bias_, bias, sizeof(bias))) {
      commands_->setDepthBias(bias[0], 0.0f, bias[1]);
      std::memcpy(bound_bias_, bias, sizeof(bias));
    }
    has_bound_bias_ = true;
    ++polygon_offset_draws_;
  } else if (!has_bound_bias_ || bound_bias_[0] != 0.0f || bound_bias_[1] != 0.0f) {
    commands_->setDepthBias(0.0f, 0.0f, 0.0f);
    bound_bias_[0] = bound_bias_[1] = 0.0f;
    has_bound_bias_ = true;
  }
  commands_->setGraphicsPushConstants(0, pushConstants);
  commands_->setVertexBuffers(0, vertexViews.data(), uint32_t(vertexViews.size()), inputSlots.data());
  if (rects) {
    commands_->drawInstanced(rectCount * 6, 1, 0, 0);
  } else if (call.indices_size && quads) {
    // Each four indices make two triangles.
    std::vector<uint8_t> guest(call.indices_size);
    std::memcpy(guest.data(), physical_ + (call.indices_address & 0x1FFFFFFF), guest.size());
    xenos::SwapEndian(guest.data(), guest.size(), call.indices_endian);
    const uint32_t bytes = call.indices_32bit ? 4 : 2;
    const uint32_t quadCount = std::min<uint32_t>(call.count, uint32_t(guest.size() / bytes)) / 4;
    std::vector<uint8_t> indices(size_t(quadCount) * 6 * bytes);
    for (uint32_t q = 0; q < quadCount; ++q) {
      static constexpr uint8_t kTriangles[] = {0, 1, 2, 0, 2, 3};
      for (uint32_t i = 0; i < 6; ++i) {
        std::memcpy(&indices[(q * 6 + i) * bytes], &guest[(q * 4 + kTriangles[i]) * bytes], bytes);
      }
    }
    const uint64_t at = Allocate(indices.data(), indices.size(), 16);
    RenderIndexBufferView view{upload_->at(at), uint32_t(indices.size()),
                               call.indices_32bit ? RenderFormat::R32_UINT : RenderFormat::R16_UINT};
    commands_->setIndexBuffer(&view);
    commands_->drawIndexedInstanced(quadCount * 6, 1, 0, int32_t(call.base_vertex), 0);
  } else if (call.indices_size) {
    const auto fill = [&](uint8_t* data) { xenos::SwapEndian(data, call.indices_size, call.indices_endian); };
    const RenderBuffer* buffer =
        GetGuestBuffer(call.indices_address, call.indices_size, call.indices_32bit ? 2 : 1, std::ref(fill));
    RenderIndexBufferView view{buffer, call.indices_size,
                               call.indices_32bit ? RenderFormat::R32_UINT : RenderFormat::R16_UINT};
    commands_->setIndexBuffer(&view);
    commands_->drawIndexedInstanced(call.count, 1, call.start, int32_t(call.base_vertex), 0);
  } else if (quads) {
    const uint32_t quadCount = call.count / 4;
    std::vector<uint16_t> indices;
    for (uint32_t q = 0; q < quadCount; ++q) {
      const uint16_t v = uint16_t(call.start + 4 * q);
      for (uint16_t i : {0, 1, 2, 0, 2, 3}) indices.push_back(uint16_t(v + i));
    }
    const uint64_t at = Allocate(indices.data(), indices.size() * 2, 16);
    RenderIndexBufferView view{upload_->at(at), uint32_t(indices.size() * 2), RenderFormat::R16_UINT};
    commands_->setIndexBuffer(&view);
    commands_->drawIndexedInstanced(uint32_t(indices.size()), 1, 0, 0, 0);
  } else {
    commands_->drawInstanced(call.count, 1, call.start, 0);
  }
  ++draws_;
  if (call.export_count) {
    // The draws after this one fetch the textures again.
    ++texture_generation_;
    ++export_generation_;
    for (uint32_t i = 0; i < call.export_count; ++i) {
      if (ExportRegion* region = FindExportRegion(call.exports[i].address)) region->generation = export_generation_;
    }
  }
}

bool Renderer::ReadResolved(uint32_t address, std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height,
                            uint32_t* format_out) {
  auto it = resolved_.find(address & 0xFFFFF000);
  if (it == resolved_.end()) return false;
  Texture& texture = it->second;
  width = texture.width;
  height = texture.height;
  const uint32_t texelBytes = RenderFormatSize(texture.format);
  if (format_out) *format_out = uint32_t(texture.format);
  auto readback = device_->createBuffer(RenderBufferDesc::ReadbackBuffer(uint64_t(width) * height * texelBytes));
  WaitIdle();
  Frame& frame = frames_[frame_];
  commands_ = frame.commands.get();
  commands_->begin();
  layouts_.erase(texture.texture.get());
  layouts_[texture.texture.get()] = RenderTextureLayout::SHADER_READ;
  Transition(texture.texture.get(), RenderTextureLayout::COPY_SOURCE);
  // plume has no texture-to-buffer copy.
  VkBufferImageCopy region{};
  region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region.imageSubresource.layerCount = 1;
  region.imageExtent = {width, height, 1};
  vkCmdCopyImageToBuffer(static_cast<VulkanCommandList*>(commands_)->vk,
                         static_cast<VulkanTexture*>(texture.texture.get())->vk,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, static_cast<VulkanBuffer*>(readback.get())->vk,
                         1, &region);
  Transition(texture.texture.get(), RenderTextureLayout::SHADER_READ);
  commands_->end();
  const RenderCommandList* lists[] = {commands_};
  queue_->executeCommandLists(lists, 1, nullptr, 0, nullptr, 0, frame.fence.get());
  queue_->waitForCommandFence(frame.fence.get());
  const uint8_t* data = static_cast<const uint8_t*>(readback->map());
  rgba.assign(data, data + size_t(width) * height * texelBytes);
  readback->unmap();
  return true;
}

void Renderer::WriteTraceDumps(const std::string& path) {
  // What memory export wrote: each region as guest memory would hold it.
  for (const ExportRegion& region : export_regions_) {
    const uint32_t size = region.end - region.begin;
    auto readback = device_->createBuffer(RenderBufferDesc::ReadbackBuffer(size));
    WaitIdle();
    Frame& frame = frames_[frame_];
    commands_ = frame.commands.get();
    commands_->begin();
    ExportBarrier();
    commands_->copyBufferRegion(readback->at(0), export_buffer_->at(region.offset), size);
    commands_->end();
    const RenderCommandList* lists[] = {commands_};
    queue_->executeCommandLists(lists, 1, nullptr, 0, nullptr, 0, frame.fence.get());
    queue_->waitForCommandFence(frame.fence.get());
    char name[64];
    snprintf(name, sizeof(name), ".export.%08X_%X.raw", region.begin, size);
    if (FILE* f = fopen((path + name).c_str(), "wb")) {
      fwrite(readback->map(), 1, size, f);
      readback->unmap();
      fclose(f);
    }
  }
  // The virtual texture table as it stands after the traced frame, level by level (RGBA16F raw values).
  if (table_texture_ && !trace_dumps_.empty()) {
    for (uint32_t level = 0; level < table_levels_.size(); ++level) {
      const xenos::MipLevel& m = table_levels_[level];
      const uint64_t size = uint64_t(m.width) * m.height * 8;
      auto readback = device_->createBuffer(RenderBufferDesc::ReadbackBuffer(size));
      WaitIdle();
      Frame& frame = frames_[frame_];
      commands_ = frame.commands.get();
      commands_->begin();
      layouts_[table_texture_] = RenderTextureLayout::SHADER_READ;
      Transition(table_texture_, RenderTextureLayout::COPY_SOURCE);
      VkBufferImageCopy region{};
      region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      region.imageSubresource.mipLevel = level;
      region.imageSubresource.layerCount = 1;
      region.imageExtent = {m.width, m.height, 1};
      vkCmdCopyImageToBuffer(static_cast<VulkanCommandList*>(commands_)->vk, static_cast<VulkanTexture*>(table_texture_)->vk,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, static_cast<VulkanBuffer*>(readback.get())->vk, 1, &region);
      Transition(table_texture_, RenderTextureLayout::SHADER_READ);
      commands_->end();
      const RenderCommandList* lists[] = {commands_};
      queue_->executeCommandLists(lists, 1, nullptr, 0, nullptr, 0, frame.fence.get());
      queue_->waitForCommandFence(frame.fence.get());
      char name[64];
      snprintf(name, sizeof(name), ".table.%u_%ux%u.raw", level, m.width, m.height);
      if (FILE* f = fopen((path + name).c_str(), "wb")) {
        fwrite(readback->map(), 1, size, f);
        readback->unmap();
        fclose(f);
      }
    }
  }
  for (TraceDump& dump : trace_dumps_) {
    char name[64];
    snprintf(name, sizeof(name), ".%03u.%08X_%ux%u_%u.raw", dump.index, dump.address, dump.width, dump.height, dump.format);
    if (FILE* f = fopen((path + name).c_str(), "wb")) {
      fwrite(dump.buffer->map(), 1, size_t(dump.width) * dump.height * dump.bytes, f);
      dump.buffer->unmap();
      fclose(f);
    }
  }
  trace_dumps_.clear();
}

}  // namespace native_gpu
