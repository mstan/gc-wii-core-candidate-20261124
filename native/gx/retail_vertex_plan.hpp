// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace gcnllr {
// A native layout's invariant work belongs to its CP format lifetime. Draws
// inspect only index bytes; all-direct geometry has no array-sizing work.
class RetailVertexPlan {
    struct Field {
        uint32_t offset,attribute,width,components;
        bool operator==(const Field& rhs) const {
            return offset==rhs.offset && attribute==rhs.attribute && width==rhs.width && components==rhs.components;
        }
    };
    std::array<Field,32> fields_{};
    uint32_t stride_{},count_{};
public:
    bool operator==(const RetailVertexPlan& rhs) const {
        if (stride_!=rhs.stride_ || count_!=rhs.count_) return false;
        for (uint32_t i=0;i<count_;++i) if (!(fields_[i]==rhs.fields_[i])) return false;
        return true;
    }
    uint32_t stride() const {return stride_;}
    void direct(uint32_t bytes) {
        if(bytes>UINT32_MAX-stride_)throw std::runtime_error("Vertex layout overflow");
        stride_+=bytes;
    }
    void index(uint32_t attribute,uint32_t width,uint32_t components,bool array=true) {
        if((width!=1 && width!=2) || !components || components>3 || attribute>=32)
            throw std::runtime_error("Unsupported vertex index layout");
        if(array) {
            if(count_==fields_.size())throw std::runtime_error("Too many indexed vertex fields");
            fields_[count_++]={stride_,attribute,width,components};
        }
        direct(width*components);
    }
    template<class Stride,class Sizes>
    void accumulate(const uint8_t* vertices,size_t bytes,uint32_t count,bool big_endian,
                    Stride array_stride,Sizes& sizes) const {
        if(uint64_t(stride_)*count>bytes)throw std::runtime_error("Vertex index input truncated");
        if(!count)return;
        for(uint32_t f=0;f<count_;++f) {
            const auto& field=fields_[f];uint32_t maximum=0;
            const uint8_t* row=vertices+field.offset;
            for(uint32_t v=0;v<count;++v,row+=stride_) {
                const uint8_t* value=row;
                for(uint32_t n=0;n<field.components;++n,value+=field.width) {
                    const uint32_t index=field.width==1?value[0]:big_endian?
                        (uint32_t(value[0])<<8)|value[1]:(uint32_t(value[1])<<8)|value[0];
                    if(index>maximum)maximum=index;
                }
            }
            const uint64_t required=uint64_t(maximum+1u)*array_stride(field.attribute);
            if(required>UINT32_MAX)throw std::runtime_error("Vertex array extent overflow");
            auto& size=sizes[field.attribute];
            if(required>size)size=uint32_t(required);
        }
    }
};
}
