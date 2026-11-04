#include "gfx.hpp"
#include <cstdlib>
#include <iostream>
#include <random>
#include <thread>
#include <stdexcept>
using namespace sc64::gfx;
void require(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
template<typename F> void rejects(F f) {bool rejected=false;try{f();}catch(const std::runtime_error&){rejected=true;}require(rejected,"Malformed input accepted");}
void put(std::vector<std::uint8_t>& ram,unsigned a,std::uint32_t v) {
    for(unsigned i=0;i<4;++i)ram.at(a+i)=v>>(24-i*8);
}
void command(std::vector<std::uint8_t>& ram,unsigned& a,unsigned hi,unsigned lo) {put(ram,a,hi);put(ram,a+4,lo);a+=8;}
void tmem_fixtures() {
    std::vector<std::uint8_t> ram(0x800000);
    Tmem memory;Tile t;t.format=4;t.size=1;t.line=1;t.tmem=2;
    for(unsigned i=0;i<16;++i)ram[0x100+i]=i+10;
    memory.load_tile(ram,{0x100,8,4,1},t,0,0,28,4);
    require(memory.bytes[16]==10 && memory.bytes[16+8+4]==18,"Odd row physical TMEM swap");
    require(memory.fetch(t,0,1,0)==Color{18,18,18,18},"Odd row fetch");
    require(memory.fetch(t,7,1,0).r==25,"Line stride last texel");
    // Partial source load starts at x2,y1; image stride differs from tile stride.
    memory.load_tile(ram,{0x100,8,4,1},t,8,4,20,4);
    require(memory.fetch(t,0,0,0).r==20 && memory.fetch(t,3,0,0).r==23,"LOADTILE source stride/subregion");
    ram[0x200]=0x12;ram[0x201]=0x34;
    Tile ci;ci.format=2;ci.size=0;ci.line=1;ci.palette=1;
    memory.load_tile(ram,{0x200,4,2,0},ci,0,0,12,0);
    ram[0x301]=0x01;ram[0x300]=0xf8; // opaque red
    memory.load_tlut(ram,{0x300,1,0,2},273,1);
    require(memory.bytes[273*8]==0xf8 && memory.bytes[273*8+6]==0xf8,"Four-bank TLUT replication");
    require(memory.fetch(ci,0,0,2)==Color{255,0,0,255},"CI4 palette high nibble");
    ram[0x300]=0x07;ram[0x301]=0xc1;
    memory.load_tlut(ram,{0x300,1,0,2},273,1);
    require(memory.fetch(ci,0,0,2)==Color{0,255,0,255},"Single-entry palette update invalidates color");
    ci.size=1;ram[0x200]=17;memory.load_tile(ram,{0x200,1,2,1},ci,0,0,0,0);
    require(memory.fetch(ci,0,0,2).g==255,"CI8 palette index");
    ram[0x300]=0x80;ram[0x301]=0x40;memory.load_tlut(ram,{0x300,1,0,2},273,1);
    require(memory.fetch(ci,0,0,3)==Color{128,128,128,64},"IA16 TLUT");
    ci.tmem=256;
    require(memory.fetch(ci,0,0,3)==Color{128,128,128,64},"CI/TLUT source fetch wraps within lower 2 KiB bank");
    ci.tmem=0;
    for(auto pair:{std::pair{3u,0u},std::pair{3u,1u},std::pair{3u,2u},std::pair{4u,0u}}) {
        Tile q;q.format=pair.first;q.size=pair.second;q.line=1;
        ram[0x400]=q.size==0?0xf0:q.size==1?0x82:0x80;ram[0x401]=0x40;
        memory.load_tile(ram,{0x400,1,q.format,q.size},q,0,0,0,0);
        Color expected=q.format==4?Color{255,255,255,255}:q.size==0?Color{255,255,255,255}:
            q.size==1?Color{136,136,136,34}:Color{128,128,128,64};
        require(memory.fetch(q,0,0,0)==expected,"IA/I independent decode fixture");
    }
    Tile rgba;rgba.format=0;rgba.size=3;rgba.line=1;
    ram[0x500]=0x12;ram[0x501]=0x34;ram[0x502]=0x56;ram[0x503]=0x78;
    memory.load_tile(ram,{0x500,1,0,3},rgba,0,0,0,0);
    require(memory.bytes[0]==0x12 && memory.bytes[2048]==0x56,"RGBA32 split bank layout");
    require(memory.fetch(rgba,0,0,0)==Color{18,52,86,120},"RGBA32 decode");
    require(rgba16(0xf801)==Color{255,0,0,255},"RGBA16 component expansion");
    rejects([&]{memory.load_tile(ram,{0x7fffff,1,0,2},rgba,0,0,0,0);});
    rejects([&]{memory.load_tlut(ram,{0,1,0,2},511,2);});
}
void negative_nibble_fixtures() {
    Tmem memory;Tile tile;tile.format=4;tile.size=0;
    memory.bytes[0]=0x12;memory.bytes[4095]=0xab;memory.bytes[4094]=0xcd;
    for(auto [s,expected]:{std::pair{-1,187},std::pair{-2,170},std::pair{-3,221},
                           std::pair{-4,204},std::pair{0,17},std::pair{1,34}})
        require(memory.fetch(tile,s,0,0)==Color{expected,expected,expected,expected},
                "I4 negative odd/even TMEM wrap");
    memory.bytes[4091]=0xef;
    require(memory.fetch(tile,-1,1,0).r==255,"Negative I4 fetch respects odd-row swap");
    tile.format=3;
    require(memory.fetch(tile,-1,0,0)==Color{182,182,182,255},"Negative IA4 nibble alpha");
    require(memory.fetch(tile,-2,0,0)==Color{182,182,182,0},"Negative IA4 even nibble");
    tile.format=2;memory.bytes[2047]=0xab;
    std::vector<std::uint8_t> ram(4);ram[0]=0xf8;ram[1]=1;ram[2]=7;ram[3]=0xc1;
    memory.load_tlut(ram,{0,2,0,2},266,2);
    require(memory.fetch(tile,-1,0,2)==Color{0,255,0,255},"Negative CI4 lower-bank wrap");
    require(memory.fetch(tile,-2,0,2)==Color{255,0,0,255},"Negative CI4 even nibble");
}
void image_address_fixtures() {
    std::vector<std::uint8_t> ram(0x800000);unsigned a=0x1000;
    command(ram,a,0xdb060004,0x20000);
    command(ram,a,0xff100003,0x01004000);
    command(ram,a,0xed000000,0x10010);command(ram,a,0xef800cff,0);
    command(ram,a,0xfcffffff,0xfffdf6fb);command(ram,a,0xfa000000,0xf80000ff);
    command(ram,a,0xe400c00c,0);command(ram,a,0xe1000000,0);command(ram,a,0xf1000000,0);
    command(ram,a,0xdf000000,0);
    auto gbi=interpret_gbi(ram,0x1000);
    require(gbi.rectangles[0].target.address==0x24000,"GBI color image resolves segment base");
    auto rendered=render_software(gbi);
    require(rendered.ram[0x24000]==0xf8 && rendered.ram[0x24001]==1,"Segmented target receives pixel stores");
    require(rendered.ram[0x4000]==0 && rendered.ram[0x4001]==0,"Unresolved target remains untouched");
    std::array<std::uint64_t,8> rdp={0xff10000300024000ULL,0xed00000000010010ULL,
        0xef800cff00000000ULL,0xfcfffffffffdf6fbULL,0xfa000000f80000ffULL,
        0xe400c00c00000000ULL,0,0xe900000000000000ULL};
    auto physical=interpret_rdp(ram,rdp);
    require(physical.rectangles[0].target.address==0x24000,"Raw RDP color image stays physical");
    require(render_software(physical).ram==rendered.ram,"GBI/raw RDP target writes agree");
}
void background_descriptor_fixtures() {
    std::vector<std::uint8_t> ram(0x800000);
    put(ram,0x3100,16);put(ram,0x3104,16);put(ram,0x3108,4);put(ram,0x310c,4);
    put(ram,0x3110,0x5000);put(ram,0x311c,0x04000400);
    for(unsigned op:{0x09u,0x0au})for(auto [format,size]:{
            std::pair{2u,4u},std::pair{2u,31u},std::pair{2u,32u},std::pair{2u,255u},
            std::pair{1u,2u},std::pair{7u,2u},std::pair{0u,1u},std::pair{4u,2u}}) {
        unsigned a=0x1000;
        command(ram,a,0xff100003,0x4000);command(ram,a,op<<24,0x3100);command(ram,a,0xdf000000,0);
        put(ram,0x3114,0xfff40000|(format<<8)|size);
        bool rejected=false;
        try {interpret_gbi(ram,0x1000);}
        catch(const std::runtime_error& e) {
            rejected=std::string(e.what()).find("Unsupported S2DEX background format/size")!=std::string::npos;
        }
        require(rejected,"Invalid background format/size must fail before source arithmetic");
    }
}
void gbi_fixtures() {
    std::vector<std::uint8_t> ram(0x800000);unsigned a=0x1000;
    // A primitive-only rectangle must never attempt to sample stale tile state.
    command(ram,a,0xff100003,0x4000); // RGBA16 target width4
    command(ram,a,0xed000000,0x00010010); // scissor 4x4
    command(ram,a,0xef800cff,0); // no dither, no blend
    command(ram,a,0xfcffffff,0xfffdf6fb);
    command(ram,a,0xfa000000,0xf80000ff);
    command(ram,a,0xe400c00c,0);
    command(ram,a,0xe1000000,0);
    command(ram,a,0xf1000000,0);
    command(ram,a,0xdf000000,0);
    auto list=interpret_gbi(ram,0x1000);
    require(list.rectangles.size()==1,"GBI rectangle packet assembly");
    put(ram,0x4000,0xffffffff); // owned initial memory must survive caller reuse
    DrawList batched=list;batched.rectangles.push_back(list.rectangles[0]);
    batched.rectangles.push_back(list.rectangles[0]);
    batched.rectangles.back().state.primitive.g=16;
    auto batches=adjacent_batches(batched);
    require(batches.size()==2 && batches[0].count==2 && batches[1].first==2,"Adjacent batching respects state barriers");
    auto image=render_software(list);
    require(image.written_pixels==9,"One-cycle exclusive rectangle edges");
    require(image.ram[0x4000]==0xf8 && image.ram[0x4001]==1,"Primitive RGB output");
    require(image.ram[0x4006]==0,"Right edge untouched");
    auto bad=ram;put(bad,0x1028,0xe4ff0000);put(bad,0x1030,0);
    rejects([&]{interpret_gbi(bad,0x1000);});
    rejects([&]{interpret_gbi(ram,0x7ffffc);});
    put(bad,0x1000,0xde010000);put(bad,0x1004,0x1000);
    rejects([&]{interpret_gbi(bad,0x1000);});
    // Segmented nested DL, guest-endian descriptor reads, in-place CI8→RGBA16.
    std::fill(ram.begin(),ram.end(),0);a=0x1000;
    command(ram,a,0xdb060004,0x2000);
    command(ram,a,0xde000000,0x01000000);
    command(ram,a,0xdf000000,0);
    a=0x2000;command(ram,a,0xff100003,0x4000);command(ram,a,0xed000000,0x10004);
    command(ram,a,0xef808cff,0);command(ram,a,0x05000000,0x3000);
    command(ram,a,0x0a000000,0x3100);command(ram,a,0xdf000000,0);
    put(ram,0x3000,0x30);put(ram,0x3004,0x3200);put(ram,0x3008,0x01000001);
    ram[0x3200]=0;ram[0x3201]=0;ram[0x3202]=0xf8;ram[0x3203]=1;
    put(ram,0x3100,16);put(ram,0x3104,16);put(ram,0x3108,4);put(ram,0x310c,4);
    put(ram,0x3110,0x4004);put(ram,0x3114,0xfff40201);
    ram[0x4004]=1;ram[0x4005]=0;ram[0x4006]=1;ram[0x4007]=0;
    auto alias=interpret_gbi(ram,0x1000);auto expanded=render_software(alias);
    require(expanded.written_pixels==4,"BG_COPY frame size");
    require(expanded.ram[0x4003]==0,"COPY alpha-zero palette pixels still overwrite target bytes");
    require(expanded.ram[0x4000]==0xf8 && expanded.ram[0x4004]==0xf8,"Aliased CPU image expanded in order");
    // An additional palette load from the earlier framebuffer is real feedback,
    // unlike the source/output alias of the BG_COPY itself.
    put(ram,a-8,0x05000000);put(ram,a-4,0x3300);
    command(ram,a,0xdf000000,0);
    put(ram,0x3300,0x30);put(ram,0x3304,0x4000);put(ram,0x3308,0x01000000);
    rejects([&]{interpret_gbi(ram,0x1000);});

}
void interpreter_checkpoint_fixtures() {
    std::vector<std::uint8_t> ram(0x800000);unsigned a=0x1000;
    command(ram,a,0xff10003f,0x20000);
    command(ram,a,0xed000000,0x00100100);
    command(ram,a,0x0a000000,0x3100);command(ram,a,0xdf000000,0);
    put(ram,0x3100,256);put(ram,0x3104,256);
    put(ram,0x3108,256);put(ram,0x310c,256);
    put(ram,0x3110,0x10000);put(ram,0x3114,0xfff40002);
    for(unsigned i=0;i<64*64;++i) {ram[0x10000+i*2]=0xf8;ram[0x10001+i*2]=1;}
    // The checkpointed pass first: a later identical background is a decoded-
    // background cache hit (ADR-084) and runs no decode loop.
    unsigned polls=0;
    const auto unchanged=ram;
    const auto serviced=interpret_gbi(ram,0x1000,[&] {
        ++polls;
        // A guest thread can modify live RAM after the snapshot. Decoder
        // checkpoints must continue reading the complete owned snapshot.
        ram[0x10000]=0;
    });
    const auto original=interpret_gbi(unchanged,0x1000);
    require(polls>64,"Checkpoints occur within background decode/hash loops");
    require(serviced.opcodes==original.opcodes && serviced.textures[0].key==original.textures[0].key,
        "Scheduling does not change display-list commands or content keys");
    require(render_software(serviced).ram==render_software(original).ram,
        "Interpreter checkpoints preserve owned input and exact framebuffer writes");
    bool cancelled=false;
    try {interpret_gbi(ram,0x1000,[]{throw std::runtime_error("cancelled");});}
    catch(const std::runtime_error& e) {cancelled=std::string(e.what())=="cancelled";}
    require(cancelled,"Checkpoint cancellation must reach the caller");
}
std::uint64_t legacy_texture_key(const Texture& tex) {
    // Independent slow encoding from the original content-key contract.
    std::uint64_t hash=14695981039346656037ull;
    auto add=[&](std::uint64_t v) {for(unsigned j=0;j<8;++j) {hash^=(v>>(j*8))&255;hash*=1099511628211ull;}};
    add(tex.width);add(tex.height);add(tex.background);add(tex.tlut_mode);
    if(!tex.background)for(auto value:tex.memory.bytes)add(value);
    const auto& t=tex.tile;
    for(auto v:{t.format,t.size,t.line,t.tmem,t.palette,t.cms,t.cmt,t.masks,t.maskt,t.shifts,t.shiftt,
                unsigned(t.sl),unsigned(t.tl),unsigned(t.sh),unsigned(t.th)})add(v);
    for(std::size_t i=0;i<tex.texel_count();++i){auto c=tex.texel(i);for(int v:{c.r,c.g,c.b,c.a})add(v);}
    return hash;
}
void texture_key_fixtures() {
    std::vector<std::uint8_t> ram(0x800000);unsigned a=0x1000;
    command(ram,a,0xff1000ff,0x20000);command(ram,a,0xed000000,0x00400004);
    command(ram,a,0x0a000000,0x3100);command(ram,a,0x0a000000,0x3100);
    command(ram,a,0xdf000000,0);
    put(ram,0x3100,1024);put(ram,0x3104,1024);
    put(ram,0x3108,4);put(ram,0x310c,4);
    put(ram,0x3110,0x10000);put(ram,0x3114,0xfff40003);
    for(unsigned i=0;i<256;++i)for(unsigned j=0;j<4;++j)ram[0x10000+i*4+j]=(i+j*79)&255;
    auto background=interpret_gbi(ram,0x1000);
    require(background.textures.size()==1 && background.rectangles.size()==2,
        "Identical backgrounds reuse one owned texture");
    const auto& bg=background.textures[0];
    require(bg.key==legacy_texture_key(bg),"All channel values preserve the historical 64-bit key");
    for(unsigned i=0;i<256;++i)
        require(bg.texel(i)==Color{ram[0x10000+i*4],ram[0x10000+i*4+1],ram[0x10000+i*4+2],ram[0x10000+i*4+3]},
            "Background pixels equal the independently authored RGBA32 source");

    // CI8 snapshots repeat, then a palette load changes the same source index.
    // Reuse must compare semantic input before sharing a texture.
    a=0x1000;
    command(ram,a,0xff100003,0x20000);command(ram,a,0xed000000,0x00010004);
    command(ram,a,0xef208000,0); // COPY, RGBA16 TLUT
    command(ram,a,0xfd100000,0x5000);command(ram,a,0xf5000100,0);
    command(ram,a,0xf0000000,0); // load palette entry 0
    command(ram,a,0xfd480003,0x6000);command(ram,a,0xf5480200,0);
    command(ram,a,0xf4000000,0x0000c000); // load four CI8 indices
    auto rect=[&] {
        command(ram,a,0xe400f003,0);command(ram,a,0xe1000000,0);
        command(ram,a,0xf1000000,0x10000400);
    };
    rect();unsigned end=a;command(ram,a,0xdf000000,0);
    ram[0x5000]=0xf8;ram[0x5001]=1;ram[0x5010]=7;ram[0x5011]=0xc1;
    unsigned single_polls=0;auto single=interpret_gbi(ram,0x1000,[&]{++single_polls;});
    a=end;rect();command(ram,a,0xdf000000,0);
    unsigned double_polls=0;auto repeated=interpret_gbi(ram,0x1000,[&]{++double_polls;});
    require(repeated.textures.size()==1 && repeated.rectangles[1].texture==0,
        "Repeated CI8 rectangles reuse the same collision-checked texture");
    require(double_polls-single_polls==9,"Repeated TMEM snapshot retains hash/command checkpoints");
    require(repeated.textures[0].memory.bytes==single.textures[0].memory.bytes,"Reused TMEM snapshot stays identical");
    end=a-8;a=end;
    command(ram,a,0xfd100000,0x5010);command(ram,a,0xf5000100,0);command(ram,a,0xf0000000,0);
    command(ram,a,0xf5480200,0);
    rect();command(ram,a,0xdf000000,0);
    auto changed=interpret_gbi(ram,0x1000);
    require(changed.textures.size()==2 && changed.rectangles[2].texture==1,
        "Palette change invalidates repeated texture");
    for(const auto& tex:changed.textures)require(tex.key==legacy_texture_key(tex),"TMEM/TLUT keys retain slow reference encoding");
    require(changed.textures[0].memory.palette(0)!=changed.textures[1].memory.palette(0),
        "TLUT snapshots reflect ordered palette updates");
}
void addressing_fixtures() {
    DrawList list;list.initial_ram.resize(0x800000);
    Texture tex;tex.tile.format=4;tex.tile.size=1;tex.tile.line=1;
    tex.tile.cms=1;tex.tile.masks=2;tex.tile.sh=12;
    tex.memory.bytes[0]=16;tex.memory.bytes[1]=48;tex.memory.bytes[2]=80;tex.memory.bytes[3]=112;
    list.textures.push_back(tex);
    Rectangle r;r.x1=24;r.y1=4;r.s=128;r.texture=0;r.target={0x4000,6,0,2};
    r.state.combine=0xfcfffffffffcf279ULL;r.state.other_hi=0xc0;
    r.state.scissor={0,0,24,4,0};list.rectangles.push_back(r);
    auto check=[&](std::array<int,6> expected) {
        auto output=render_software(list);
        for(unsigned x=0;x<6;++x) {
            unsigned actual=(output.ram[0x4000+x*2]<<8)|output.ram[0x4001+x*2];
            require(actual==encode_rgba16({expected[x],expected[x],expected[x],255}),"Independent mask/mirror/clamp/shift fixture");
        }
    };
    check({112,80,48,16,16,48}); // mirrored four-texel period, then repeat
    list.textures[0].tile.cms=2;check({112,112,112,112,112,112});
    list.textures[0].tile.cms=0;list.textures[0].tile.shifts=1;list.rectangles[0].s=0;
    check({16,16,48,48,80,80});
    // Reversed SETTILESIZE limits would give the clamp an inverted range.
    list.textures[0].tile.cms=2;list.textures[0].tile.sl=16;list.textures[0].tile.sh=0;
    rejects([&]{render_software(list);});
}
// Raw RDP command words for interpret_rdp fixtures.
struct Rdp {
    std::vector<std::uint64_t> words;
    void op(std::uint32_t hi,std::uint32_t lo) {words.push_back((std::uint64_t(hi)<<32)|lo);}
    void image(unsigned op_code,unsigned format,unsigned size,unsigned width,unsigned address) {
        op((op_code<<24)|(format<<21)|(size<<19)|(width-1),address);
    }
    void tile(unsigned index,unsigned format,unsigned size,unsigned line,unsigned tmem) {
        op((0x35u<<24)|(format<<21)|(size<<19)|(line<<9)|tmem,index<<24);
    }
    void load(unsigned op_code,unsigned index,unsigned sl,unsigned tl,unsigned sh,unsigned th) {
        op((op_code<<24)|(sl<<12)|tl,(index<<24)|(sh<<12)|th);
    }
    // COPY rectangle of width x 1 pixels at the origin, one texel per pixel.
    void copy_rect(unsigned index,unsigned width) {
        op((0x24u<<24)|((((width-1)<<2)|3)<<12)|3,index<<24);
        words.push_back((std::uint64_t(4096)<<16)|1024);
    }
};
std::uint16_t pixel(const SoftwareResult& out,unsigned a) {return (out.ram[a]<<8)|out.ram[a+1];}
std::uint64_t combine_word(unsigned a,unsigned b,unsigned c,unsigned d) {
    // Second-cycle RGB fields over the pure-texel alpha of FCFFFFFFFFFCF279.
    std::uint64_t v=0xfcfffffffffcf279ULL;
    v&=~((15ull<<37)|(31ull<<32)|(15ull<<24)|(7ull<<6));
    return v|(std::uint64_t(a)<<37)|(std::uint64_t(c)<<32)|(std::uint64_t(b)<<24)|(std::uint64_t(d)<<6);
}
void combiner_input_fixtures() {
    DrawList list;list.initial_ram.resize(0x800000);
    Texture tex;tex.background=true;tex.width=tex.height=1;tex.pixels={{200,100,48,255}};
    list.textures.push_back(tex);
    Rectangle r;r.x1=4;r.y1=4;r.target={0x4000,1,0,2};r.state.other_hi=0xc0;
    r.state.scissor={0,0,4,4,0};r.state.primitive={128,128,128,255};
    list.rectangles.push_back(r);
    auto render=[&](unsigned a,unsigned b,unsigned c,unsigned d) {
        list.rectangles[0].state.combine=combine_word(a,b,c,d);return pixel(render_software(list),0x4000);
    };
    // Unmodelled hardware sources (angrylion combiner.c) must not read as zero.
    rejects([&]{render(1,7,1,7);});  // B: K4 from SET_CONVERT
    rejects([&]{render(1,6,1,7);});  // B: key center
    rejects([&]{render(1,15,6,7);}); // C: key scale
    rejects([&]{render(1,15,7,7);}); // C: combined alpha
    rejects([&]{render(1,15,13,7);});// C: LOD fraction
    rejects([&]{render(7,15,1,7);}); // A: noise
    rejects([&]{render(0,15,1,7);}); // combined color
    // Explicit hardware zero codes and constants remain valid.
    require(render(8,8,16,7)==encode_rgba16({0,0,0,255}),"A/B 8..15, C 16..31 and D 7 are zero");
    require(render(15,15,31,1)==encode_rgba16({200,100,48,255}),"Pure texel through D");
    require(render(6,15,3,7)==encode_rgba16({128,128,128,255}),"A one times primitive");
    require(render(1,15,11,6)==encode_rgba16({255,255,255,255}),"C shade alpha is zero; D one");
}
void copy_alpha_compare_fixtures() {
    std::vector<std::uint8_t> ram(0x800000);
    for(unsigned i=0;i<16;i+=2) {ram[0x4000+i]=0x12;ram[0x4001+i]=0x34;}
    // RGBA16 texels: opaque red, then transparent red.
    ram[0x5000]=0xf8;ram[0x5001]=0x01;ram[0x5002]=0xf8;ram[0x5003]=0x00;
    auto rgba=[&](std::uint32_t other_lo,std::uint32_t blend) {
        Rdp rdp;rdp.image(0x3f,0,2,4,0x4000);rdp.op(0x2d000000,(16u<<12)|16);
        rdp.op((0x2fu<<24)|(2u<<20),other_lo);rdp.op(0x39000000,blend);
        rdp.image(0x3d,0,2,2,0x5000);rdp.tile(0,0,2,1,0);rdp.load(0x34,0,0,0,4,0);
        rdp.copy_rect(0,2);
        return render_software(interpret_rdp(ram,rdp.words));
    };
    auto out=rgba(1,0);
    // COPY into a 16-bit target tests texel bit 0 even when BLEND alpha is zero.
    require(pixel(out,0x4000)==0xf801 && pixel(out,0x4002)==0x1234 && out.written_pixels==1,
        "COPY alpha compare rejects alpha-bit-zero texels with zero blend alpha");
    out=rgba(0,0);
    require(pixel(out,0x4002)==0xf800 && out.written_pixels==2,"COPY without alpha compare stores both texels");
    out=rgba(3,0xff);
    require(out.written_pixels==1,"COPY alpha compare ignores alpha dither and the blend threshold");
    // IA16 TLUT: the entry low bit decides, not the alpha byte against BLEND.
    ram[0x6000]=0x80;ram[0x6001]=0xfe;ram[0x6002]=0x40;ram[0x6003]=0x01;
    ram[0x5100]=0;ram[0x5101]=1;
    Rdp rdp;rdp.image(0x3f,0,2,4,0x4000);rdp.op(0x2d000000,(16u<<12)|16);
    rdp.op((0x2fu<<24)|(2u<<20)|(3u<<14),1);rdp.op(0x39000000,0x80);
    rdp.image(0x3d,0,2,2,0x6000);rdp.tile(1,0,2,0,256);rdp.load(0x30,1,0,0,4,0);
    rdp.image(0x3d,2,1,2,0x5100);rdp.tile(0,2,1,1,0);rdp.load(0x34,0,0,0,4,0);
    rdp.copy_rect(0,2);
    out=render_software(interpret_rdp(ram,rdp.words));
    require(pixel(out,0x4000)==0x1234 && pixel(out,0x4002)!=0x1234 && out.written_pixels==1,
        "COPY IA16 TLUT alpha compare uses entry bit 0");
    // 8-bit COPY texels without a TLUT have no represented raw halfword.
    Rdp i8;i8.image(0x3f,0,2,4,0x4000);i8.op(0x2d000000,(16u<<12)|16);
    i8.op((0x2fu<<24)|(2u<<20),1);i8.image(0x3d,4,1,2,0x5100);i8.tile(0,4,1,1,0);
    i8.load(0x34,0,0,0,4,0);i8.copy_rect(0,2);
    rejects([&]{render_software(interpret_rdp(ram,i8.words));});
}
void load_tlut_fixtures() {
    std::vector<std::uint8_t> ram(0x800000);
    ram[0x6000]=0xf8;ram[0x6001]=0x01;ram[0x6002]=0x07;ram[0x6003]=0xc1;
    ram[0x5100]=1;
    auto render=[&](unsigned sl,unsigned tl,unsigned sh,unsigned th) {
        Rdp rdp;rdp.image(0x3f,0,2,4,0x4000);rdp.op(0x2d000000,(16u<<12)|16);
        rdp.op((0x2fu<<24)|(2u<<20)|(2u<<14),0);
        rdp.image(0x3d,0,2,2,0x6000);rdp.tile(1,0,2,0,256);rdp.load(0x30,1,sl,tl,sh,th);
        rdp.image(0x3d,2,1,1,0x5100);rdp.tile(0,2,1,1,0);rdp.load(0x34,0,0,0,0,0);
        rdp.copy_rect(0,1);
        return render_software(interpret_rdp(ram,rdp.words));
    };
    // The load spans integer columns sl>>2..sh>>2: a fractional SL of 1 still
    // loads both entries (count 2), while (SH-SL)/4+1 would load only one.
    require(pixel(render(1,0,4,0),0x4000)==0x07c1,"LOADTLUT integer column count");
    require(pixel(render(0,3,4,1),0x4000)==0x07c1,"LOADTLUT fractional T stays on one row");
    rejects([&]{render(4,0,0,0);});  // reversed columns
    rejects([&]{render(0,0,4,4);});  // second row crashes the RDP pipeline
}
void feedback_dependency_fixtures() {
    std::vector<std::uint8_t> ram(0x800000);
    // A 1-cycle primitive fill of columns 1..2 on rows 0..1 of a width-4 target.
    auto parse=[&](unsigned load_address) {
        Rdp rdp;rdp.image(0x3f,0,2,4,0x4000);rdp.op(0x2d000000,(16u<<12)|16);
        rdp.op(0x2f0000c0,0);rdp.op(0xfcffffff,0xfffdf6fb);
        rdp.op((0x24u<<24)|(12u<<12)|8,(4u<<12)|0);rdp.words.push_back(0);
        rdp.image(0x3d,0,2,1,load_address);rdp.tile(0,0,2,1,0);rdp.load(0x34,0,0,0,0,0);
        return interpret_rdp(ram,rdp.words);
    };
    // Row spans are [0x4002,0x4006) and [0x400a,0x400e).
    require(parse(0x4006).rectangles.size()==1,"Gap between written rows is not feedback");
    require(parse(0x4010).rectangles.size()==1,"Range past the written hull is not feedback");
    require(parse(0x4000).rectangles.size()==1,"Range before the written hull is not feedback");
    rejects([&]{parse(0x400c);});
    rejects([&]{parse(0x4004);});
}
// Parallel rasterization must be byte-identical to the serial command order,
// including blends that read earlier stores and targets aliasing other targets.
DrawList parallel_list(std::uint32_t seed) {
    std::mt19937 random(seed);
    DrawList list;list.initial_ram.resize(0x800000);
    for(auto& byte:list.initial_ram)byte=std::uint8_t(random());
    Texture texture;texture.background=true;texture.width=37;texture.height=23;
    for(unsigned i=0;i<texture.width*texture.height;++i)
        texture.pixels.push_back({int(random()&255),int(random()&255),int(random()&255),int(random()&255)});
    list.textures.push_back(texture);
    State state;
    // RGB/alpha: (TEXEL0 - 0) * PRIMITIVE + 0; BLEND: color*a + memory*(1-a).
    state.combine=(std::uint64_t((1u<<5)|3u)<<32)|((8u<<24)|(1u<<21)|(3u<<18)|(7u<<6)|(7u<<3)|7u);
    state.primitive={255,200,128,255};
    state.other_lo=(1u<<22)|(1u<<14);
    const Image a{0x100000,320,0,2},b{0x100000+640*10+64,200,0,2};
    for(unsigned i=0;i<24;++i) {
        Rectangle r;r.texture=0;r.state=state;r.target=i%3==2?b:a;
        const int w=r.target.width;
        r.x0=int(random()%unsigned(w))*4-8;r.y0=int(random()%200)*4-8;
        r.x1=r.x0+int(40+random()%200)*4;r.y1=r.y0+int(10+random()%120)*4;
        r.s=int(random()%1024);r.t=int(random()%1024);
        if(i%5==1){r.state.other_lo|=1;r.state.blend.a=int(random()&255);}
        if(i%7==3)r.state.other_hi|=3u<<6; // no dither
        list.rectangles.push_back(r);
    }
    return list;
}
void require_same(const SoftwareResult& x,const SoftwareResult& y,const char* message) {
    bool writes=x.writes.size()==y.writes.size();
    for(std::size_t i=0;writes && i<x.writes.size();++i)
        writes=x.writes[i].address==y.writes[i].address && x.writes[i].length==y.writes[i].length;
    require(x.ram==y.ram && x.written_pixels==y.written_pixels && writes,message);
}
std::string failure(const std::function<void()>& call) {
    try{call();}catch(const std::exception& error){return error.what();}
    return {};
}
void parallel_raster_fixtures() {
    RasterPool pool3(3),pool1(1),pool0(0);
    for(std::uint32_t seed=1;seed<=6;++seed) {
        const auto list=parallel_list(seed);
        const auto serial=render_software(list);
        require(serial.written_pixels>parallel_raster_min_pixels,"Parallel fixture has enough work");
        for(std::size_t i=1;i<serial.writes.size();++i)
            require(serial.writes[i-1].address+serial.writes[i-1].length<serial.writes[i].address,
                    "Writes are sorted, merged and disjoint");
        for(unsigned repeat=0;repeat<4;++repeat) {
            require_same(serial,render_software(list,{},&pool3),"Three helpers match serial raster");
            require_same(serial,render_software(list,{},&pool1),"One helper matches serial raster");
        }
        require_same(serial,render_software(list,{},&pool0),"Empty pool uses serial raster");
        auto odd=list;for(auto& r:odd.rectangles)if(r.target.address==0x100000)r.target.address=0x100001;
        require_same(render_software(odd),render_software(odd,{},&pool3),"Odd target falls back to serial");
    }
    // Checkpoints run only on the calling thread, including while helpers work.
    const auto list=parallel_list(9);
    const auto caller=std::this_thread::get_id();
    unsigned calls=0;bool foreign=false;
    render_software(list,[&]{++calls;foreign=foreign || std::this_thread::get_id()!=caller;},&pool3);
    require(calls>0 && !foreign,"Parallel checkpoints stay on the guest thread");
    // A throwing checkpoint cancels the task and leaves the pool reusable.
    struct Stop {};
    bool stopped=false;
    try{render_software(list,[]{throw Stop{};},&pool3);}catch(const Stop&){stopped=true;}
    require(stopped,"Checkpoint exception propagates from a parallel raster");
    require_same(render_software(list),render_software(list,{},&pool3),"Pool is reusable after cancellation");
    // The reported failure is the first in serial order regardless of timing:
    // an early out-of-RDRAM store wins over later rectangle-level errors.
    auto bad=parallel_list(11);
    bad.rectangles[2].target={0x7ffff0,320,0,2};
    bad.rectangles[2].x0=0;bad.rectangles[2].y0=0;bad.rectangles[2].x1=1280;bad.rectangles[2].y1=400;
    bad.rectangles[2].state.other_lo&=~1u;
    bad.rectangles[17].state.other_lo|=0x10;
    bad.rectangles[20].texture=5;
    const auto expected=failure([&]{render_software(bad);});
    require(expected=="Framebuffer write outside RDRAM","Serial failure fixture");
    for(unsigned repeat=0;repeat<8;++repeat)
        require(failure([&]{render_software(bad,{},&pool3);})==expected,"Parallel failure matches serial order");
    bad.rectangles[2]=parallel_list(11).rectangles[2];
    require(failure([&]{render_software(bad,{},&pool3);})==failure([&]{render_software(bad);}),
            "Rectangle-level failure matches serial order");
}
int main() {
    try {tmem_fixtures();negative_nibble_fixtures();image_address_fixtures();background_descriptor_fixtures();interpreter_checkpoint_fixtures();texture_key_fixtures();
         gbi_fixtures();addressing_fixtures();combiner_input_fixtures();copy_alpha_compare_fixtures();
         load_tlut_fixtures();feedback_dependency_fixtures();parallel_raster_fixtures();std::cout<<"gfx_core fixtures passed\n";}
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
