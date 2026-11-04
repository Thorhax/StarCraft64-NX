#include "session.hpp"
#include "pcm_ring.hpp"
#include "pcm_playback.hpp"
#include "pcm_inbox.hpp"
#include "pcm_trace.hpp"
#include "pcm_output.hpp"
#include "audio_timing.hpp"
#include "cursor_feedback.hpp"
#include "ui_feedback.hpp"
#include "mouse_cursor.hpp"
#include "rdp_writeback.hpp"
#include "pause_policy.hpp"
#include "rumble_motor.hpp"
#include <cassert>
#include <future>
#include <thread>

static void rdp_completion_regression() {
    using namespace sc64::desktop;
    PauseSources causes;
    causes.suspended=true; assert(causes.paused());
    causes.requested=true; causes.suspended=false; assert(causes.paused());
    causes.requested=false; assert(!causes.paused());
    causes.suspended=true; causes.requested=false; assert(causes.paused());
    using namespace sc64::gfx;
    std::vector<std::uint8_t> guest(0x800000);
    auto snapshot=[&] {
        std::vector<std::uint8_t> canonical(guest.size());
        for (std::size_t i=0;i<guest.size();++i) canonical[i]=guest[i^3];
        return canonical;
    };
    DrawList first; first.initial_ram=snapshot(); first.identity.task=1;
    Texture texture; texture.background=true; texture.width=4; texture.height=2;
    texture.pixels={{255,0,0,255},{255,0,0,255},{255,0,0,255},{255,0,0,255},
                    {0,255,0,255},{0,255,0,255},{0,255,0,255},{0,255,0,255}};
    first.textures.push_back(texture);
    Rectangle rect; rect.x1=16; rect.y1=8; rect.target={0x1000,4,0,2};
    rect.state.scissor={0,0,16,8,0}; rect.state.other_hi=2u<<20;
    first.rectangles.push_back(rect);
    auto painted=std::make_shared<RenderedFrame>();
    unsigned checkpoints=0;
    painted->pixels=render_software(first,[&] { ++checkpoints; guest[0x2000^3]=0x61; });
    auto uninterrupted=render_software(first);
    assert(checkpoints>0 && painted->pixels.ram==uninterrupted.ram);
    assert(painted->pixels.written_pixels==uninterrupted.written_pixels);
    assert(painted->pixels.written_pixels==8);
    commit_rdp_writes(painted->pixels,guest);
    assert(guest[0x2000^3]==0x61); // audio/other guest writes survive snapshot RDP completion
    FrameMailbox frames;
    frames.publish(std::make_shared<DrawList>(first),{},painted);
    DrawList fade=first; fade.initial_ram=snapshot(); fade.identity.task=2;
    fade.rectangles[0].state.other_hi=3u<<6; // no RGB dither
    fade.rectangles[0].state.other_lo=(1u<<14)|(1u<<22); // black over memory
    fade.rectangles[0].state.combine=0xfcfffffffffdf6fbULL;
    fade.rectangles[0].state.primitive={0,0,0,128};
    auto faded=std::make_shared<RenderedFrame>(); faded->pixels=render_software(fade);
    guest[0x2000^3]=0x52; // unrelated CPU data must survive an old packet
    commit_rdp_writes(faded->pixels,guest);
    frames.publish(std::make_shared<DrawList>(fade),{},faded);
    const auto packets=frames.take(); // both packets, oldest first
    assert(packets.size()==2 && packets[0].frame->identity.task==1 && packets[1].frame->identity.task==2);
    const auto presented=packets[1].rendered;
    assert(presented==faded); // the newer presentation and the stores of both
    assert(guest[0x1000^3]==0x78 && guest[0x1001^3]==0x01);
    assert(guest[0x1008^3]==0x03 && guest[0x1009^3]==0xc1);
    assert(guest[0x2000^3]==0x52);

    DrawList rejected=first; rejected.initial_ram=snapshot();
    rejected.textures[0].tile.size=2; // RGBA16: COPY alpha compare tests texel bit 0
    for (auto& c:rejected.textures[0].pixels) c.a=0;
    rejected.rectangles[0].state.other_lo=1;
    rejected.rectangles[0].state.blend.a=64;
    auto transparent=render_software(rejected);
    assert(transparent.written_pixels==0 && transparent.writes.empty());
    guest[0x1000^3]=0xaa;
    commit_rdp_writes(transparent,guest);
    assert(guest[0x1000^3]==0xaa);

    auto malformed=painted->pixels;
    malformed.writes.push_back({0x7fffff,2});
    bool failed=false;
    try { commit_rdp_writes(malformed,guest); }
    catch (const std::runtime_error&) { failed=true; }
    assert(failed && guest[0x1000^3]==0xaa); // reject before any stores
}

