#pragma once
#include "gfx.hpp"
#include "hd.hpp"
#include "cursor_feedback.hpp"
#include "ui_feedback.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <mutex>
#include <memory>
#include <deque>
#include <map>
#include <vector>
#include <utility>

namespace sc64::desktop {
// Owned VI registers in the pinned runtime's ViRegs order. Never retain its
// shared register pointer outside RendererContext::update_screen().
struct Scanout {
    std::array<std::uint32_t,14> regs{};
    std::uint64_t revision=0, field=0, completed_task=0, minimum_task=0;
    bool blank() const {
        return (regs[0]&3)<2 || (regs[9]>>16)>=(regs[9]&0xffff)
            || (regs[10]>>16)>=(regs[10]&0xffff);
    }
};
class ScanoutMailbox {
public:
    void publish(std::array<std::uint32_t,14> regs, std::uint64_t field, std::uint64_t task) {
        std::lock_guard lock(mutex_);
        Scanout next{regs,latest_.revision+1,field,task,latest_.minimum_task};
        // Retain invalidation even if the engine misses every blank VI field.
        // An allocation reused after blanking must get a new completed task.
        if ((!latest_.blank() && next.blank()) || regs[2]!=latest_.regs[2]
            || (regs[0]&3)!=(latest_.regs[0]&3)) next.minimum_task=task+1;
        latest_=next;
        if (field>=trace_from_ && field<=trace_to_ && trace_.size()<2048) trace_.push_back(next);
    }
    Scanout latest() { std::lock_guard lock(mutex_); return latest_; }
    void trace(std::uint64_t from,std::uint64_t to) {
        std::lock_guard lock(mutex_); trace_from_=from; trace_to_=to;
    }
    std::vector<Scanout> take_trace() {
        std::lock_guard lock(mutex_); return std::exchange(trace_,{});
    }
private:
    std::mutex mutex_;
    Scanout latest_;
    std::uint64_t trace_from_=1,trace_to_=0;
    std::vector<Scanout> trace_;
};
// A native target image lies wholly inside guest RDRAM. A target whose
// scissor extent runs past the top of RDRAM cannot be exported or scanned.
inline bool target_in_ram(gfx::Image image,unsigned height,std::size_t ram_size) {
    const std::uint64_t stride=std::uint64_t(image.width)*(image.size==2?2:4);
    return std::uint64_t(image.address)+stride*height<=ram_size;
}
// VI geometry only: mode, width, pixel size and origin within the image.
inline bool scanout_covers(const Scanout& vi,gfx::Image image,unsigned height,std::size_t ram_size) {
    if (vi.blank() || image.format!=0 || image.width==0 || image.width>1024 || height==0 || height>1024
        || image.width!=vi.regs[2] || image.size!=(vi.regs[0]&3)) return false;
    const std::uint64_t stride=std::uint64_t(image.width)*(image.size==2?2:4);
    const std::uint64_t origin=vi.regs[1]&0xffffff;
    // VI field origins in the recorded modes point at row one or row two.
    return image.address<=origin && origin-image.address<=stride*2
        && (origin-image.address)%stride==0
        && target_in_ram(image,height,ram_size);
}
inline bool scanout_matches(const Scanout& vi,gfx::Image image,unsigned height,
                            std::uint64_t task,std::size_t ram_size) {
    return task>=vi.minimum_task && task<=vi.completed_task
        && scanout_covers(vi,image,height,ram_size);
}
// Native columns/rows covered by the nominal NTSC raster: 640 output pixels and
// 240 lines per field through the VI X/Y scale registers (low 12 bits, 2.10
// fixed point). Recorded menu, 1P and 2P modes cover their whole native target;
// the opening animation's Y scale 716 covers 168 of 240 rows.
struct ViRegion {
    unsigned width=0, height=0;
    bool operator==(const ViRegion&) const = default;
};
inline ViRegion vi_region(const Scanout& vi,unsigned width,unsigned height) {
    const unsigned x_scale=vi.regs[12]&0xfff, y_scale=vi.regs[13]&0xfff;
    const unsigned columns=x_scale ? (640*x_scale+512)/1024 : width;
    const unsigned rows=y_scale ? (240*y_scale+512)/1024 : height;
    return {std::clamp(columns,1u,std::max(width,1u)),std::clamp(rows,1u,std::max(height,1u))};
}
// Present the covered region over the whole native image size, as the VI scales
// it over the raster; nearest rows/columns, no VI filtering. Identity when the
// region is the whole image, which keeps native coordinates for those modes.
inline std::vector<std::uint8_t> vi_present(const std::vector<std::uint8_t>& rgba,
                                            unsigned width,unsigned height,ViRegion region) {
    if (region==ViRegion{width,height} || rgba.size()!=std::size_t(width)*height*4) return rgba;
    std::vector<std::uint8_t> out(rgba.size());
    for (unsigned y=0;y<height;++y) {
        const unsigned sy=unsigned(std::uint64_t(y)*region.height/height);
        for (unsigned x=0;x<width;++x) {
            const unsigned sx=unsigned(std::uint64_t(x)*region.width/width);
            std::copy_n(rgba.begin()+(std::size_t(sy)*width+sx)*4,4,out.begin()+(std::size_t(y)*width+x)*4);
        }
    }
    return out;
}
// Canonical RGBA16 framebuffer bytes owned at osViSwapBuffer, for a buffer no
// RDP task drew since its previous swap: the CPU-written boot logos and title.
struct CpuScanout {
    gfx::Image image;
    unsigned height=0;
    std::uint64_t after_task=0; // newer than this task, older than the next
    std::vector<std::uint8_t> canonical;
};
// Guest threads record RDP targets and swaps; the engine thread selects. A CPU
// image stays valid until a newer task drawing its address has completed, so
// the buffer keeps its owned bytes while that task's image is not yet shown.
class CpuScanouts {
public:
    void targeted(std::uint32_t address,std::uint64_t task) {
        std::lock_guard lock(mutex_);
        bound(drawn_); drawn_[address]=std::max(drawn_[address],task);
    }
    // True when no task drew this buffer since it was last swapped in. A swap
    // of task-drawn bytes retires that address's older CPU image once the
    // drawing task has completed, independently of later target bookkeeping.
    bool swapped(std::uint32_t address,std::uint64_t submitted) {
        std::lock_guard lock(mutex_);
        bound(shown_);
        const auto drawn=drawn_.find(address);
        const auto shown=shown_.find(address);
        const bool cpu=drawn==drawn_.end() || (shown!=shown_.end() && drawn->second<=shown->second);
        shown_[address]=submitted;
        if (const auto image=images_.find(address); !cpu && image!=images_.end() && !retire_.contains(address))
            retire_[address]=drawn->second;
        return cpu;
    }
    void publish(std::shared_ptr<const CpuScanout> image) {
        std::lock_guard lock(mutex_);
        if (drawn_.contains(image->image.address) && drawn_[image->image.address]>image->after_task) return;
        // An unchanged repeated swap keeps the converted image when no task
        // drew this buffer in between.
        retire_.erase(image->image.address);
        auto& slot=images_[image->image.address];
        const auto drawn=drawn_.find(image->image.address);
        if (slot && (drawn==drawn_.end() || drawn->second<=slot->after_task)
            && slot->image==image->image && slot->height==image->height
            && slot->canonical==image->canonical) return;
        slot=std::move(image);
        while (images_.size()>4) {
            auto oldest=images_.begin();
            for (auto it=images_.begin();it!=images_.end();++it)
                if (it->second->after_task<oldest->second->after_task) oldest=it;
            retire_.erase(oldest->first);
            images_.erase(oldest);
        }
    }
    std::shared_ptr<const CpuScanout> select(const Scanout& vi,std::size_t ram_size) {
        std::lock_guard lock(mutex_);
        for (const auto& [address,image]:images_) {
            if (const auto retire=retire_.find(address);
                retire!=retire_.end() && retire->second<=vi.completed_task) continue;
            const auto drawn=drawn_.find(address);
            if (drawn!=drawn_.end() && drawn->second>image->after_task
                && drawn->second<=vi.completed_task) continue;
            // VI blank/mode/width transitions retire images from before them.
            if (image->after_task+1<vi.minimum_task) continue;
            if (scanout_covers(vi,image->image,image->height,ram_size)) return image;
        }
        return {};
    }
private:
    // Keep the most recent 48 of at most 64 addresses; never forget them all.
    static void bound(std::map<std::uint32_t,std::uint64_t>& map) {
        while (map.size()>48) {
            auto oldest=map.begin();
            for (auto it=map.begin();it!=map.end();++it) if (it->second<oldest->second) oldest=it;
            map.erase(oldest);
        }
    }
    std::mutex mutex_;
    std::map<std::uint32_t,std::uint64_t> drawn_, shown_;
    std::map<std::uint32_t,std::shared_ptr<const CpuScanout>> images_;
    std::map<std::uint32_t,std::uint64_t> retire_; // per stored image only
};
struct ScanoutImage {
    gfx::Image image;
    unsigned height=0;
    std::uint64_t task=0;
    std::vector<std::uint8_t> rgba;
    std::shared_ptr<hd::Pending> hd; // 2x composite of an HD mission frame (hd.hpp), being made
    // Input geometry must follow the selected VI image, including retained
    // older tasks while the newest target is offscreen or not yet scanned.
    CursorFeedback cursor;
    UiFeedback ui;
};
class ScanoutImages {
public:
    void add(std::shared_ptr<const ScanoutImage> image) {
        // Eight bounded native RGBA images (at most 32 MiB). Retain previous
        // tasks too: an upcoming RDP target must not replace the scanned one.
        images_.push_back(std::move(image));
        while (images_.size()>8) images_.pop_front();
    }
    std::shared_ptr<const ScanoutImage> select(const Scanout& vi) const {
        for (auto it=images_.rbegin();it!=images_.rend();++it)
            if (scanout_matches(vi,(*it)->image,(*it)->height,(*it)->task,0x800000)) return *it;
        return {};
    }
private:
    std::deque<std::shared_ptr<const ScanoutImage>> images_;
};
}
