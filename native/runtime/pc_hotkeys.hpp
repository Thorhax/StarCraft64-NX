#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>
#include <utility>

// PC StarCraft hotkeys for the N64 command interface (ADR-091).
namespace sc64::runtime {
// Build-grid cells point to button records in the PC layout, whose enabled text
// (record +0x10) is a 1-based PC stat_txt.tbl string ID. PC button texts start
// with their hotkey letter; these are those letters ("Build Barracks" is 649, b).
inline constexpr std::pair<std::uint16_t, char> pc_button_hotkeys[] = {
    {324,'t'}, {325,'l'}, {326,'e'}, {327,'m'}, {328,'s'}, {329,'m'}, {330,'i'}, {331,'y'}, {332,'c'},
    {333,'c'}, {334,'t'}, {335,'l'}, {336,'i'}, {337,'s'}, {338,'o'}, {340,'d'}, {341,'e'}, {342,'i'},
    {343,'y'}, {344,'c'}, {364,'b'}, {365,'i'}, {366,'b'}, {367,'s'}, {368,'r'}, {369,'g'}, {370,'e'},
    {371,'c'}, {372,'u'}, {375,'b'}, {376,'w'}, {377,'r'}, {378,'g'}, {379,'c'}, {380,'u'}, {381,'e'},
    {395,'p'}, {396,'h'}, {397,'r'}, {398,'s'}, {399,'a'}, {400,'t'}, {401,'l'}, {402,'r'}, {403,'t'},
    {404,'r'}, {456,'a'}, {457,'p'}, {458,'h'}, {459,'c'}, {460,'c'}, {461,'a'}, {462,'a'}, {463,'w'},
    {464,'w'}, {465,'s'}, {466,'m'}, {467,'a'}, {468,'a'}, {469,'w'}, {470,'w'}, {471,'s'}, {472,'u'},
    {473,'i'}, {474,'b'}, {475,'t'}, {476,'o'}, {477,'m'}, {478,'a'}, {479,'c'}, {480,'v'}, {481,'a'},
    {482,'p'}, {483,'m'}, {484,'a'}, {485,'m'}, {486,'g'}, {487,'g'}, {488,'m'}, {489,'s'}, {490,'l'},
    {491,'s'}, {492,'c'}, {493,'g'}, {494,'s'}, {495,'g'}, {496,'k'}, {497,'a'}, {498,'g'}, {499,'c'},
    {500,'k'}, {575,'z'}, {576,'h'}, {577,'u'}, {578,'d'}, {579,'o'}, {580,'m'}, {581,'g'}, {582,'q'},
    {583,'f'}, {584,'s'}, {585,'i'}, {586,'m'}, {587,'g'}, {588,'f'}, {589,'v'}, {590,'g'}, {591,'t'},
    {592,'s'}, {593,'w'}, {594,'v'}, {595,'d'}, {596,'b'}, {597,'n'}, {598,'o'}, {599,'p'}, {600,'z'},
    {601,'d'}, {602,'t'}, {603,'s'}, {604,'s'}, {605,'a'}, {606,'c'}, {607,'i'}, {608,'v'}, {609,'r'},
    {610,'b'}, {611,'g'}, {612,'r'}, {613,'h'}, {614,'c'}, {615,'e'}, {616,'s'}, {617,'v'}, {618,'d'},
    {619,'n'}, {620,'s'}, {621,'q'}, {622,'u'}, {623,'d'}, {624,'l'}, {625,'h'}, {626,'g'}, {627,'s'},
    {628,'u'}, {630,'n'}, {631,'p'}, {632,'a'}, {633,'g'}, {634,'f'}, {635,'c'}, {636,'y'}, {637,'b'},
    {638,'r'}, {639,'o'}, {640,'c'}, {641,'t'}, {642,'s'}, {643,'f'}, {644,'a'}, {645,'b'}, {646,'c'},
    {647,'s'}, {648,'r'}, {649,'b'}, {650,'e'}, {651,'t'}, {652,'a'}, {653,'u'}, {654,'f'}, {655,'s'},
    {656,'i'}, {657,'a'}, {658,'c'}, {659,'n'}, {660,'c'}, {661,'c'}, {662,'p'}, {663,'c'}, {1245,'r'},
    {1246,'f'}, {1247,'a'}, {1248,'r'}, {1249,'f'}, {1253,'l'}, {1260,'d'}, {1261,'m'}, {1262,'f'},
    {1263,'e'}, {1264,'r'}, {1265,'d'}, {1266,'c'}, {1267,'f'}, {1268,'e'}, {1281,'d'}, {1282,'c'},
    {1283,'a'}, {1284,'c'}, {1285,'j'}, {1286,'t'}, {1288,'d'}, {1289,'l'}, {1290,'c'}, {1291,'y'},
    {1292,'o'}, {1293,'k'},
};
inline char hotkey_for_string(unsigned id) {
    const auto* end = std::end(pc_button_hotkeys);
    const auto* it = std::lower_bound(std::begin(pc_button_hotkeys), end, id,
                                      [](const auto& entry, unsigned key) { return entry.first < key; });
    return it != end && it->first == id ? it->second : 0;
}

// The labels of the C hints and the B hint (table 0x800A24FC / 0x800A2B1C) and
// the PC key of the same command.
inline char hotkey_for_label(std::string_view label) {
    static constexpr std::pair<std::string_view, char> labels[] = {
        {"Move", 'm'}, {"Stop", 's'}, {"Attack", 'a'}, {"Patrol", 'p'}, {"Hold", 'h'},
        {"Harvest", 'g'}, {"Return", 'c'}, {"Cargo return", 'c'}, {"Repair", 'r'},
        {"Build", 'b'}, {"build", 'b'}, {"BuildAdv", 'v'}, {"Set Rally", 'r'}, {"Rally", 'r'},
        {"Stimpack", 't'}, {"Lay Mine", 'i'}, {"siege", 'o'}, {"unsiege", 'o'}, {"Lockdown", 'l'},
        {"EMP", 'e'}, {"Irradiate", 'i'}, {"Defmatrix", 'd'}, {"Scanner", 's'}, {"Yamato", 'y'},
        {"Cloak", 'c'}, {"Decloak", 'c'}, {"Burrow", 'u'}, {"Unburrow", 'u'}, {"Liftoff", 'l'},
        {"Land", 'l'}, {"Load", 'l'}, {"Unload", 'u'}, {"Nuke", 'n'}, {"Heal", 'a'},
        {"Restore", 'r'}, {"Flare", 'f'}, {"Plague", 'g'}, {"Ensnare", 'e'}, {"Dark Swarm", 'w'},
        {"Parasite", 'r'}, {"Consume", 'c'}, {"Broodling", 'b'}, {"Infest", 'i'}, {"Psistorm", 't'},
        {"Mirage", 'l'}, {"Stasis", 't'}, {"Recall", 'r'}, {"Archon", 'r'}, {"Dark Arc", 'r'},
        {"Disruptor", 'd'}, {"Possess", 'c'}, {"FeedBack", 'f'}, {"Paralyze", 'e'},
        {"Select Larva", 's'}, {"Lurker", 'l'}, {"Guardian", 'g'}, {"Devourer", 'd'},
        {"Scarab", 'r'}, {"Intrcptr", 'i'}, {"Comsat", 'c'}, {"Silo", 'n'}, {"Contower", 'c'},
        {"CovertOp", 'c'}, {"Physics", 'p'}, {"Mac Shop", 'c'}, {"Lair", 'l'}, {"Hive", 'h'},
        {"G Spire", 'g'}, {"Bld Nuke", 'n'},
    };
    for (const auto& [text, key] : labels) if (text == label) return key;
    return 0;
}
// Hints that open a command grid (units, structures, research, upgrades) without
// a PC key of their own: a letter not on any hint is looked for inside them.
inline bool grid_label(std::string_view label) {
    for (std::string_view text : {"Build", "build", "BuildAdv", "Ground Units", "Air Units",
                                  "Tech", "Res/Upg", "Upgrades etc"})
        if (text == label) return true;
    return false;
}
}
