// SPDX-License-Identifier: MIT
// Included in gfx after the shared GPU buffers and worker are declared.
// Transient GPU output and the persistent ranges share existing draw bindings.
struct GeometryRegion {
    uint64_t start{},limit{},next{};
};
static std::array<GeometryRegion,3> geometry_regions;
static uint64_t geometry_generation=1;
static std::array<uint64_t,3> geometry_uploaded_bytes;
static uint64_t geometry_uploads{},geometry_recycles{},geometry_pressure{};
static bool geometry_recycle_pending;

static uint64_t geometry_buffer_size(GeometryKind kind,uint64_t transient) {
    const auto index=static_cast<size_t>(kind);
    constexpr uint64_t MiB=1024u*1024u;
    constexpr std::array<uint64_t,3> reserve{24*MiB,8*MiB,24*MiB};
    wgpu::Limits limits{};
    if(!g_device.GetLimits(&limits))return transient;
    const uint64_t ceiling=kind==GeometryKind::indices ? limits.maxBufferSize :
        std::min(limits.maxBufferSize,limits.maxStorageBufferBindingSize);
    const uint64_t capacity=ceiling>transient ? std::min(ceiling-transient,reserve[index])&~uint64_t(255u):0;
    geometry_regions[index]={transient,transient+capacity,transient};
    return transient+capacity;
}

uint64_t persistent_geometry_generation() {return geometry_generation;}
static uint64_t geometry_allocation_size(GeometryKind kind,size_t length) {
    // Uint16 draws bind their exact byte range. The stronger requirement
    // is the queue write, which needs four-byte offsets and lengths.
    const uint64_t alignment=kind==GeometryKind::indices ? 4u : 256u;
    return (length+alignment-1u)&~(alignment-1u);
}

bool persistent_geometry_room(GeometryKind kind,size_t length) {
    if(!length || length>UINT32_MAX-255u)return false;
    const auto& region=geometry_regions[static_cast<size_t>(kind)];
    const uint64_t aligned=geometry_allocation_size(kind,length);
    if(aligned<=region.limit-region.next)return true;
    ++geometry_pressure;
    if(aligned<=region.limit-region.start)geometry_recycle_pending=true;
    return false;
}

Range retain_geometry(GeometryKind kind,const uint8_t* bytes,size_t length) {
    if(!persistent_geometry_room(kind,length))return {};
    auto& region=geometry_regions[static_cast<size_t>(kind)];
    const std::array<const wgpu::Buffer*,3> buffers{&g_vertexBuffer,&g_indexBuffer,&g_storageBuffer};
    const auto& buffer=*buffers[static_cast<size_t>(kind)];
    const size_t prefix=length&~size_t(3u);
    if(prefix)g_queue.WriteBuffer(buffer,region.next,bytes,prefix);
    if(prefix!=length) {
        std::array<uint8_t,4> end{};
        std::memcpy(end.data(),bytes+prefix,length-prefix);
        g_queue.WriteBuffer(buffer,region.next+prefix,end.data(),end.size());
    }
    const Range range{static_cast<uint32_t>(region.next),static_cast<uint32_t>(length)};
    region.next+=geometry_allocation_size(kind,length);
    ++geometry_uploads;geometry_uploaded_bytes[static_cast<size_t>(kind)]+=length;
    return range;
}

static void begin_geometry_frame(bool continuation) {
    if(continuation || !geometry_recycle_pending)return;
    // The prior packet's commands must reach the queue before reusing a
    // persistent offset. Queue ordering protects their remaining GPU reads.
    render_worker::synchronize();
    for(auto& region:geometry_regions)region.next=region.start;
    if(geometry_generation==UINT64_MAX)throw std::runtime_error("Persistent geometry generations exhausted");
    ++geometry_generation;++geometry_recycles;
    geometry_recycle_pending=false;
}
static void release_geometry() {
    const auto& indices=geometry_regions[static_cast<size_t>(GeometryKind::indices)];
    std::fprintf(stderr,"[native-geometry-arena] uploads=%llu vertex-bytes=%llu index-bytes=%llu array-bytes=%llu index-range-bytes=%llu recycles=%llu pressure=%llu\n",
        (unsigned long long)geometry_uploads,(unsigned long long)geometry_uploaded_bytes[0],
        (unsigned long long)geometry_uploaded_bytes[1],(unsigned long long)geometry_uploaded_bytes[2],
        (unsigned long long)(indices.next-indices.start),
        (unsigned long long)geometry_recycles,(unsigned long long)geometry_pressure);
    geometry_regions={};geometry_generation=1;geometry_recycle_pending=false;
    geometry_uploaded_bytes={};
    geometry_uploads=geometry_recycles=geometry_pressure=0;
}