static void minimap_regression() {
    using namespace sc64::runtime;
    std::vector<std::uint8_t> ram(0x800000);
    auto half=[&](unsigned a,int v) { ram[a]=unsigned(v)>>8; ram[a+1]=v; };
    half(0xa5ab4,3); ram[0xafd7c]=1;
    half(0xafd84,17); half(0xafd86,159); half(0xafd88,79); half(0xafd8a,221);
    half(0xef670,64); half(0xef672,64); half(0xef656,32); half(0xef650,1);
    half(0xa5b04,640); half(0xa5b08,480);
    auto profile=read_minimap(ram); assert(profile);
    std::vector<std::uint8_t> swapped(ram.size());
    for (unsigned i=0;i<ram.size();++i) swapped[i^3]=ram[i];
    assert(read_minimap(swapped,3)==profile);
    MinimapInbox inbox;
    inbox.submit({60.75,200.25,profile->context,123});
    auto target=inbox.take(profile);
    assert(target && target->x==1056 && target->y==1088 && target->field==123);
    assert(!inbox.take(profile));
    inbox.submit({17,159,profile->context,123});
    target=inbox.take(profile); assert(target && target->x==0 && target->y==0);
    inbox.submit({79.9,221.9,profile->context,124});
    target=inbox.take(profile); assert(target && target->x==1664 && target->y==1760);
    inbox.submit({60,200,profile->context,125});
    inbox.submit({50,190,profile->context,126});
    target=inbox.take(profile); assert(target && target->field==126 && target->x==736 && target->y==768);
    inbox.submit({60,200,profile->context+1,123}); assert(!inbox.take(profile));
    inbox.submit({80,200,profile->context,123}); assert(!inbox.take(profile));
    inbox.submit({NAN,200,profile->context,123}); assert(!inbox.take(profile));
    inbox.submit({60,200,profile->context,123}); inbox.clear(); assert(!inbox.take(profile));
    for (unsigned a : {0xafefcu,0xa5b0eu,0xc6a64u,0xfd038u}) {
        ram[a]=1; assert(!read_minimap(ram)); ram[a]=0;
    }
    half(0xf0c20,1); assert(!read_minimap(ram)); half(0xf0c20,0);
    ram[0xafd7c]=0; assert(!read_minimap(ram)); ram[0xafd7c]=1;
    half(0xef650,0); assert(!read_minimap(ram)); half(0xef650,1);
    half(0xef654,31); assert(!read_minimap(ram)); half(0xef654,0);
    half(0xa5ab4,4); assert(!read_minimap(ram));
    assert(!read_minimap(std::span<const std::uint8_t>(ram).first(0x100)));
}

struct Terminated : std::exception {};
static void review_regressions() {
    using namespace sc64;
    // The first failure is the root cause; shutdown consequences cannot replace it.
    runtime::FirstError error;
    assert(error.get().empty());
    assert(error.record("Graphics microcode identity mismatch"));
    assert(!error.record("std::exception"));
    assert(error.get()=="Graphics microcode identity mismatch");
    // Guest-thread shutdown unwinding is never reported as a host failure.
    std::string reported;
    bool unwound=false;
    try { runtime::report_host_failure<Terminated>([] { throw Terminated{}; },
                                                   [&](const char* e) { reported=e; }); }
    catch (const Terminated&) { unwound=true; }
    assert(unwound && reported.empty());
    runtime::report_host_failure<Terminated>([] { throw std::runtime_error("bad task"); },
                                             [&](const char* e) { reported=e; });
    assert(reported=="bad task");

    // Both HUD context callers agree: live word-swapped and canonical snapshots.
    std::vector<std::uint8_t> ram(0x800000);
    ram[0xa5ab4]=0; ram[0xa5ab5]=3; // in a level, no modal/build/split screen
    std::uint32_t expected=2166136261u, seed=7;
    auto fill=[&](unsigned a,unsigned n) {
        for (unsigned i=0;i<n;++i) { seed=seed*1103515245u+12345u; ram[a+i]=seed>>16; }
    };
    fill(0xc6c14,8); fill(0xfd0b0,4); fill(0xc6c20,72);
    for (unsigned a : {0xc6c14u,0xc6c18u,0xfd0b0u})
        for (unsigned i=0;i<4;++i) expected=(expected^ram[a+i])*16777619u;
    for (unsigned i=0;i<72;++i) expected=(expected^ram[0xc6c20+i])*16777619u;
    std::vector<std::uint8_t> swapped(ram.size());
    for (unsigned i=0;i<ram.size();++i) swapped[i^3]=ram[i];
    const auto ui=desktop::read_ui(ram,1);
    assert(ui.mode=="game" && ui.context==expected);
    assert(runtime::hud_context(swapped.data())==ui.context);

    // A scissor extent past the top of RDRAM is skipped, never exported.
    const gfx::Image top{0x7ff000,320,0,2}, low{0x100000,320,0,2};
    assert(!desktop::target_in_ram(top,240,0x800000));
    assert(desktop::target_in_ram(low,240,0x800000));
    assert(desktop::target_in_ram({0x800000-640*240,320,0,2},240,0x800000));
    desktop::Scanout vi; vi.regs[0]=3; vi.regs[1]=0x7ff000+640; vi.regs[2]=320;
    vi.regs[9]=1; vi.regs[10]=1; vi.completed_task=1;
    assert(!desktop::scanout_matches(vi,top,240,1,0x800000));
}

