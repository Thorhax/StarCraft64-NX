#include "gfx.hpp"
#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <tuple>

namespace sc64::gfx {
namespace {
int floor_div(std::int64_t n,int d) {return n>=0?n/d:-int((-n+d-1)/d);}
int address(int value,unsigned mask,unsigned mode,int maximum,bool copy) {
    if(!copy && ((mode&2) || mask==0))value=std::clamp(value,0,maximum*32);
    int coord=floor_div(value,32);
    if(mask) {
        unsigned bit=std::min(mask,10u);
        if((mode&1) && ((coord>>bit)&1))coord=~coord;
        coord&=((1u<<mask)-1)&1023;
    }
    return coord;
}
int shifted(int v,unsigned shift) {
    v=std::int16_t(v);
    if(shift<=10)return floor_div(v,1<<shift);
    return std::int16_t(v*(1<<(16-shift)));
}
Color sample(const Texture& tex,int s,int t,bool filter,bool copy) {
    if(!tex.background){s=shifted(s,tex.tile.shifts)-tex.tile.sl*8;t=shifted(t,tex.tile.shiftt)-tex.tile.tl*8;}
    auto fetch=[&](int ss,int tt){
        int x,y;
        if(tex.background) {
            x=floor_div(ss,32);y=floor_div(tt,32);
            x=(x%int(tex.width)+tex.width)%tex.width;y=(y%int(tex.height)+tex.height)%tex.height;
        } else {
            x=address(ss,tex.tile.masks,tex.tile.cms,(tex.tile.sh-tex.tile.sl)/4,copy);
            y=address(tt,tex.tile.maskt,tex.tile.cmt,(tex.tile.th-tex.tile.tl)/4,copy);
            return tex.memory.fetch(tex.tile,x,y,tex.tlut_mode);
        }
        return tex.pixels[std::size_t(y)*tex.width+x];
    };
    auto c=fetch(s,t);if(!filter)return c;
    // RDP's three-point interpolation, in 5-bit fractional coordinates.
    int fs=s-floor_div(s,32)*32,ft=t-floor_div(t,32)*32;
    Color a,b,d;int wa,wb,wd;
    if(fs+ft<32) {a=c;b=fetch(s+32,t);d=fetch(s,t+32);wa=32-fs-ft;wb=fs;wd=ft;}
    else {a=fetch(s+32,t+32);b=fetch(s,t+32);d=fetch(s+32,t);wa=fs+ft-32;wb=32-fs;wd=32-ft;}
    return {(a.r*wa+b.r*wb+d.r*wd+16)/32,(a.g*wa+b.g*wb+d.g*wd+16)/32,
            (a.b*wa+b.b*wb+d.b*wd+16)/32,(a.a*wa+b.a*wb+d.a*wd+16)/32};
}
int channel(Color c,unsigned index) {return index==0?c.r:index==1?c.g:index==2?c.b:c.a;}
Color combine(Color tex,const State& state) {
    unsigned hi=state.combine>>32,lo=state.combine;
    unsigned a=hi>>5&15,b=lo>>24&15,c=hi&31,d=lo>>6&7;
    unsigned aa=lo>>21&7,ab=lo>>3&7,ac=lo>>18&7,ad=lo&7;
    // RGB input tables follow angrylion combiner.c set_{suba,subb,mul,add}_rgb_input.
    // kind: 0 subtract A, 1 subtract B, 2 multiply C, 3 add D. Only explicit
    // zero codes return 0; every unmodelled source fails instead.
    auto rgb=[&](unsigned code,unsigned lane,unsigned kind) {
        if(code==1 || code==2)return channel(tex,lane);
        if(code==3)return channel(state.primitive,lane);
        if(code==4)return 0; // rectangle shade is initialized to zero by RSP/RDP
        if(code==5)return channel(state.environment,lane);
        if(code==0)throw std::runtime_error("Combiner depends on unimplemented combined input");
        switch(kind) {
        case 0:
            if(code==6)return 256;
            if(code==7)throw std::runtime_error("Combiner depends on unimplemented noise input");
            return 0; // 8..15
        case 1:
            if(code==6 || code==7)throw std::runtime_error("Combiner depends on unimplemented key center/K4 input");
            return 0; // 8..15
        case 2:
            if(code==8 || code==9)return tex.a;
            if(code==10)return state.primitive.a;
            if(code==11)return 0; // shade alpha
            if(code==12)return state.environment.a;
            if(code<=15)throw std::runtime_error("Combiner depends on unimplemented key scale/combined alpha/LOD/K5 input");
            return 0; // 16..31
        default:
            return code==6?256:0; // 7 is zero
        }
    };
    auto alpha=[&](unsigned code,bool mul) {
        if(code==1 || code==2)return tex.a;
        if(code==3)return state.primitive.a;
        if(code==5)return state.environment.a;
        if(code==6 && !mul)return 256;
        if(code==0 || (code==6 && mul))throw std::runtime_error("Unimplemented alpha LOD/combined input");
        return 0;
    };
    auto calc=[](int a,int b,int c,int d){return std::clamp(((a-b)*c+d*256+128)>>8,0,255);};
    Color result;
    for(unsigned i=0;i<3;++i) {
        int cv=rgb(c,i,2);int v=cv?calc(rgb(a,i,0),rgb(b,i,1),cv,rgb(d,i,3)):std::clamp(rgb(d,i,3),0,255);
        if(i==0)result.r=v;else if(i==1)result.g=v;else result.b=v;
    }
    int av=alpha(ac,true);result.a=av?calc(alpha(aa,false),alpha(ab,false),av,alpha(ad,false)):std::clamp(alpha(ad,false),0,255);
    return result;
}
void dither(Color& c,unsigned mode,int x,int y) {
    static constexpr int magic[]={0,6,1,7,4,2,5,3,3,5,2,4,7,1,6,0};
    static constexpr int bayer[]={0,4,1,5,4,0,5,1,3,7,2,6,7,3,6,2};
    if(mode==3)return;
    if(mode==2)throw std::runtime_error("Noise RGB dither is unsupported");
    int v=(mode==0?magic:bayer)[(y&3)*4+(x&3)];
    for(int* component:{&c.r,&c.g,&c.b})if((*component&7)>v)*component=std::min(255,(*component&248)+8);
}
// COPY alpha compare into a 16-bit target tests bit 0 of each raw 16-bit
// TMEM texel or TLUT entry; BLEND alpha and alpha dither are not used
// (angrylion rasterizer.c render_spans_copy, fetch_qword_copy). Recover that
// bit from the decoded color only where the decode is lossless for it.
bool copy_alpha_bit(const Texture& tex,const Rectangle& r,Color c) {
    // The TLUT mode in force when this rectangle's texels were decoded.
    const unsigned tlut=(r.state.other_hi>>14)&3;
    const bool palette=tlut==2 || tlut==3;
    const auto& t=tex.tile;
    // Tmem::fetch/source_texel reject CI without an RGBA16/IA16 TLUT.
    // RGBA16 alpha decodes to 0/255; IA16 keeps its raw low byte.
    if(t.format==2 && palette)return tlut==2?c.a!=0:(c.a&1)!=0;
    if(!palette && t.size==2 && t.format==0)return c.a!=0;
    if(!palette && t.size==2 && t.format==3)return (c.a&1)!=0;
    // Hardware TLUT applies to non-CI tiles and 4/8/32-bit COPY texels form
    // other halfwords; neither raw value is represented by the decoder.
    throw std::runtime_error("COPY alpha compare needs a 16-bit texel or CI TLUT entry");
}
Color blend(Color color,Color memory,const State& state) {
    unsigned lo=state.other_lo;
    if(!(lo&(1u<<14)))return color;
    unsigned p=lo>>30&3,a=lo>>26&3,m=lo>>22&3,b=lo>>18&3;
    auto input=[&](unsigned code){return code==0?color:code==1?memory:code==2?state.blend:state.fog;};
    Color first=input(p),second=input(m);
    int weight=a==0?color.a:a==1?state.fog.a:a==2?0:0;
    int weight2=b==0?255-weight:b==1?memory.a:b==2?255:0;
    // Full-coverage rectangle baseline. RDP coverage/AA resolve remains explicit
    // debt D-029 until independently measured original target regressions pass.
    if(weight==255)return first;
    auto value=[&](int x,int y){return std::clamp((x*(weight>>3)+y*((weight2>>3)+1))>>5,0,255);};
    return {value(first.r,second.r),value(first.g,second.g),value(first.b,second.b),color.a};
}
}
PixelBounds raster_bounds(const Rectangle& r) {
    return {std::max({0,(r.x0+3)/4,(r.state.scissor.x0+3)/4}),
            std::max({0,(r.y0+3)/4,(r.state.scissor.y0+3)/4}),
            std::min({int(r.target.width),(r.x1+3)/4,r.state.scissor.x1/4}),
            std::min((r.y1+3)/4,r.state.scissor.y1/4)};
}
std::vector<Batch> adjacent_batches(const DrawList& list) {
    std::vector<Batch> batches;
    for(unsigned i=0;i<list.rectangles.size();++i) {
        bool merge=false;
        if(i) {
            const auto& a=list.rectangles[i-1];const auto& b=list.rectangles[i];
            merge=a.texture==b.texture && a.target==b.target && a.state==b.state;
        }
        if(merge)++batches.back().count;else batches.push_back({i,1});
    }
    return batches;
}
RasterPool::RasterPool(unsigned helpers) {
    threads_.reserve(helpers);
    for(unsigned i=0;i<helpers;++i)threads_.emplace_back([this]{work();});
}
RasterPool::~RasterPool() {
    {std::lock_guard lock(mutex_);stop_=true;}
    wake_.notify_all();
    for(auto& thread:threads_)thread.join();
}
#if defined(__SWITCH__)
extern "C" std::uint32_t svcSetThreadCoreMask(std::uint32_t handle, std::int32_t core, std::uint32_t mask);
#endif
unsigned RasterPool::default_helpers() {
#if defined(__SWITCH__)
    // hardware_concurrency() is 0 on libnx. The submitting guest thread (core 1)
    // rasterizes too. Helper 0 on core 0 (shell), helper 1 on core 3 when the
    // launch grants it (4-core forwarder), helper 2 on core 2 (Gfx/SP thread,
    // mostly idle). Helpers keep libnx's lowest priority, so they only take
    // otherwise idle time on shared cores.
    return 3;
#else
    const unsigned cores=std::thread::hardware_concurrency();
    return cores>2?std::min(3u,cores-2):0;
#endif
}
void RasterPool::work() {
#if defined(__SWITCH__)
    static std::atomic<unsigned> slots{0};
    const unsigned slot=slots.fetch_add(1);
    int core=slot==2?2:0;
    if(slot==1 && svcSetThreadCoreMask(0xFFFF8000u,3,1u<<3)==0)core=3;
    else svcSetThreadCoreMask(0xFFFF8000u,core,1u<<core);
    std::printf("SC64 raster helper %u on core %d\n",slot,core);
    std::fflush(stdout);
#endif
    std::uint64_t seen=0;
    for(;;) {
        const std::function<void(unsigned)>* job=nullptr;unsigned count=0;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock,[&]{return stop_ || generation_!=seen;});
            if(stop_)return;
            seen=generation_;job=job_;count=count_;
        }
        for(unsigned i;(i=next_.fetch_add(1))<count;)(*job)(i);
        // Every helper checks out of every generation, so job_ stays valid
        // until the caller observes busy_==0.
        std::lock_guard lock(mutex_);
        if(--busy_==0)done_.notify_all();
    }
}
void RasterPool::drain(const std::function<void()>& idle) {
    std::unique_lock lock(mutex_);
    while(busy_) {
        if(!idle){done_.wait(lock);continue;}
        if(done_.wait_for(lock,std::chrono::milliseconds(1),[&]{return busy_==0;}))break;
        lock.unlock();idle();lock.lock();
    }
}
void RasterPool::run(unsigned count, const std::function<void(unsigned)>& job, const std::function<void()>& idle) {
    if(!count)return;
    {
        std::lock_guard lock(mutex_);
        job_=&job;count_=count;next_.store(0);busy_=helpers();++generation_;
    }
    wake_.notify_all();
    try {
        for(unsigned i;(i=next_.fetch_add(1))<count;) {
            job(i);
            if(idle)idle();
        }
        drain(idle);
    } catch(...) {
        // Cancel unstarted jobs; running ones reference caller-owned data.
        next_.store(count);
        drain({});
        throw;
    }
}
namespace {
// An address range of the rendered RAM owned by one job. The last range is
// unbounded so stores outside RDRAM still reach the explicit bounds check.
struct Band {
    std::uint64_t low=0,high=UINT64_MAX;
    std::vector<SoftwareResult::Write> writes;
    std::uint64_t pixels=0;
    // First failure in serial order: rectangle, then pre-pixel checks before
    // pixels, then band. One rectangle has one target, whose stores rise with
    // row-major order, so a lower band holds the earlier pixel and each band
    // stops at its own first failing pixel.
    std::exception_ptr error;
    std::tuple<unsigned,unsigned,std::uint64_t> error_key{};
};
void raster_band(const DrawList& list,std::vector<std::uint8_t>& ram,Band& band,
                 const std::function<void()>* checkpoint,std::atomic<unsigned>& first_error) {
    for(unsigned index=0;index<list.rectangles.size();++index) {
        if(index>first_error.load(std::memory_order_relaxed))return;
        if(checkpoint && *checkpoint)(*checkpoint)();
        const auto& r=list.rectangles[index];
        unsigned phase=0;
        try {
        if((r.state.other_hi>>20&3)==3) {
            // FILL: the fill word as is, no combine/blend/dither; on a 16-bit
            // target even x takes the high half, odd x the low half.
            const auto [x0,y0,x1,y1]=raster_bounds(r);
            if(y1>1024 || y0>1024)throw std::runtime_error("Raster height exceeds budget");
            phase=1;
            for(int y=y0;y<y1;++y) {
                if(x1<=x0)continue;
                const std::uint64_t row=r.target.address+std::uint64_t(y)*r.target.width*2;
                int bx0=x0,bx1=x1;
                if(band.low && band.low>row)bx0=int(std::min<std::uint64_t>(x1,std::max<std::uint64_t>(x0,(band.low-row+1)/2)));
                if(band.high!=UINT64_MAX) {
                    if(band.high<=row)continue;
                    bx1=int(std::min<std::uint64_t>(x1,(band.high-row+1)/2));
                }
                for(int x=bx0;x<bx1;++x) {
                    const std::uint64_t a=row+std::uint64_t(x)*2;
                    if(a+2>ram.size())throw std::runtime_error("Framebuffer write outside RDRAM");
                    const unsigned v=(x&1) ? (r.state.fill&0xffff) : (r.state.fill>>16);
                    ram[a]=v>>8;ram[a+1]=v&255;
                    if(!band.writes.empty() && std::uint64_t(band.writes.back().address)+band.writes.back().length==a)
                        band.writes.back().length+=2;
                    else band.writes.push_back({std::uint32_t(a),2});
                    ++band.pixels;
                }
            }
            continue;
        }
        if(r.texture>=list.textures.size())throw std::runtime_error("Invalid DrawList texture reference");
        const auto& tex=list.textures[r.texture];unsigned cycle=r.state.other_hi>>20&3;
        if(cycle!=0 && cycle!=2)throw std::runtime_error("Only observed 1-cycle/COPY is implemented");
        if(r.state.other_lo&0x30)throw std::runtime_error("Depth comparison/update needs new evidence");
        // SETTILESIZE is stored unvalidated; reversed limits have no observed
        // hardware meaning and would give address() an inverted clamp range.
        if(!tex.background && (tex.tile.sh<tex.tile.sl || tex.tile.th<tex.tile.tl))
            throw std::runtime_error("Reversed SETTILESIZE limits need new evidence");
        const auto [x0,y0,x1,y1]=raster_bounds(r);
        if(y1>1024 || y0>1024)throw std::runtime_error("Raster height exceeds budget");
        phase=1;
        for(int y=y0;y<y1;++y) {
            if (checkpoint && *checkpoint && (y-y0)%32==0) (*checkpoint)();
            if(x1<=x0)continue;
            // Clip this row to the band: pixel x stores at row+2x.
            const std::uint64_t row=r.target.address+std::uint64_t(y)*r.target.width*2;
            int bx0=x0,bx1=x1;
            if(band.low && band.low>row)bx0=int(std::min<std::uint64_t>(x1,std::max<std::uint64_t>(x0,(band.low-row+1)/2)));
            if(band.high!=UINT64_MAX) {
                if(band.high<=row)continue;
                bx1=int(std::min<std::uint64_t>(x1,(band.high-row+1)/2));
            }
            for(int x=bx0;x<bx1;++x) {
            std::uint64_t a=row+std::uint64_t(x)*2;
            int dx=x-r.x0/4,dy=y-r.y0/4;
            int s=r.s+floor_div(std::int64_t(r.flip?dy:dx)*r.dsdx,32);
            int t=r.t+floor_div(std::int64_t(r.flip?dx:dy)*r.dtdy,32);
            Color c=r.state.combine==0xfcfffffffffdf6fbULL && cycle==0 ? Color{} :
                sample(tex,s,t,cycle==0 && (r.state.other_hi&(1u<<13)),cycle==2);
            if(cycle==0)c=combine(c,r.state);
            if(cycle==0 && (r.state.other_lo&(1u<<12)) && c.a<32)continue;
            if(r.state.other_lo&1) {
                if(cycle==2) {
                    if(!copy_alpha_bit(tex,r,c))continue;
                } else {
                    if(r.state.other_lo&2)throw std::runtime_error("Dithered alpha compare requires deterministic noise state");
                    if(c.a<r.state.blend.a)continue;
                }
            }
            if(a+2>ram.size())throw std::runtime_error("Framebuffer write outside RDRAM");
            auto memory=rgba16((ram[a]<<8)|ram[a+1]);
            // Framebuffer reads zero-extend the five RGB bits; TMEM texture
            // fetches replicate them. These are distinct RDP paths.
            memory.r&=248;memory.g&=248;memory.b&=248;
            if(cycle==0){c=blend(c,memory,r.state);dither(c,r.state.other_hi>>6&3,x,y);}
            // COPY writes the fetched palette alpha bit even when alpha compare
            // is disabled. One-cycle stores coverage; full hidden coverage is D-029.
            if(cycle==0)c.a=255;
            auto v=encode_rgba16(c);ram[a]=v>>8;ram[a+1]=v&255;
            // Retain actual stores, including equal-value stores; alpha rejects
            // produce no write. Native completion commits only these bytes.
            if (!band.writes.empty() && std::uint64_t(band.writes.back().address)
                +band.writes.back().length==a) band.writes.back().length+=2;
            else band.writes.push_back({std::uint32_t(a),2});
            ++band.pixels;
            }
        }
        } catch(...) {
            band.error=std::current_exception();band.error_key={index,phase,band.low};
            unsigned current=first_error.load();
            while(index<current && !first_error.compare_exchange_weak(current,index)) {}
            return;
        }
    }
}
}
SoftwareResult render_software(const DrawList& list, const std::function<void()>& checkpoint, RasterPool* pool) {
    SoftwareResult result{list.initial_ram,0,{}};
    std::atomic<unsigned> first_error{UINT_MAX};
    // Parallel ranges need pixel-aligned (even) targets so no two ranges share
    // a byte, and enough work to outweigh dispatch.
    std::uint64_t candidates=0,low=UINT64_MAX,high=0;bool aligned=true;
    for(const auto& r:list.rectangles) {
        const auto b=raster_bounds(r);
        if(b.empty())continue;
        aligned=aligned && r.target.address%2==0;
        candidates+=std::uint64_t(b.x1-b.x0)*std::uint64_t(b.y1-b.y0);
        low=std::min(low,r.target.address+(std::uint64_t(b.y0)*r.target.width+b.x0)*2);
        high=std::max(high,r.target.address+(std::uint64_t(b.y1-1)*r.target.width+b.x1)*2);
    }
    std::vector<Band> bands;
    if(pool && pool->helpers() && aligned && candidates>=parallel_raster_min_pixels) {
        const std::uint64_t count=4*(pool->helpers()+1),span=high-low;
        bands.resize(count);
        for(std::uint64_t i=1;i<count;++i)
            bands[i-1].high=bands[i].low=(low+span*i/count)&~std::uint64_t(1);
        pool->run(unsigned(count),[&](unsigned i){raster_band(list,result.ram,bands[i],nullptr,first_error);},checkpoint);
    } else {
        bands.resize(1);
        raster_band(list,result.ram,bands[0],&checkpoint,first_error);
    }
    const Band* failed=nullptr;
    for(const auto& band:bands)
        if(band.error && (!failed || band.error_key<failed->error_key))failed=&band;
    if(failed)std::rethrow_exception(failed->error);
    for(auto& band:bands) {
        result.written_pixels+=band.pixels;
        result.writes.insert(result.writes.end(),band.writes.begin(),band.writes.end());
    }
    const auto by_address=[](auto x,auto y){return x.address<y.address;};
    if(!std::is_sorted(result.writes.begin(),result.writes.end(),by_address))
        std::sort(result.writes.begin(),result.writes.end(),by_address);
    std::vector<SoftwareResult::Write> merged;
    merged.reserve(result.writes.size());
    for(const auto& w:result.writes) {
        if(!merged.empty() && std::uint64_t(merged.back().address)+merged.back().length>=w.address) {
            const auto end=std::max<std::uint64_t>(std::uint64_t(merged.back().address)+merged.back().length,
                                                  std::uint64_t(w.address)+w.length);
            merged.back().length=std::uint32_t(end-merged.back().address);
        } else merged.push_back(w);
    }
    result.writes=std::move(merged);
    return result;
}
}
