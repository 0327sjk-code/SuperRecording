#include "annotations/AnnotationCompositor.h"
#include <algorithm>
#include <cstring>
namespace qrec::annotations {
bool FrameCompositor::Apply(std::span<std::uint8_t> pixels,unsigned width,unsigned height,unsigned stride,Time time) {
    if (!scene_ || scene_->Marks().empty()) return true;
    if (!width || !height || stride<width*4 || pixels.size()<static_cast<std::size_t>(stride)*height) return false;
    bool changed=width_!=width || height_!=height || stride_!=stride;
    std::size_t activeCount=0;
    for (const auto& mark : scene_->Marks()) if (Visible(mark,time)) {
        if (activeCount>=active_.size() || active_[activeCount]!=mark.id) changed=true;
        ++activeCount;
    }
    changed=changed || activeCount!=active_.size();
    if (changed) {
        active_.clear(); runs_.clear();
        for (const auto& mark : scene_->Marks()) if (Visible(mark,time)) active_.push_back(mark.id);
        width_=width; height_=height; stride_=stride;
        if (active_.empty()) return true;
        overlay_.assign(static_cast<std::size_t>(stride)*height,0);
        if (!Render(overlay_,width,height,stride,scene_,time,Surface::PremultipliedOverlay)) return false;
        for (unsigned y=0;y<height;++y) {
            unsigned x=0;
            while (x<width) {
                const std::size_t offset=static_cast<std::size_t>(y)*stride+x*4;
                const auto alpha=overlay_[offset+3];
                if (alpha==0) { ++x; continue; }
                const bool opaque=alpha==255; const unsigned begin=x++;
                while (x<width) {
                    const auto a=overlay_[static_cast<std::size_t>(y)*stride+x*4+3];
                    if (a==0 || (a==255)!=opaque) break;
                    ++x;
                }
                runs_.push_back({offset,x-begin,opaque});
            }
        }
    }
    for (const Run& run : runs_) {
        auto* target=pixels.data()+run.offset; const auto* source=overlay_.data()+run.offset;
        if (run.opaque) { std::memcpy(target,source,run.count*4); continue; }
        for (std::size_t i=0;i<run.count;++i,target+=4,source+=4) {
            const unsigned inverse=255U-source[3];
            for (unsigned channel=0;channel<3;++channel)
                target[channel]=static_cast<std::uint8_t>(std::min(255U,static_cast<unsigned>(source[channel])+(target[channel]*inverse+127U)/255U));
            target[3]=255;
        }
    }
    return true;
}
}