static void rumble_motor_regression() {
    using sc64::desktop::RumbleMotor;
    RumbleMotor motor;
    const auto t0=RumbleMotor::Clock::now();
    assert(motor.update(false,t0)==RumbleMotor::none && !motor.on());
    assert(motor.update(true,t0)==RumbleMotor::burst_ms && motor.on());
    assert(motor.update(true,t0+std::chrono::milliseconds(100))==RumbleMotor::none);
    // A long guest motor is re-armed before the one-shot burst expires.
    assert(motor.update(true,t0+RumbleMotor::rearm)==RumbleMotor::burst_ms);
    static_assert(RumbleMotor::rearm<std::chrono::milliseconds(RumbleMotor::burst_ms));
    assert(motor.update(false,t0+RumbleMotor::rearm)==0 && !motor.on());
    assert(motor.update(false,t0+RumbleMotor::rearm)==RumbleMotor::none);
}
int main() {
    rdp_completion_regression();
    minimap_regression();
    review_regressions();
    rumble_motor_regression();
    using namespace sc64::desktop;
    PausePolicy policy;
    policy.inactive=true;
    for (int mode : {0,4,12,-1}) {
        policy.observe_mode(mode);
        assert(!policy.paused() && !policy.input_active());
    }
    policy.observe_mode(3);
    assert(policy.paused() && !policy.input_active());
    policy.observe_mode(-1); // a blank/invalid frame cannot release a level pause
    assert(policy.paused());
    policy.inactive=false; assert(!policy.paused() && policy.input_active());
    policy.user=true; assert(policy.paused() && !policy.input_active());
    policy.inactive=true; policy.observe_mode(4);
    assert(policy.paused()); // entering the front end never cancels user pause
    policy.user=false; assert(!policy.paused() && !policy.input_active());
    policy.iconified=true; assert(policy.paused());
    policy.inactive=false; assert(policy.paused()); // activation alone does not undo iconify
    policy.iconified=false; assert(!policy.paused() && policy.input_active());
    policy.inactive=true; policy.observe_mode(3); policy.background_probe=true;
    assert(!policy.paused() && policy.input_active());
    policy.iconified=true; assert(policy.paused() && !policy.input_active());
    using namespace std::chrono_literals;
    std::vector<std::uint8_t> ram(8 * 1024 * 1024);
    auto word = [&](unsigned a, std::uint32_t value) {
        for (unsigned i=0; i<4; ++i) ram[a+i]=std::uint8_t(value >> (24-8*i));
    };
    word(0xafdbc, std::bit_cast<std::uint32_t>(320.25f));
    word(0xafdc4, std::bit_cast<std::uint32_t>(240.5f));
    word(0xafdcc, (320u << 16) | 241u);
    word(0xafdf8,std::bit_cast<std::uint32_t>(200.0f));
    auto feedback=read_cursor(ram, 42);
    assert(feedback.valid && feedback.x==160.125f && feedback.y==120.25f && feedback.field==42);
    assert(feedback.divisor==200);
    word(0xafdf8,0x7fc00000); assert(read_cursor(ram,42).divisor==0);
    word(0xafdf8,std::bit_cast<std::uint32_t>(150.0f)); assert(read_cursor(ram,42).divisor==150);
    assert(!read_cursor(std::span<const std::uint8_t>(ram.data(),0xafdd0),42).valid);
    // The OS pointer remains visible over the painted HUD even when Lua has
    // no recent mouse move. Projection uses the actual letterboxed viewport.
    const Sc64CursorRegion image{.02,.05,.98,.95};
    const auto hud_cursor=sc64_project_cursor_region(image,176,160,312,220,320,240);
    assert(std::abs(hud_cursor.x0-.548)<1e-6 && std::abs(hud_cursor.y0-.125)<1e-6);
    assert(hud_cursor.contains(.8,.2) && !hud_cursor.contains(.4,.2));
    assert(!hud_cursor.contains(hud_cursor.x1,.2) && !hud_cursor.contains(.8,hud_cursor.y1));
    word(0xafdbc, std::bit_cast<std::uint32_t>(800.0f));
    assert(!read_cursor(ram,43).valid);
    word(0xafdbc, 0x7fc00000); // NaN in a non-gameplay/malformed snapshot
    assert(!read_cursor(ram,43).valid && !read_cursor({},43).valid);
    auto half = [&](unsigned a, unsigned value) { ram[a]=value>>8; ram[a+1]=value; };
    half(0xa5ab4, 3); half(0xf0c20, 0);
    assert(read_ui(ram,44).mode == "game");
    sc64::runtime::FrontendFeedback pick_frame;
    auto context=read_ui(ram,44).context;
    half(0xafe18,200); half(0xafe1a,160); word(0xa2ee8,0); word(0xa2eec,0);
    pick_frame.pick=sc64::runtime::CursorPick{context,100,80,0,0,0x4000,176,true};
    assert(read_ui(ram,44,pick_frame).pick && read_ui(ram,44).pick_required);
    word(0xa2ee8,8); assert(!read_ui(ram,44,pick_frame).pick); word(0xa2ee8,0);
    half(0xafe18,202); assert(!read_ui(ram,44,pick_frame).pick); half(0xafe18,200);
    word(0xc6c18,0x80301000); assert(!read_ui(ram,44,pick_frame).pick); word(0xc6c18,0);

    half(0xf0c20, 1); word(0xa4ab0,0x80300000);
    half(0x300004,102); half(0x300006,20);
    word(0x30002c,0x8003fe04); word(0x300040,0x80300100); word(0x300044,0x80300100);
    word(0x300100,0x80300200); word(0x300118,0x1a18); word(0x300130,0x8001ecb4);
    half(0x300104,12); half(0x300106,30); half(0x300108,103); half(0x30010a,49);
    word(0x300200,0); word(0x300218,0xa1a); word(0x300230,0x8001ecb4); // disabled flag 2
    auto menu=read_ui(ram,45);
    assert(menu.mode=="modal" && menu.token==0x80300000 && menu.items.size()==1);
    assert(menu.items[0].selected && menu.items[0].x0==114 && menu.items[0].y0==50);
    word(0x30002c,0x8003faac); assert(read_ui(ram,46).mode=="message");
    word(0xb03d8,7); assert(read_ui(ram,46).token==(7ull<<32 | 0x80300000));
    word(0xb03d8,8); assert(read_ui(ram,46).token==(8ull<<32 | 0x80300000));
    word(0x300200,0x80300100); assert(read_ui(ram,47).items.empty()); // cycle fails closed
    word(0x300044,0x807ffff0); assert(read_ui(ram,47).items.empty()); // incomplete object
    assert(read_ui({},47).mode=="unknown");
    half(0xa5ab4,4); word(0xd1170,25); assert(read_ui(ram,48).mode=="intro");
    word(0xd1170,0); assert(read_ui(ram,49).mode=="menu");
    word(0xd1170,15); word(0x112800,0x80300000); word(0xa4ab0,0x80300000);
    word(0x30002c,0x8012c90c); word(0x12c90c,0x27bdffe0);
    word(0x12b4a4,0x27bdffc0);
    for (unsigned id=0; id<4; ++id) {
        half(0x112860+id*8,384); half(0x112862+id*8,45);
        word(0x112864+id*8,0x80400000+id*17280);
    }
    for (unsigned selected=0; selected<4; ++selected) {
        word(0x112804,selected);
        auto results=read_ui(ram,49);
        assert(results.mode=="results" && results.token==0x80300000 && results.items.size()==4);
        for (unsigned id=0; id<4; ++id) {
            const auto& tab=results.items[id];
            assert(tab.group==6 && tab.selected==(id==selected) && !tab.activate && tab.horizontal);
            constexpr int layouts[][5]={{232,373,445,521,592},{232,284,445,521,592},
                {232,284,361,521,592},{232,284,361,443,592}};
            const auto& edges=layouts[selected];
            assert(tab.x0==edges[id] && tab.x1==edges[id+1]-1 && tab.y0==44 && tab.y1==72);
        }
    }
    for (unsigned screen=13; screen<=18; ++screen) {
        word(0xd1170,screen); assert(read_ui(ram,49).mode=="results");
    }
    word(0xd1170,19); assert(read_ui(ram,49).mode=="menu"); word(0xd1170,15);
    half(0x112860,383); assert(read_ui(ram,49).items.empty()); half(0x112860,384);
    word(0x12b4a4,0); assert(read_ui(ram,49).items.empty()); word(0x12b4a4,0x27bdffc0);
    word(0x112804,4); assert(read_ui(ram,49).items.empty()); word(0x112804,0);
    word(0x112864,0x807fffff); assert(read_ui(ram,49).items.empty());
    word(0x12c90c,0); assert(read_ui(ram,49).mode=="menu");
    word(0xd1170,0);
    sc64::runtime::FrontendFeedback main_frame;
    main_frame.main=sc64::runtime::MainMenuFeedback{2,true,-1,{}};
    sc64::runtime::UiItem hero;
    hero.group=5; hero.id=2; hero.context=2; hero.selected=true;
    hero.x0=240; hero.y0=220; hero.x1=360; hero.y1=440;
    main_frame.items.push_back(hero);
    auto main_ui=read_ui(ram,49,main_frame);
    assert(main_ui.main && main_ui.main->ready && main_ui.carousel_bounds && main_ui.items.size()==1);
    main_frame.main->warning=0x217;
    main_frame.main->warning_bounds=sc64::runtime::UiBounds{160,222,480,388,2};
    auto warning_ui=read_ui(ram,49,main_frame);
    assert(warning_ui.mode=="campaign_warning" && warning_ui.bounds && warning_ui.items.empty());
    assert(warning_ui.token!=(read_ui(ram,49).token));
    main_frame.main->campaign=3;
    assert(read_ui(ram,49,main_frame).token!=warning_ui.token);
    main_frame.main->warning=0x219;
    assert(read_ui(ram,49,main_frame).mode=="menu_warning");
    main_frame.main->warning_bounds->x1=700;
    assert(!read_ui(ram,49,main_frame).bounds);
    main_frame.main->warning=-1;
    assert(read_ui(ram,49,main_frame).items.empty()); // old campaign geometry cannot navigate
    assert(!read_ui(ram,49,main_frame).carousel_bounds);
    word(0xd1170,7); assert(read_ui(ram,49).mode=="loading");
    half(0xa5ab4,12); word(0xd1170,7); word(0x127db4,0x27bdf4b0);
    assert(read_ui(ram,49).mode=="briefing");
    half(0xa5ab4,4);
    sc64::runtime::FrontendFeedback briefing_frame; briefing_frame.briefing=true;
    assert(read_ui(ram,49,briefing_frame).mode=="briefing");
    assert(read_ui(ram,49).mode=="loading");
    half(0xa5ab4,12);
    word(0x127db4,0); assert(read_ui(ram,49).mode=="unknown"); // overlay identity required
    half(0xa5ab4,3); half(0xf0c20,0); ram[0xc6a64]=1; word(0xc6a68,0x80300000);
    word(0x300044,0x80300100); word(0x300100,0); word(0x300130,0x800723bc);
    half(0x300120,5); word(0xafe0c,4); word(0xafe9c,4);
    half(0x300004,232); half(0x300006,143); half(0x300008,319); half(0x30000a,223);
    auto build=read_ui(ram,50);
    assert(build.mode=="build" && build.items.size()==1 && build.items[0].selected);
    assert(build.items[0].id==4 && build.items[0].columns==3);
    assert(build.bounds && build.bounds->x0==232 && build.bounds->y1==223);
    // Build navigation can stop on a disabled cell, unlike ordinary menu
    // navigation. Preserve that selected item, but do not confirm its command.
    word(0x300118,0x1a); word(0xafe0c,0); word(0xafe9c,4);
    build=read_ui(ram,50);
    assert(build.items.size()==1 && build.items[0].selected && !build.items[0].activate);
    word(0x300118,0x8); // visible grid cells do not need the generic selectable flag
    assert(read_ui(ram,50).items.size()==1);
    word(0x300118,0x10); assert(read_ui(ram,50).items.empty()); // hidden cell stays hidden
    word(0x300118,0x18);
    half(0x300008,200); assert(!read_ui(ram,50).bounds); // malformed panel cannot close/replay
    // A sidecar from a prior selection must not become a clickable HUD target.
    ram[0xc6a64]=0;
    std::uint32_t stamp=2166136261u;
    for (unsigned i=0;i<84;++i) stamp*=16777619u; // all-zero selection/command fixture
    sc64::runtime::FrontendFeedback hud;
    sc64::runtime::UiItem hint;
    hint.group=4; hint.id=1; hint.buttons=2; hint.context=stamp;
    hint.x0=195; hint.y0=178; hint.x1=240; hint.y1=195;
    hud.items.push_back(hint);
    hud.hud_bounds=sc64::runtime::UiBounds{176,160,312,220,stamp};
    assert(read_ui(ram,51,hud).items.size()==1);
    assert(read_ui(ram,51,hud).bounds && read_ui(ram,51,hud).context==stamp);
    word(0xc6c18,0x80301000);
    assert(read_ui(ram,52,hud).items.empty());
    assert(!read_ui(ram,52,hud).bounds);
    word(0xc6c18,0); ram[0xafefc]=1;
    assert(read_ui(ram,52,hud).items.empty());
    ram[0xafefc]=0; hud.items[0].buttons=0x8000;
    assert(read_ui(ram,52,hud).items.empty()); // a corrupt C hint cannot issue A
    word(0xafdbc,std::bit_cast<std::uint32_t>(320.f));
    word(0xafdc4,std::bit_cast<std::uint32_t>(240.f));
    word(0xafdcc,(320u<<16)|240u);
    half(0xa5ab4,3); ram[0xafefc]=0;
    word(0xa2eb4,std::uint32_t(-32)); word(0xa2ebc,1440);
    word(0xa2eb8,0); word(0xa2ec0,1600);
    word(0xa2ee8,480); word(0xa2eec,288);
    word(0xd13d8,std::bit_cast<std::uint32_t>(0.f));
    auto camera=read_cursor(ram,60);
    assert(camera.camera_valid && camera.camera_x==240 && camera.camera_y==144);
    assert(camera.camera_min_x==-16 && camera.camera_max_y==800 && camera.camera_divisor==4);
    ram[0xafefc]=1; assert(!read_cursor(ram,61).camera_valid);
    ram[0xafefc]=0; word(0xa2ee8,1441); assert(!read_cursor(ram,62).camera_valid);
    word(0xa2ee8,480); word(0xd13d8,0x7fc00000); assert(!read_cursor(ram,63).camera_valid);

    AudioIntervalPeak source_timing;
    source_timing.observe(0,0); source_timing.observe(46'000'000,0);
    source_timing.observe(100'000'000,0);
    assert(source_timing.take_ms()==54 && source_timing.take_ms()==0);
    source_timing.observe(9'000'000'000,1); // Home/rate changes start a new baseline.
    assert(source_timing.take_ms()==0);
    source_timing.observe(9'010'000'000,1);
    assert(source_timing.take_ms()==10);
    AudioPeak concurrent_peak;
    auto peak_writer=std::async(std::launch::async,[&] {
        for (std::uint64_t i=1;i<=100000;++i) concurrent_peak.record(i);
    });
    double observed_peak=0;
    for (unsigned i=0;i<1000;++i) observed_peak=std::max(observed_peak,concurrent_peak.take_ms());
    peak_writer.get(); observed_peak=std::max(observed_peak,concurrent_peak.take_ms());
    assert(observed_peak==.1);

    PcmRing<3> pcm;
    pcm.push({1, 2}); pcm.push({3, 4}); pcm.push({5, 6});
    bool overflow = false;
    try { pcm.push({7, 8}); } catch (const std::runtime_error&) { overflow = true; }
    assert(overflow && pcm.size() == 3 && pcm[0][0] == 1);
    pcm.pop(); pcm.push({7, 8});
    assert(pcm.size() == 3 && pcm[0][1] == 4 && pcm[2][0] == 7);
    pcm.clear(); pcm.pop(); assert(pcm.empty());
    // Host playback must wait through short startup DMAs without draining
    // them. A real starvation re-enters pre-roll once; waiting is not an xrun.
    for (unsigned rate : {1000u, 22050u, 32000u, 48000u, 96000u}) {
        PcmPlayback<19202> playback;
        std::array<float,1024> output;
        auto append = [&](unsigned n) { for (unsigned i=0;i<n;++i) playback.push({.25f,-.5f}); };
        append(rate/50);
        for (unsigned i=0;i<10;++i) {
            auto r=playback.render(output,rate);
            assert(r.waiting && !r.starved && r.played==0);
            assert(playback.size()==rate/50);
            for (float value:output) assert(value==0);
        }
        append(rate/20+2);
        auto r=playback.render(output,rate);
        assert(!r.waiting && !r.starved && r.played==512 && r.nonzero==512);
        assert(output[0]>0 && output[0]<.25f); // ramp after pre-roll
        assert(output[1]<0 && output[1]>-.5f); // stereo order preserved
        bool starved=false;
        for (unsigned i=0;i<20 && !starved;++i) starved=playback.render(output,rate).starved;
        assert(starved && playback.buffering());
        append(rate/50);
        assert(playback.render(output,rate).waiting);
        append(rate/20+2);
        r=playback.render(output,rate);
        assert(!r.waiting && !r.starved && r.nonzero>0);
        for (float value:output) assert(std::isfinite(value) && std::abs(value)<=.5f);
        playback.clear(); assert(playback.size()==0 && playback.buffering());
    }

    // The consumer can drain published data even while the producer is parked.
    // Wrap and concurrent transfer verify sample/stream coherence and ordering.
    PcmInbox<3> small_inbox;
    assert(small_inbox.push({1,2,7}) && small_inbox.push({3,4,8}) && small_inbox.push({5,6,9}));
    assert(!small_inbox.push({7,8,10}));
    PcmFrame transferred;
    assert(small_inbox.pop(transferred) && transferred.left==1 && transferred.stream==7);
    assert(small_inbox.push({7,8,10}));
    for (unsigned i=1;i<4;++i) {
        assert(small_inbox.pop(transferred));
        assert(transferred.left==float(i*2+1) && transferred.stream==7+i);
    }
    assert(!small_inbox.pop(transferred) && small_inbox.pending()==0);
    PcmInbox<3> parked_inbox;
    std::promise<void> published, release_producer;
    auto release_signal=release_producer.get_future();
    std::mutex producer_mutex;
    auto parked_writer=std::async(std::launch::async,[&] {
        std::lock_guard producer_guard(producer_mutex);
        assert(parked_inbox.push({.25f,-.25f,3}));
        published.set_value(); release_signal.wait();
    });
    published.get_future().wait();
    assert(!producer_mutex.try_lock());
    assert(parked_inbox.pop(transferred) && transferred.stream==3);
    release_producer.set_value(); parked_writer.get();
    PcmInbox<17> concurrent_inbox;
    auto writer=std::async(std::launch::async,[&] {
        for (unsigned i=0;i<100000;++i)
            while (!concurrent_inbox.push({float(i),-float(i),std::uint64_t(i/123)+1})) std::this_thread::yield();
    });
    for (unsigned i=0;i<100000;++i) {
        while (!concurrent_inbox.pop(transferred)) std::this_thread::yield();
        assert(transferred.left==float(i) && transferred.right==-float(i));
        assert(transferred.stream==std::uint64_t(i/123)+1);
    }
    writer.get(); assert(concurrent_inbox.pending()==0);
    // Large DMA occupancy changes must be low-pass filtered, not a discontinuous
    // +/-0.5% playback-rate jump on each callback.
    PcmPlayback<19202> smoothed;
    std::array<float,1024> smooth_output;
    for (unsigned i=0;i<8000;++i) smoothed.push({.25f,.25f});
    auto smooth=smoothed.render(smooth_output,48000,6000);
    assert(smooth.played==512 && !smooth.starved);
    assert(smoothed.correction()>0 && smoothed.correction()<.00003);
    auto prior_correction=smoothed.correction();
    smooth=smoothed.render(smooth_output,48000,6000);
    assert(std::abs(smoothed.correction()-prior_correction)<.00003);
    smoothed.clear(); assert(smoothed.correction()==0);
    // A front-end transition raises the jitter reserve without dropping the
    // queued source music; recovery waits once, then continues the same stream.
    PcmPlayback<19202> menu_recovery;
    for (unsigned i=0;i<1024;++i) menu_recovery.push({.25f,-.25f});
    assert(menu_recovery.render(smooth_output,11025,1022).played==512);
    const auto retained=menu_recovery.size(); menu_recovery.rebuffer();
    assert(menu_recovery.size()==retained && menu_recovery.render(smooth_output,11025,1764).waiting);
    for (unsigned i=0;i<1024;++i) menu_recovery.push({.25f,-.25f});
    assert(menu_recovery.render(smooth_output,11025,1764).played==512 && !menu_recovery.buffering());
    auto menu_jitter=[&](unsigned target) {
        PcmPlayback<19202> player;
        unsigned packets=0,starvations=0;
        for (unsigned callback=0;callback<1400;++callback) {
            const double seconds=double(callback*512)/48000;
            while (double(packets*1024)/11025 + (packets%2 ? .008 : 0) <= seconds) {
                for (unsigned i=0;i<1024;++i) player.push({.25f,-.25f});
                ++packets;
            }
            starvations+=player.render(smooth_output,11025,target).starved;
        }
        return starvations;
    };
    assert(menu_jitter(1022)>0 && menu_jitter(1764)==0);

    PcmTraceLane diagnostic;
    diagnostic.initialize(3);
    diagnostic.append({42,7,1.5,0,0,0,11025},2,[](std::size_t i) { return float(i); });
    diagnostic.append({43,7,1.6,0,0,0,11025},2,[](std::size_t i) { return float(i+4); });
    assert(diagnostic.truncated() && diagnostic.samples().size()==6);
    assert(diagnostic.blocks().size()==2 && diagnostic.blocks()[1].offset==2);
    assert(diagnostic.blocks()[1].frames==1 && diagnostic.samples()[5]==5);
    diagnostic.append({44},2,[](std::size_t) { return 99.f; });
    assert(diagnostic.samples().size()==6 && diagnostic.blocks().size()==2);
    diagnostic.initialize(2);
    assert(diagnostic.samples().empty() && diagnostic.blocks().empty() && !diagnostic.truncated());
    diagnostic.append({45},3,[](std::size_t i) { return float(i); });
    assert(diagnostic.samples().size()==4 && diagnostic.blocks().size()==1 && diagnostic.truncated());

    std::array<std::int16_t,4104> signed_output; signed_output.fill(123);
    unsigned output_frames=0,calls=0;
    render_pcm16(std::span(signed_output).subspan(2,4100),[&](float* dst,unsigned frames) {
        assert(frames<=1024); output_frames+=frames; ++calls;
        for (unsigned i=0;i<frames;++i) { dst[2*i]=.25f; dst[2*i+1]=-.5f; }
    });
    assert(output_frames==2050 && calls==3 && signed_output[0]==123 && signed_output.back()==123);
    for (unsigned i=2;i<4102;i+=2) assert(signed_output[i]==8192 && signed_output[i+1]==-16384);
    render_pcm16(std::span(signed_output).subspan(2,2),[](float* dst,unsigned) { dst[0]=1.5f; dst[1]=-1.5f; });
    assert(signed_output[2]==32767 && signed_output[3]==-32768);

    FrameMailbox mailbox;
    assert(mailbox.take().empty());
    auto first = std::make_shared<sc64::gfx::DrawList>();
    first->identity.task = 1;
    first->initial_ram = {1, 2, 3};
    sc64::runtime::FrontendFeedback first_ui; first_ui.items.push_back({42});
    mailbox.publish(first,first_ui);
    auto taken = mailbox.take();
    assert(taken.size() == 1);
    const auto held = taken[0].frame;
    const auto held_ui = taken[0].ui;
    assert(mailbox.take().empty());
    first.reset();
    std::thread producer([&] {
        for (unsigned i = 2; i <= 1000; ++i) {
            auto packet = std::make_shared<sc64::gfx::DrawList>();
            packet->identity.task = i;
            mailbox.publish(packet);
        }
    });
    producer.join();
    const auto latest = mailbox.take(); // a consumer three behind keeps the newest three, in order
    assert(latest.size() == 3 && latest[0].frame->identity.task == 998 && latest[2].frame->identity.task == 1000);
    assert(held->identity.task == 1 && held->initial_ram[2] == 3);
    assert(held_ui.items.size()==1 && held_ui.items[0].id==42);

    // VI updates must blank a previously displayed frame_image without another DL.
    ScanoutMailbox vi_mailbox;
    std::array<std::uint32_t,14> regs{};
    regs[0]=2; regs[1]=0x100000+640; regs[2]=320;
    regs[9]=(108u<<16)|748; regs[10]=(37u<<16)|511;
    vi_mailbox.publish(regs,10,1);
    vi_mailbox.publish(regs,11,2);
    auto vi=vi_mailbox.latest();
    sc64::gfx::Image frame_image{0x100000,320,0,2};
    assert(scanout_matches(vi,frame_image,240,2,ram.size()));
    assert(!scanout_matches(vi,frame_image,240,3,ram.size())); // future RDP work
    regs[9]=0; vi_mailbox.publish(regs,12,2);
    assert(vi_mailbox.latest().blank());
    regs[9]=(108u<<16)|748; vi_mailbox.publish(regs,13,2);
    vi=vi_mailbox.latest(); // engine missed the blank field; invalidation persists
    assert(!scanout_matches(vi,frame_image,240,2,ram.size()));
    vi_mailbox.publish(regs,14,3); vi=vi_mailbox.latest();
    assert(scanout_matches(vi,frame_image,240,3,ram.size()));
    regs[2]=640; regs[1]=0x100000+1280; vi_mailbox.publish(regs,15,3);
    vi=vi_mailbox.latest();
    assert(!scanout_matches(vi,frame_image,240,3,ram.size()));
    frame_image.width=640;
    assert(!scanout_matches(vi,frame_image,480,3,ram.size())); // reused allocation/mode
    vi_mailbox.publish(regs,16,4); vi=vi_mailbox.latest();
    assert(scanout_matches(vi,frame_image,480,4,ram.size()));
    auto bad=vi; bad.regs[1]+=1;
    assert(!scanout_matches(bad,frame_image,480,4,ram.size()));
    bad=vi; bad.regs[1]=frame_image.address+1280*3;
    assert(!scanout_matches(bad,frame_image,480,4,ram.size()));
    assert(!scanout_matches(vi,frame_image,480,4,0x100100));
    assert(!scanout_matches(vi,frame_image,1025,4,ram.size()));
    ScanoutImages images;
    auto scanned=std::make_shared<ScanoutImage>();
    scanned->image=frame_image; scanned->height=480; scanned->task=4;
    scanned->cursor={true,100,80,200,400};
    scanned->ui.mode="menu"; scanned->ui.field=400; scanned->ui.token=256;
    scanned->ui.items.push_back({.id=2,.group=5,.x0=240,.y0=240,.x1=360,.y1=460,.selected=true});
    scanned->rgba={10,20,30,255}; images.add(scanned);
    auto offscreen=std::make_shared<ScanoutImage>(*scanned);
    offscreen->cursor.x=250; offscreen->cursor.field=401;
    offscreen->ui.mode="campaign_warning"; offscreen->ui.field=401; offscreen->ui.items.clear();
    offscreen->image.address=0x200000; images.add(offscreen);
    assert(images.select(vi)==scanned); // last CIMG is not the VI framebuffer
    assert(images.select(vi)->cursor.x==100 && images.select(vi)->cursor.field==400);
    assert(images.select(vi)->ui.mode=="menu" && images.select(vi)->ui.items.size()==1);
    auto future=std::make_shared<ScanoutImage>(*scanned); future->task=5;
    future->cursor.x=200; future->cursor.field=402;
    future->ui.mode="campaign_warning"; future->ui.field=402;
    future->ui.bounds=sc64::runtime::UiBounds{160,222,480,388,0};
    future->ui.items.clear();
    images.add(future);
    assert(images.select(vi)==scanned);
    assert(images.select(vi)->cursor.x==100 && images.select(vi)->ui.field==400);
    vi.completed_task=5; assert(images.select(vi)==future);
    assert(images.select(vi)->cursor.x==200 && images.select(vi)->cursor.field==402);
    assert(images.select(vi)->ui.mode=="campaign_warning" && images.select(vi)->ui.bounds->x0==160);
    assert(images.select(vi)->ui.items.empty());
    vi.regs[9]=0; assert(!images.select(vi));
    vi_mailbox.trace(17,20); vi_mailbox.publish(regs,17,5);
    assert(vi_mailbox.take_trace().size()==1 && vi_mailbox.take_trace().empty());

    // Recorded VI scales cover the whole native target, except the opening
    // animation: Y scale 716 covers 168 of 240 rows (ADR-068).
    {
        Scanout mode; mode.regs[12]=512; mode.regs[13]=1024;
        assert((vi_region(mode,320,240)==ViRegion{320,240}));      // 1P map
        mode.regs[12]=1024; mode.regs[13]=0x2000800;                 // offset bits ignored
        assert((vi_region(mode,640,480)==ViRegion{640,480}));      // menu/title/CPU logos
        mode.regs[12]=819; mode.regs[13]=1638;
        assert((vi_region(mode,512,384)==ViRegion{512,384}));      // split screen
        mode.regs[12]=512; mode.regs[13]=716;
        const auto intro=vi_region(mode,320,240);
        assert((intro==ViRegion{320,168}));
        mode.regs[13]=4095; assert(vi_region(mode,320,240).height==240); // clamped
        mode.regs[12]=0; mode.regs[13]=0; assert((vi_region(mode,320,240)==ViRegion{320,240}));
        std::vector<std::uint8_t> rows(320*240*4);
        for (unsigned y=0;y<240;++y) rows[std::size_t(y)*320*4]=std::uint8_t(y);
        assert(vi_present(rows,320,240,ViRegion{320,240})==rows);
        const auto scaled=vi_present(rows,320,240,intro);
        assert(scaled.size()==rows.size());
        assert(scaled[0]==0 && scaled[std::size_t(239)*320*4]==167); // last row shows row 167
        assert(scaled[std::size_t(10)*320*4]==7);                     // floor(10*168/240)
    }
    // CPU-written framebuffers: shown only for swaps without a newer RDP draw.
    {
        CpuScanouts cpu;
        assert(cpu.swapped(0x176c00,0));             // boot logo, no task drew it
        auto logo=std::make_shared<CpuScanout>();
        logo->image={0x176c00,640,0,2}; logo->height=480; logo->after_task=0;
        logo->canonical.assign(640*2*480,0);
        cpu.publish(logo);
        Scanout shown; shown.regs[0]=2; shown.regs[1]=0x176c00+1280; shown.regs[2]=640;
        shown.regs[9]=(108u<<16)|748; shown.regs[10]=(35u<<16)|509;
        shown.minimum_task=1; // swapped after task 0, at or after the VI transition
        assert(cpu.select(shown,0x800000)==logo);
        auto wrong=shown; wrong.regs[2]=320; assert(!cpu.select(wrong,0x800000));
        cpu.targeted(0x176c00,31);                   // fade task draws the buffer
        shown.completed_task=30;                     // not yet presentable: keep CPU bytes
        assert(cpu.select(shown,0x800000)==logo);
        shown.completed_task=31;                     // completed newer task wins
        assert(!cpu.select(shown,0x800000));
        assert(!cpu.swapped(0x176c00,31));           // first swap shows task 31
        assert(cpu.swapped(0x176c00,31));            // CPU redraw, no new task
        auto stale=std::make_shared<CpuScanout>(*logo); stale->after_task=30;
        cpu.publish(stale); assert(!cpu.select(shown,0x800000)); // older than task 31
        auto next=std::make_shared<CpuScanout>(*logo); next->after_task=31;
        cpu.publish(next); assert(cpu.select(shown,0x800000)==next);
        cpu.publish(std::make_shared<CpuScanout>(*next)); // unchanged repeated swap
        assert(cpu.select(shown,0x800000)==next);
        auto redrawn=std::make_shared<CpuScanout>(*next); redrawn->canonical[0]=1;
        cpu.publish(redrawn); assert(cpu.select(shown,0x800000)==redrawn);
        next=redrawn;
        assert(cpu.swapped(0x200000,31));            // other buffers are independent
        cpu.targeted(0x300000,32); assert(!cpu.swapped(0x300000,32));
        assert(cpu.select(shown,0x800000)==next);
        shown.minimum_task=33;                       // VI transition after task 32
        assert(!cpu.select(shown,0x800000));
        shown.minimum_task=32; assert(cpu.select(shown,0x800000)==next);
        for (std::uint32_t i=0;i<80;++i) cpu.targeted(0x500000+i*0x1000,100+i); // many targets
        assert(!cpu.swapped(0x500000+79*0x1000,200)); // recent targets survive bounding
        cpu.targeted(0x176c00,40); shown.completed_task=39;
        assert(cpu.select(shown,0x800000)==next);    // task 40 not complete yet
        assert(!cpu.swapped(0x176c00,40));           // task-drawn swap retires CPU image...
        assert(cpu.select(shown,0x800000)==next);    // ...only once task 40 completes
        for (std::uint32_t i=0;i<80;++i) cpu.targeted(0x600000+i*0x1000,300+i); // forget task 40
        shown.completed_task=40; assert(!cpu.select(shown,0x800000));
        shown.completed_task=31; next=nullptr;
        auto again=std::make_shared<CpuScanout>(*logo); again->after_task=41;
        assert(cpu.swapped(0x176c00,41)); cpu.publish(again);
        assert(cpu.select(shown,0x800000)==again);
        for (unsigned i=0;i<6;++i) {
            auto other=std::make_shared<CpuScanout>(*logo);
            other->image.address=0x400000+i*0x100000/8; other->after_task=50+i;
            cpu.publish(other);
        }
        assert(!cpu.select(shown,0x800000)); // bounded: oldest images are evicted
    }

    PauseClock clock;
    clock.set(true);
    auto frozen = clock.now();
    auto waiter = std::async(std::launch::async, [&] { clock.wait(); });
    assert(waiter.wait_for(20ms) == std::future_status::timeout);
    assert(clock.now() == frozen);
    clock.set(false);
    assert(waiter.wait_for(1s) == std::future_status::ready);
    assert(clock.now() - frozen < 20ms);
    clock.set(true);
    auto shutdown_waiter = std::async(std::launch::async, [&] { clock.wait(); });
    clock.stop();
    assert(shutdown_waiter.wait_for(1s) == std::future_status::ready);

    // Runtime's uint64_t VI index promotes duration representation to unsigned.
    // A deadline in the past must never become a multi-year positive sleep.
    std::uint64_t fields = 0;
    auto origin = PauseClock::Clock::now() - 500ms;
    auto unsigned_deadline = origin + (fields * 1000000us) / 60;
    auto deadline = PauseClock::Clock::time_point(
        std::chrono::duration_cast<PauseClock::Clock::duration>(unsigned_deadline.time_since_epoch()));
    assert(deadline - PauseClock::Clock::now() < 0ns);
}
