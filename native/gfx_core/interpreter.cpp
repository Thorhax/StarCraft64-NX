#include "gfx.hpp"
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace sc64::gfx {
namespace {
enum class ImageAddresses { Physical, Segmented };
// Decoded S2DEX backgrounds reused across frames. An entry is reused only when
// every decode input is byte-identical (source image bytes, TLUT/TMEM bytes for
// CI, geometry, palette and TLUT mode), so the pixels and their content key are
// exactly what a fresh decode would produce. Menus redraw 640x480 backgrounds
// every frame; decoding and hashing them per texel dominated menu frame time.
struct BackgroundCacheEntry {
    Image image; unsigned height = 0, palette = 0, mode = 0;
    std::vector<std::uint8_t> source;
    std::array<std::uint8_t,4096> tmem{};
    std::vector<Color> pixels;
    std::uint64_t key = 0;
};
std::vector<BackgroundCacheEntry>& background_cache() {
    thread_local std::vector<BackgroundCacheEntry> cache;
    return cache;
}
class Interpreter {
public:
    DrawList out;
    Tmem tmem;
    std::array<Tile,8> tiles{};
    std::array<unsigned,16> segments{};
    Image texture_image{},target{};
    State state;
    unsigned pc=0;
    ImageAddresses image_addresses;
    std::function<void()> checkpoint;
    std::uint64_t texture_bytes=0;
    // Takes the snapshot by value: callers that own one move it in (no 8 MiB copy).
    Interpreter(std::vector<std::uint8_t> ram,ImageAddresses addresses,
                const std::function<void()>& poll = {}):image_addresses(addresses),checkpoint(poll) {
        if(ram.size()!=0x800000)throw std::runtime_error("Expected canonical 8 MiB RDRAM");
        out.initial_ram=std::move(ram);
        out.limitations.push_back("D-029: RGB target reference only; hidden coverage and VI are not bit exact");
    }
    void poll() const { if(checkpoint)checkpoint(); }
    [[noreturn]] void fail(const std::string& reason,std::uint32_t high=0,std::uint32_t low=0) const {
        std::ostringstream s;s<<reason<<" at PC 0x"<<std::hex<<pc<<" words "<<high<<' '<<low;
        throw std::runtime_error(s.str());
    }
    void bounds(unsigned a,unsigned n) const {
        if(a>out.initial_ram.size() || n>out.initial_ram.size()-a)fail("RDRAM range exceeds snapshot");
    }
    void input_range(unsigned a,unsigned n) const {
        bounds(a,n);
        // Interpretation snapshots loads before rasterization. A later load of
        // earlier target writes would need an ordered feedback executor, so fail
        // at that dependency instead of reading stale pre-task bytes (D-019).
        std::uint64_t end=std::uint64_t(a)+n;
        for(const auto& r:out.rectangles) {
            poll();
            const auto [x0,y0,x1,y1]=raster_bounds(r);
            if(x1<=x0 || y1<=y0)continue;
            std::uint64_t start=r.target.address;
            // Every row span lies inside [first row start, last row end), so
            // a range outside that hull cannot overlap any row span.
            auto first=start+(std::uint64_t(y0)*r.target.width+x0)*2;
            auto last=start+(std::uint64_t(y1-1)*r.target.width+x1)*2;
            if(a>=last || end<=first)continue;
            for(int y=y0;y<y1;++y) {
                auto low=start+(std::uint64_t(y)*r.target.width+x0)*2;
                auto high=start+(std::uint64_t(y)*r.target.width+x1)*2;
                if(a<high && end>low)fail("Texture load depends on earlier framebuffer writes (D-019)");
            }
        }
    }
    unsigned half(unsigned a) const {bounds(a,2);return (out.initial_ram[a]<<8)|out.initial_ram[a+1];}
    std::uint32_t word(unsigned a) const {bounds(a,4);return (half(a)<<16)|half(a+2);}
    unsigned resolve(unsigned a) const {
        if(a&0x80000000)return a&0x1fffffff;
        unsigned seg=a>>24;if(seg>=16)fail("Invalid segmented address");
        std::uint64_t value=std::uint64_t(segments[seg])+(a&0xffffff);
        if(value>0x1fffffff)fail("Segment address overflow");return value;
    }
    unsigned image_address(unsigned a) const {
        // RSP resolves GBI pointers before emitting RDP commands. Raw RDP
        // images already contain physical addresses and must skip segmentation.
        return image_addresses==ImageAddresses::Segmented?resolve(a):a&0xffffff;
    }
    static Color color(unsigned v) {return {int(v>>24),int(v>>16&255),int(v>>8&255),int(v&255)};}
    unsigned store(Texture tex) {
        tex.key=texture_key(tex);
        return insert(std::move(tex));
    }
    // Same as store() for a texture whose key is already known exactly.
    unsigned store_known(Texture tex,std::uint64_t key) {
        tex.key=key;
        return insert(std::move(tex));
    }
    std::uint64_t texture_key(const Texture& tex) const {
        // FNV identifies candidates only. Equal keys are checked against the full
        // semantic payload, so a collision cannot alias a different resource.
        std::uint64_t hash=14695981039346656037ull;
        auto add=[&](std::uint64_t v){for(unsigned j=0;j<8;++j){hash^=(v>>(j*8))&255;hash*=1099511628211ull;}};
        constexpr std::uint64_t byte_multiplier=[] {
            std::uint64_t value=1;for(unsigned j=0;j<8;++j)value*=1099511628211ull;return value;
        }();
        // The key's historical encoding uses eight bytes even for a channel.
        // Its seven zero bytes reduce to multiplication by FNV_prime^8 modulo
        // 2^64. Keep that encoding exactly, rather than assigning new keys.
        auto add_byte=[&](std::uint8_t value){hash=(hash^value)*byte_multiplier;};
        add(tex.width);add(tex.height);add(tex.background);add(tex.tlut_mode);
        if(!tex.background)for(unsigned i=0;i<tex.memory.bytes.size();++i) {
            if((i&511)==0)poll();
            add_byte(tex.memory.bytes[i]);
        }
        const auto& t=tex.tile;
        for(auto v:{t.format,t.size,t.line,t.tmem,t.palette,t.cms,t.cmt,t.masks,t.maskt,t.shifts,t.shiftt,
                    unsigned(t.sl),unsigned(t.tl),unsigned(t.sh),unsigned(t.th)})add(v);
        // Decoded payload includes every TLUT entry actually sampled by the draw.
        for(unsigned i=0;i<tex.pixels.size();++i) {
            if((i&255)==0)poll();
            // All pixels here come from the bounded texture decoders, whose
            // components are 0..255; callers cannot supply arbitrary Colors.
            auto c=tex.pixels[i];for(int v:{c.r,c.g,c.b,c.a})add_byte(std::uint8_t(v));
        }
        return hash;
    }
    unsigned insert(Texture tex) {
        const std::uint64_t hash=tex.key;
        for(unsigned i=0;i<out.textures.size();++i)if(out.textures[i].key==hash) {
            const auto& old=out.textures[i];
            if(old.tile==tex.tile && old.width==tex.width && old.height==tex.height &&
               old.background==tex.background && old.pixels==tex.pixels &&
               old.tlut_mode==tex.tlut_mode && old.memory.bytes==tex.memory.bytes)return i;
            fail("Texture content key collision");
        }
        // Running total of every stored texture (was re-summed per store: O(n^2)).
        const std::uint64_t allocated=texture_bytes+tex.pixels.size()*sizeof(Color)+4096;
        if(allocated>64*1024*1024)fail("Owned texture payload exceeds 64 MiB");
        texture_bytes=allocated;
        out.textures.push_back(std::move(tex));return out.textures.size()-1;
    }
    unsigned snapshot(unsigned index) {
        const auto& tile=tiles[index];
        if(state.combine==0xfcfffffffffdf6fbULL) {
            Texture tex;tex.width=tex.height=1;tex.background=true;tex.pixels.push_back({255,255,255,255});
            return store(std::move(tex));
        }
        Texture tex;tex.width=1024;tex.height=256;tex.tile=tile;
        tex.memory=tmem;tex.tlut_mode=(state.other_hi>>14)&3;
        return store(std::move(tex));
    }
    void emit(Rectangle rect) {
        if(target.width==0 || target.width>1024 || target.format!=0 || target.size!=2)
            fail("Unsupported or unset color target");
        if(state.scissor.field!=0)fail("Interlaced RDP scissor requires new evidence");
        rect.target=target;rect.state=state;rect.source_pc=pc;
        out.rectangles.push_back(std::move(rect));
        if(out.rectangles.size()>100000)fail("Rectangle budget exceeded");
    }
    void rdp(unsigned hi,unsigned lo,std::uint64_t params=0) {
        unsigned op=(hi>>24)&63,index=(lo>>24)&7;
        switch(op) {
        case 0:case 0x26:case 0x27:case 0x28:case 0x29:break;
        case 0x2d:
            state.scissor={int(hi>>12&4095),int(hi&4095),int(lo>>12&4095),int(lo&4095),(lo>>24)&3};break;
        case 0x2f:state.other_hi=hi&0xffffff;state.other_lo=lo;break;
        case 0x35:{
            auto& t=tiles[index];t.format=hi>>21&7;t.size=hi>>19&3;t.line=hi>>9&511;t.tmem=hi&511;
            t.palette=lo>>20&15;t.cmt=lo>>18&3;t.maskt=lo>>14&15;t.shiftt=lo>>10&15;
            t.cms=lo>>8&3;t.masks=lo>>4&15;t.shifts=lo&15;break;
        }
        case 0x32:{auto& t=tiles[index];t.sl=hi>>12&4095;t.tl=hi&4095;t.sh=lo>>12&4095;t.th=lo&4095;break;}
        case 0x34:{
            unsigned sl=hi>>12&4095,tl=hi&4095,sh=lo>>12&4095,th=lo&4095;
            if(sh<sl || th<tl)fail("Reversed LOADTILE limits",hi,lo);
            std::uint64_t first=std::uint64_t(tl/4)*texture_image.width+sl/4;
            std::uint64_t last=std::uint64_t(th/4)*texture_image.width+sh/4+1;
            unsigned bits=4u<<texture_image.size;
            auto start=std::uint64_t(texture_image.address)+first*bits/8;
            auto end=std::uint64_t(texture_image.address)+(last*bits+7)/8;
            if(end>out.initial_ram.size())fail("LOADTILE source exceeds snapshot",hi,lo);
            input_range(start,end-start);
            tmem.load_tile(out.initial_ram,texture_image,tiles[index],sl,tl,sh,th);break;
        }
        case 0x30:{
            // The load edge walker spans integer columns sl>>2..sh>>2 on a
            // single row (angrylion tex.c tile_tlut_common_cs_decoder and
            // loading_pipeline); a multi-row TLUT load crashes the pipeline.
            unsigned sl=hi>>12&4095,tl=hi&4095,sh=lo>>12&4095,th=lo&4095;
            if((sh>>2)<(sl>>2))fail("Reversed LOADTLUT limits",hi,lo);
            if((th>>2)!=(tl>>2))fail("Multi-row LOADTLUT crashes the RDP pipeline",hi,lo);
            unsigned count=(sh>>2)-(sl>>2)+1;
            if(count>256)fail("LOADTLUT exceeds 256 entries",hi,lo);
            input_range(texture_image.address+((tl>>2)*texture_image.width+(sl>>2))*2,count*2);
            tmem.load_tlut(out.initial_ram,texture_image,tiles[index].tmem,count,sl,tl);break;
        }
        case 0x3d:texture_image={image_address(lo),(hi&1023)+1,hi>>21&7,hi>>19&3};break;
        case 0x3f:target={image_address(lo),(hi&1023)+1,hi>>21&7,hi>>19&3};break;
        case 0x3c:state.combine=(std::uint64_t(hi)<<32)|lo;break;
        case 0x38:state.fog=color(lo);break;
        case 0x39:state.blend=color(lo);break;
        case 0x37:state.fill=lo;break;
        case 0x36:{
            // FILLRECTANGLE (Brood War loading screen clears). FILL mode stores
            // the fill word directly; like COPY it includes the lower/right
            // integer pixels (angrylion rdp_fill_rect / edge walker in FILL).
            unsigned cycle=state.other_hi>>20&3;
            if(cycle!=3)fail("FILLRECTANGLE outside FILL mode needs evidence",hi,lo);
            Rectangle r;r.x0=lo>>12&4095;r.y0=lo&4095;r.x1=(hi>>12&4095|3)+1;r.y1=(hi&4095|3)+1;
            r.texture=no_texture;emit(r);break;
        }
        case 0x3a:state.primitive=color(lo);break;
        case 0x3b:state.environment=color(lo);break;
        case 0x24:case 0x25:{
            unsigned cycle=state.other_hi>>20&3;
            if(cycle!=0 && cycle!=2)fail("Unobserved cycle mode",hi,lo);
            Rectangle r;r.x0=lo>>12&4095;r.y0=lo&4095;r.x1=hi>>12&4095;r.y1=hi&4095;
            if((r.x0&3) || (r.y0&3) ||
               (cycle==0 && ((r.x1&3) || (r.y1&3))) ||
               (cycle==2 && ((r.x1&3)!=3 || (r.y1&3)!=3)))
                fail("Unobserved fractional rectangle edges",hi,lo);
            // COPY includes lower/right integer pixels; 1-cycle ends remain
            // exclusive. Angrylion rdp_tex_rect only ORs Y with 3 for COPY/FILL.
            if(cycle==2) {r.x1=(r.x1|3)+1;r.y1=(r.y1|3)+1;}
            r.s=std::int16_t(params>>48);r.t=std::int16_t(params>>32);
            r.dsdx=std::int16_t(params>>16);r.dtdy=std::int16_t(params);
            if(cycle==2)r.dsdx/=4;
            r.flip=op==0x25;r.texture=snapshot(index);emit(r);break;
        }
        default:fail("Unsupported RDP operation",hi,lo);
        }
    }
    void background(unsigned descriptor,bool copy) {
        bounds(descriptor,40);
        unsigned width=half(descriptor+2)/4,height=half(descriptor+10)/4;
        unsigned format=out.initial_ram[descriptor+22],size=out.initial_ram[descriptor+23];
        // These descriptor fields are full bytes, not the bounded bitfields of
        // SETTIMG. Reject unknown sizes before computing bits per source texel.
        if(size>3 || !((format==0 && size>=2) || (format==2 && size<=1) ||
                      (format==3 && size<=2) || (format==4 && size<=1)))
            fail("Unsupported S2DEX background format/size");
        if(!width || !height || width>1024 || height>1024)fail("Invalid S2DEX background dimensions");
        unsigned load=half(descriptor+20),flip=half(descriptor+26);
        if(load!=0xfff4)fail("Unobserved background load method");
        if(flip)fail("Unobserved background flip flags");
        if(!copy && word(descriptor+32)!=0)
            fail("Unobserved background imageYorig phase");
        Image img{resolve(word(descriptor+16)),width,format,size};
        input_range(img.address,(std::uint64_t(width)*height*(4u<<size)+7)/8);
        Texture tex;tex.width=width;tex.height=height;tex.background=true;
        tex.tile.format=format;tex.tile.size=size;tex.tile.palette=half(descriptor+24);
        const unsigned mode=(state.other_hi>>14)&3;
        const std::size_t source_size=std::size_t((std::uint64_t(width)*height*(4u<<size)+7)/8);
        const auto source=std::span<const std::uint8_t>(out.initial_ram).subspan(img.address,source_size);
        auto& cache=background_cache();
        const BackgroundCacheEntry* hit=nullptr;
        for(const auto& e:cache)
            if(e.image==img && e.height==height && e.palette==tex.tile.palette && e.mode==mode
               && std::equal(e.source.begin(),e.source.end(),source.begin(),source.end())
               && (format!=2 || e.tmem==tmem.bytes)) {hit=&e;break;}
        unsigned texture_index;
        if(hit) {
            tex.pixels=hit->pixels;
            texture_index=store_known(std::move(tex),hit->key);
        } else {
            tex.pixels.reserve(width*height);
            for(unsigned y=0;y<height;++y) {
                poll();
                for(unsigned x=0;x<width;++x)
                    tex.pixels.push_back(source_texel(out.initial_ram,img,x,y,tex.tile.palette,tmem,mode));
            }
            texture_index=store(std::move(tex));
            BackgroundCacheEntry entry;
            entry.image=img;entry.height=height;entry.palette=out.textures[texture_index].tile.palette;entry.mode=mode;
            entry.source.assign(source.begin(),source.end());
            if(format==2)entry.tmem=tmem.bytes;
            entry.pixels=out.textures[texture_index].pixels;
            entry.key=out.textures[texture_index].key;
            // Menus draw ~17 background pieces per frame, briefings ~8: a 4-entry
            // FIFO evicted every piece before its next use (profile: 0 hits).
            if(cache.size()>=64)cache.erase(cache.begin());
            cache.push_back(std::move(entry));
        }
        Rectangle r;r.texture=texture_index;
        r.x0=std::int16_t(half(descriptor+4));r.y0=std::int16_t(half(descriptor+12));
        r.x1=r.x0+half(descriptor+6);r.y1=r.y0+half(descriptor+14);
        if((r.x0&3) || (r.y0&3) || (r.x1&3) || (r.y1&3))
            fail("Unobserved fractional S2DEX frame edges");
        r.s=half(descriptor);r.t=half(descriptor+8);
        r.dsdx=copy?1024:half(descriptor+28);r.dtdy=copy?1024:half(descriptor+30);
        if(!r.dsdx || !r.dtdy)fail("Zero background scale");
        if(!copy) {
            // Scalable S2DEX leaves one source texel for the filtering border.
            // SDK us2dex_emu.c derives these exclusive integer frame maxima.
            int wmax=((int(half(descriptor+2))*1024/r.dsdx)-1)&~3;
            int hmax=((int(half(descriptor+10))*1024/r.dtdy)-1)&~3;
            int w=std::min(int(half(descriptor+6)),wmax);
            int h=std::min(int(half(descriptor+14)),hmax);
            r.x1=r.x0+w;r.y1=r.y0+h;
        }
        auto saved=state;
        // BG_COPY's emitted RDP work switches cycle to COPY, then the next GBI
        // mode command re-establishes caller state. Do not add a CPU overlay.
        if(copy)state.other_hi=(state.other_hi&~(3u<<20))|(2u<<20);
        emit(r);state=saved;
    }
    void object_texture(unsigned a) {
        bounds(a,24);
        if(word(a)!=0x30)fail("Unobserved S2DEX object texture type",word(a));
        unsigned sid=half(a+14);
        if(sid>12 || sid%4)fail("Invalid S2DEX texture state id");
        // Load each descriptor even if its flag happens to match. Only TLUT
        // descriptors were observed; repeated identical loads are idempotent.
        Image img{resolve(word(a+4)),1,0,2};
        input_range(img.address,(half(a+10)+1)*2);
        tmem.load_tlut(out.initial_ram,img,half(a+8),half(a+10)+1);
    }
};
}
DrawList interpret_rdp(std::span<const std::uint8_t> ram,std::span<const std::uint64_t> words) {
    Interpreter parser(std::vector<std::uint8_t>(ram.begin(),ram.end()),ImageAddresses::Physical);
    for(unsigned i=0;i<words.size();++i) {
        parser.pc=i*8;auto v=words[i];unsigned op=(v>>56)&63;
        parser.out.opcodes[op]++;
        std::uint64_t param=0;
        if(op==0x24 || op==0x25) {if(++i>=words.size())parser.fail("Truncated RDP rectangle");param=words[i];}
        parser.rdp(v>>32,v,param);
    }
    return std::move(parser.out);
}
DrawList interpret_gbi(std::span<const std::uint8_t> ram,unsigned root,const std::function<void()>& checkpoint) {
    return interpret_gbi(std::vector<std::uint8_t>(ram.begin(),ram.end()),root,checkpoint);
}
DrawList interpret_gbi(std::vector<std::uint8_t>&& ram,unsigned root,const std::function<void()>& checkpoint) {
    Interpreter parser(std::move(ram),ImageAddresses::Segmented,checkpoint);
    unsigned pc=root&0x1fffffff;std::vector<unsigned> stack;
    for(unsigned steps=0;steps<100000;++steps) {
        parser.poll();
        parser.pc=pc;if(pc&7)parser.fail("Misaligned display-list PC");
        auto hi=parser.word(pc),lo=parser.word(pc+4);unsigned op=hi>>24;
        pc+=8;parser.out.opcodes[op]++;
        switch(op) {
        case 0xdb:{
            if((hi>>16&255)!=6 || (hi&3))parser.fail("Unobserved MOVEWORD",hi,lo);
            unsigned index=(hi&65535)/4;if(index>=16)parser.fail("Invalid segment index",hi,lo);
            parser.segments[index]=lo&0x1fffffff;break;
        }
        case 0xde:
            if((hi>>16&255)==0){stack.push_back(pc);if(stack.size()>32)parser.fail("Display-list stack exceeds 32");}
            else if((hi>>16&255)!=1)parser.fail("Invalid DL push flag",hi,lo);
            pc=parser.resolve(lo);break;
        case 0xdf:
            if(stack.empty())return std::move(parser.out);
            pc=stack.back();stack.pop_back();break;
        case 0x09:case 0x0a:parser.background(parser.resolve(lo),op==0x0a);break;
        case 0x05:parser.object_texture(parser.resolve(lo));break;
        case 0xe2:case 0xe3:{
            unsigned n=(hi&255)+1,encoded_shift=hi>>8&255;
            if(n>32 || encoded_shift+n>32)parser.fail("Invalid othermode mask",hi,lo);
            unsigned shift=32-encoded_shift-n;
            std::uint32_t mask=n==32?0xffffffffu:((1u<<n)-1)<<shift;
            auto& value=op==0xe2?parser.state.other_lo:parser.state.other_hi;
            value=(value&~mask)|(lo&mask);break;
        }
        case 0xe4:case 0xe5:{
            auto p1=parser.word(pc),p2=parser.word(pc+8);
            if(p1!=0xe1000000 || p2!=0xf1000000)parser.fail("Missing rectangle half packets",p1,p2);
            std::uint64_t params=(std::uint64_t(parser.word(pc+4))<<32)|parser.word(pc+12);
            parser.out.opcodes[0xe1]++;parser.out.opcodes[0xf1]++;pc+=16;
            parser.rdp(hi,lo,params);break;
        }
        default:
            if(op<0xe6)parser.fail("Unsupported GBI operation",hi,lo);
            parser.rdp(hi,lo);break;
        }
    }
    parser.fail("Display-list instruction budget exceeded");
}
}
