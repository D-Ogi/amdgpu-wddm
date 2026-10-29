/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
// The FSR 4 identity answer: the policy (amd_identity_policy.h, docs/design/fsr4-identity.md).
#include "amd_identity_policy.h"
#include "../contract/bc250_caps_query.h"

// Game images whose anti-cheat software protects the process that would load the FSR DLL. They always get the
// BC-250's own identity. The _BE files are BattlEye's launchers of the same games; a match costs nothing. Each
// entry names its source (checked 2026-10-29):
static const char* const g_AntiCheat[] = {
    // Counter-Strike 2, VAC: runs in its default mode only and must not see a changed driver answer.
    "cs2.exe",
    // Apex Legends, Easy Anti-Cheat. The EAC launcher file SettingsDX12.json names r5apex_dx12.exe
    // (https://steamcommunity.com/app/1172470/discussions/0/3882724699532093109).
    "r5apex.exe", "r5apex_dx12.exe",
    // Fortnite, Easy Anti-Cheat (https://www.epicgames.com/help/c-36624475/a20611857,
    // https://www.ggrecon.com/guides/does-fortnite-have-anti-cheat/).
    "FortniteClient-Win64-Shipping.exe",
    // PUBG: BATTLEGROUNDS, BattlEye in TslGame\Win64\BattlEye
    // (https://support.pubg.com/hc/en-us/articles/360034901793, https://www.freefixer.com/library/file/TslGame_BE.exe-272158).
    "TslGame.exe", "TslGame_BE.exe",
    // Rainbow Six Siege, BattlEye: RainbowSix.exe starts RainbowSix_BE.exe
    // (https://steamcommunity.com/app/359550/discussions/0/604158686972091019/,
    // https://devtrackers.gg/r6-siege/p/0c5bcec5-rainbow-six-siege-not-launching).
    "RainbowSix.exe", "RainbowSix_BE.exe",
    // Escape from Tarkov, BattlEye (https://steamcommunity.com/app/3932890/discussions/1/687491010005083052/,
    // https://lutris.net/games/install/39686/view).
    "EscapeFromTarkov.exe", "EscapeFromTarkov_BE.exe",
    // Destiny 2, BattlEye from update 3.3.0; its log names destiny2.exe
    // (https://steamcommunity.com/app/1085660/discussions/0/3034850940605979441).
    "destiny2.exe",
    // VALORANT, Riot Vanguard (https://www.file.net/process/valorant-win64-shipping.exe.html; a weak source, and a
    // D3D11 game, so this entry is only a safety net).
    "VALORANT-Win64-Shipping.exe",
};
#define ANTI_CHEAT_COUNT (sizeof(g_AntiCheat) / sizeof(g_AntiCheat[0]))

static unsigned short Lower(unsigned short c)
{
    return (c >= 'A' && c <= 'Z') ? (unsigned short)(c - 'A' + 'a') : c;
}

// name[0..chars) against an ASCII entry, without case for A-Z.
static int SameName(const unsigned short* name, unsigned int chars, const char* entry)
{
    unsigned int i;
    for (i = 0; i < chars; i++) {
        if (!entry[i] || Lower(name[i]) != Lower((unsigned char)entry[i])) return 0;
    }
    return entry[chars] == 0;
}

unsigned int Bc250AmdIdentityNameFlags(const unsigned short* name, unsigned int chars)
{
    unsigned int i, start = 0, end = 0, flags;
    if (!name) return 0;
    while (end < chars && name[end]) end++;
    if (!end) return 0;
    flags = BC250_AMD_IDENTITY_NAME_KNOWN;
    for (i = 0; i < end; i++) {
        if (name[i] == '\\' || name[i] == '/') start = i + 1;
    }
    for (i = 0; i < ANTI_CHEAT_COUNT; i++) {
        if (SameName(name + start, end - start, g_AntiCheat[i])) return flags | BC250_AMD_IDENTITY_NAME_LISTED;
    }
    return flags;
}

int Bc250AmdIdentityDecide(int has_process, unsigned int name_flags, unsigned int shell, unsigned int* source)
{
    unsigned int s;
    int on;
    if (!has_process) { s = BC250_AMD_IDENTITY_SOURCE_NO_PROCESS; on = 0; }
    else if (name_flags & BC250_AMD_IDENTITY_NAME_LISTED) { s = BC250_AMD_IDENTITY_SOURCE_LIST; on = 0; }
    else if (shell == BC250_AMD_IDENTITY_SHELL_ON || shell == BC250_AMD_IDENTITY_SHELL_OFF) {
        s = BC250_AMD_IDENTITY_SOURCE_SHELL;
        on = shell == BC250_AMD_IDENTITY_SHELL_ON;
    }
    else if (!(name_flags & BC250_AMD_IDENTITY_NAME_KNOWN)) { s = BC250_AMD_IDENTITY_SOURCE_NO_NAME; on = 0; }
    else { s = BC250_AMD_IDENTITY_SOURCE_DEFAULT; on = 1; }
    if (source) *source = s;
    return on;
}

int Bc250AmdIdentityShellWord(unsigned int fsr_identity, unsigned int image_flags, unsigned int* word)
{
    unsigned int shell;
    if (!word) return 0;
    if (fsr_identity == BC250_AMD_IDENTITY_FSR_OFF) shell = BC250_AMD_IDENTITY_SHELL_OFF;
    else if (fsr_identity == BC250_AMD_IDENTITY_FSR_ON) shell = BC250_AMD_IDENTITY_SHELL_ON;
    else if (fsr_identity == BC250_AMD_IDENTITY_FSR_UNSET) shell = BC250_AMD_IDENTITY_SHELL_NONE;
    else return 0;
    *word = shell | ((image_flags & 0xFFu) << 8);
    return 1;
}

static void Put32(unsigned char* p, unsigned int v)
{
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

int Bc250AmdIdentityFill(void* out, unsigned int bytes, int on)
{
    unsigned char* p = (unsigned char*)out;
    unsigned int i;
    if (!out || !Bc250AmdIdentityExtent(bytes)) return 0;
    for (i = 0; i < bytes; i++) p[i] = 0;
    Put32(p + BC250_AMD_IDENTITY_FAMILY_OFFSET, BC250_AMD_IDENTITY_FAMILY_NV);
    Put32(p + BC250_AMD_IDENTITY_EREV_OFFSET, on ? BC250_AMD_IDENTITY_EREV_NAVI22 : BC250_AMD_IDENTITY_EREV_OWN);
    return 1;
}

const char* Bc250AmdIdentitySourceName(unsigned int source)
{
    switch (source) {
    case BC250_AMD_IDENTITY_SOURCE_DEFAULT: return "default";
    case BC250_AMD_IDENTITY_SOURCE_LIST: return "list";
    case BC250_AMD_IDENTITY_SOURCE_SHELL: return "shell";
    case BC250_AMD_IDENTITY_SOURCE_NO_NAME: return "no-name";
    case BC250_AMD_IDENTITY_SOURCE_NO_PROCESS: return "no-process";
    default: return "?";
    }
}

unsigned int Bc250AmdIdentityListCount(void)
{
    return (unsigned int)ANTI_CHEAT_COUNT;
}

const char* Bc250AmdIdentityListEntry(unsigned int index)
{
    return index < ANTI_CHEAT_COUNT ? g_AntiCheat[index] : 0;
}
