// Native D3D layer: lays a tiled guest texture out linearly, from the buffer memory export
// writes. One thread for each block; the result goes to a buffer that is copied to the texture.

struct PushConstants
{
    uint64_t Source;      // the texture's first byte in the export buffer
    uint64_t Target;
    uint SourceSize;      // bytes of the export buffer from Source on
    uint WidthBlocks;
    uint HeightBlocks;
    uint PitchBlocks;
    uint BlockDwords;
    uint BlockLog2;       // log2 of the block size in bytes
    uint Endian;
    uint Tiled;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_Push;

// Offset of block (x, y) in a tiled surface, as xenos_decode.h's TiledOffset2D.
int TiledOffset2D(int x, int y, uint pitch, uint log2)
{
    pitch = (pitch + 31) & ~31u;
    int macro = ((x >> 5) + (y >> 5) * int(pitch >> 5)) << (log2 + 7);
    int micro = ((x & 7) + ((y & 0xE) << 2)) << log2;
    int offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
    return ((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
           (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}

uint Swap(uint v, uint endian)
{
    if (endian == 1)
        return ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
    if (endian == 2)
        return (v << 24) | ((v & 0xFF00u) << 8) | ((v >> 8) & 0xFF00u) | (v >> 24);
    if (endian == 3)
        return (v << 16) | (v >> 16);
    return v;
}

[numthreads(8, 8, 1)]
void CSMain(uint2 id : SV_DispatchThreadID)
{
    if (id.x >= g_Push.WidthBlocks || id.y >= g_Push.HeightBlocks)
        return;
    uint blockBytes = 1u << g_Push.BlockLog2;
    uint offset = g_Push.Tiled != 0 ? uint(TiledOffset2D(int(id.x), int(id.y), g_Push.PitchBlocks, g_Push.BlockLog2))
                                    : (id.y * g_Push.PitchBlocks + id.x) * blockBytes;
    uint64_t target = g_Push.Target + uint64_t(id.y * g_Push.WidthBlocks + id.x) * blockBytes;
    for (uint d = 0; d < g_Push.BlockDwords; ++d)
    {
        uint value = offset + d * 4 + 4 <= g_Push.SourceSize ? vk::RawBufferLoad<uint>(g_Push.Source + offset + d * 4) : 0;
        vk::RawBufferStore<uint>(target + d * 4, Swap(value, g_Push.Endian));
    }
}
