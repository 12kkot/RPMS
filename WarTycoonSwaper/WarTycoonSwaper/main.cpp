#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <intrin.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <algorithm>
#include <filesystem>
#include <chrono>
#include <optional>
#include <fstream>
#include <iomanip>

namespace fs = std::filesystem;

struct BitR {
    const uint8_t* p; const uint8_t* end;
    uint64_t buf = 0; int bits = 0;
    BitR(const uint8_t* s, const uint8_t* e) : p(s), end(e) {}
    uint32_t peek(int n) {
        while (bits < n && p < end) { buf |= (uint64_t)*p++ << bits; bits += 8; }
        return (uint32_t)(buf & ((1ULL << n) - 1));
    }
    void drop(int n) { buf >>= n; bits -= n; }
    uint32_t read(int n) { uint32_t r = peek(n); drop(n); return r; }
    void align() { int s = bits & 7; drop(s); }
};

struct HuffNode { uint16_t ch[2]; bool leaf; };
struct HuffTree {
    std::vector<HuffNode> nodes;
    void build(const uint16_t* lens, int n) {
        struct SL { uint16_t sym; uint16_t len; } sorted[288];
        int ns = 0;
        for (int i = 0; i < n; i++) if (lens[i] && lens[i] <= 15) { sorted[ns].sym = (uint16_t)i; sorted[ns].len = lens[i]; ns++; }
        for (int i = 0; i < ns; i++) for (int j = i + 1; j < ns; j++)
            if (sorted[j].len < sorted[i].len || (sorted[j].len == sorted[i].len && sorted[j].sym < sorted[i].sym))
                std::swap(sorted[i], sorted[j]);

        nodes.clear(); nodes.push_back({ 0,0,false });
        if (ns == 0) { nodes[0].leaf = true; return; }
        int code = 0, cur_len = 1;
        for (int i = 0; i < ns; i++) {
            while (cur_len < sorted[i].len) { code <<= 1; cur_len++; }
            int idx = 0;
            for (int b = cur_len - 1; b >= 0; b--) {
                int bit = (code >> b) & 1;
                if (nodes[idx].ch[bit] == 0) {
                    nodes[idx].ch[bit] = (uint16_t)nodes.size();
                    nodes.push_back({ 0,0,false });
                }
                idx = nodes[idx].ch[bit];
            }
            nodes[idx].ch[0] = sorted[i].sym;
            nodes[idx].leaf = true;
            code++;
        }
    }

    int decode(BitR& br) {
        int idx = 0, depth = 0;
        while (!nodes[idx].leaf) {
            int bit = (int)br.read(1);
            if (nodes[idx].ch[bit] == 0) return -1;
            idx = nodes[idx].ch[bit];
            if ((size_t)idx >= nodes.size()) return -1;
            if (++depth > 255) return -1;
        }
        return nodes[idx].ch[0];
    }
};

bool inflate_raw(const uint8_t* in, size_t in_sz, uint8_t* out, size_t out_sz, size_t& out_pos) {
    BitR br(in, in + in_sz); out_pos = 0;

    static const int len_base[] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
    static const int len_extra[] = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
    static const int dst_base[] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
    static const int dst_extra[] = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

    uint16_t fix_lit[288], fix_dst[32];
    for (int i = 0; i < 144; i++) fix_lit[i] = 8;
    for (int i = 144; i < 256; i++) fix_lit[i] = 9;
    for (int i = 256; i < 280; i++) fix_lit[i] = 7;
    for (int i = 280; i < 288; i++) fix_lit[i] = 8;
    for (int i = 0; i < 32; i++) fix_dst[i] = 5;

    while (true) {
        int bfinal = (int)br.read(1);
        int btype = (int)br.read(2);

        if (btype == 0) {
            br.align();
            uint16_t len = (uint16_t)br.read(16);
            uint16_t nlen = (uint16_t)br.read(16);
            if ((len ^ nlen) != 0xFFFF) return false;
            for (int i = 0; i < len; i++) {
                if (out_pos >= out_sz) return false;
                out[out_pos++] = (uint8_t)br.read(8);
            }
        }
        else {
            HuffTree hl, hd;
            uint16_t lens[320], dst[32];
            if (btype == 1) {
                hl.build(fix_lit, 288);
                hd.build(fix_dst, 32);
            }
            else if (btype == 2) {
                int nl = (int)br.read(5) + 257;
                int nd = (int)br.read(5) + 1;
                int ncl = (int)br.read(4) + 4;
                static const int cl_order[] = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
                uint16_t cl_lens[19] = {};
                for (int i = 0; i < ncl; i++) cl_lens[cl_order[i]] = (uint16_t)br.read(3);

                HuffTree hcl; hcl.build(cl_lens, 19);

                int total = nl + nd;
                for (int i = 0; i < total; ) {
                    int sym = hcl.decode(br);
                    if (sym < 0) return false;
                    if (sym < 16) { lens[i] = (uint16_t)sym; i++; }
                    else if (sym == 16) {
                        int rep = (int)br.read(2) + 3;
                        if (i == 0) return false;
                        for (int j = 0; j < rep && i < total; j++) { lens[i] = lens[i - 1]; i++; }
                    }
                    else if (sym == 17) {
                        int rep = (int)br.read(3) + 3;
                        for (int j = 0; j < rep && i < total; j++) { lens[i] = 0; i++; }
                    }
                    else {
                        int rep = (int)br.read(7) + 11;
                        for (int j = 0; j < rep && i < total; j++) { lens[i] = 0; i++; }
                    }
                }
                memcpy(dst, lens + nl, nd * sizeof(uint16_t));
                hl.build(lens, nl);
                hd.build(dst, nd);
            }
            else return false;

            while (true) {
                int sym = hl.decode(br);
                if (sym < 0) return false;
                if (sym < 256) {
                    if (out_pos >= out_sz) return false;
                    out[out_pos++] = (uint8_t)sym;
                }
                else if (sym == 256) break;
                else {
                    sym -= 257;
                    int len = len_base[sym] + (int)br.read(len_extra[sym]);
                    int dsym = hd.decode(br);
                    if (dsym < 0) return false;
                    int dist = dst_base[dsym] + (int)br.read(dst_extra[dsym]);
                    if (dist <= 0 || dist > (int)out_pos) return false;
                    for (int i = 0; i < len; i++) {
                        if (out_pos >= out_sz) return false;
                        out[out_pos] = out[out_pos - dist];
                        out_pos++;
                    }
                }
            }
        }
        if (bfinal) break;
    }
    return true;
}

enum Color { GRAY = 8, RED = 12, GREEN = 10, YELLOW = 14, CYAN = 11, WHITE = 15 };

void setc(Color c) {
    SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), (WORD)c);
}

template<typename... Args>
void pf(Color c, const char* fmt, Args... args) {
    setc(c); printf(fmt, args...); setc(GRAY); fflush(stdout);
}

// ============================================================
// ZIP READER
// ============================================================

#pragma pack(push, 1)
struct LocalFileHdr {
    uint32_t sig; uint16_t ver, flags, comp, mod_t, mod_d;
    uint32_t crc, comp_sz, uncomp_sz;
    uint16_t fn_len, xtra_len;
};
struct CDEntry {
    uint32_t sig; uint16_t ver_m, ver_n, flags, comp, mod_t, mod_d;
    uint32_t crc, comp_sz, uncomp_sz;
    uint16_t fn_len, xtra_len, cmt_len, disk;
    uint16_t int_attr; uint32_t ext_attr, lh_offset;
};
struct EOCD64Loc {
    uint32_t sig; uint32_t disk_w64; uint64_t eocd64_off; uint32_t total_disks;
};
struct EOCD64 {
    uint32_t sig; uint64_t rec_sz; uint16_t ver_m, ver_n;
    uint32_t disk, disk_cd; uint64_t nentry, ntotal;
    uint64_t cd_sz, cd_off;
};
struct EOCD {
    uint32_t sig; uint16_t dnum, dcd, nentry, ntotal;
    uint32_t cd_sz, cd_off; uint16_t cmt_len;
};
#pragma pack(pop)

class ZipReader {
    std::vector<uint8_t> buf;
    struct Entry {
        std::string name;
        uint64_t comp_sz, uncomp_sz, lh_off;
        uint16_t comp;
    };
    std::vector<Entry> entries;
public:
    bool open(const std::string& path) {
        HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER li; GetFileSizeEx(h, &li);
        if (li.QuadPart > 0x7FFFFFFF) { CloseHandle(h); return false; }
        buf.resize((size_t)li.QuadPart);
        DWORD rd = 0;
        if (!ReadFile(h, buf.data(), (DWORD)buf.size(), &rd, nullptr) || rd != buf.size())
        {
            CloseHandle(h); return false;
        }
        CloseHandle(h);
        return parse_central_dir();
    }

    std::optional<std::vector<uint8_t>> read(const std::string& name) {
        const uint8_t* base = buf.data();
        size_t bsz = buf.size();
        for (auto& e : entries) {
            if (e.name != name) continue;
            if (e.comp_sz > bsz || e.comp_sz > 0x7FFFFFFF || e.uncomp_sz > 0x7FFFFFFF) {
                pf(RED, "    -> bad sizes, skipping\n"); return std::nullopt;
            }
            if (e.lh_off + sizeof(LocalFileHdr) > bsz) return std::nullopt;
            auto* lh = (LocalFileHdr*)(base + (size_t)e.lh_off);
            if (lh->sig != 0x04034b50) { pf(RED, "    -> bad lh sig\n"); return std::nullopt; }
            size_t data_off = (size_t)e.lh_off + sizeof(LocalFileHdr) + lh->fn_len + lh->xtra_len;
            if (data_off > bsz || e.comp_sz > bsz - data_off) {
                pf(RED, "    -> data out of bounds\n"); return std::nullopt;
            }
            const uint8_t* src = base + data_off;
            std::vector<uint8_t> result((size_t)e.uncomp_sz);
            if (lh->comp == 0) {
                memcpy(result.data(), src, (size_t)e.uncomp_sz);
            }
            else if (lh->comp == 8) {
                size_t out_pos = 0;
                if (!inflate_raw(src, (size_t)e.comp_sz, result.data(), (size_t)e.uncomp_sz, out_pos))
                    return std::nullopt;
                result.resize(out_pos);
            }
            else return std::nullopt;
            return result;
        }
        pf(RED, "    entry '%s' not found in zip\n", name.c_str());
        return std::nullopt;
    }

private:
    bool parse_central_dir() {
        const uint8_t* base = buf.data();
        size_t bsz = buf.size();
        if (bsz < 22) return false;

        int64_t eocd_pos = (int64_t)bsz - 22;
        int64_t search_limit = (std::max)((int64_t)0, eocd_pos - 65535);
        while (eocd_pos >= search_limit) {
            if (*(uint32_t*)(base + eocd_pos) == 0x06054b50) break;
            eocd_pos--;
        }
        if (eocd_pos < search_limit) return false;

        auto* eocd = (EOCD*)(base + eocd_pos);
        uint64_t cd_off = eocd->cd_off;
        uint64_t ntotal = eocd->ntotal;

        if (cd_off == 0xFFFFFFFF || ntotal == 0xFFFF) {
            if (eocd_pos >= 20) {
                int64_t loc_pos = eocd_pos - 20;
                if (*(uint32_t*)(base + loc_pos) == 0x07064b50) {
                    auto* loc = (EOCD64Loc*)(base + loc_pos);
                    if ((size_t)loc->eocd64_off + sizeof(EOCD64) <= bsz) {
                        auto* e64 = (EOCD64*)(base + (size_t)loc->eocd64_off);
                        if (e64->sig == 0x06064b50) {
                            cd_off = e64->cd_off;
                            ntotal = e64->ntotal;
                        }
                    }
                }
            }
        }

        uint64_t off = cd_off;
        for (uint64_t i = 0; i < ntotal; i++) {
            if (off + sizeof(CDEntry) > bsz) return false;
            auto* cd = (CDEntry*)(base + (size_t)off);
            if (cd->sig != 0x02014b50) return false;

            uint64_t fn_end = off + sizeof(CDEntry) + cd->fn_len;
            if (fn_end + cd->xtra_len > bsz) return false;
            std::string fn((const char*)(base + (size_t)off + sizeof(CDEntry)), cd->fn_len);

            uint64_t cs = cd->comp_sz, us = cd->uncomp_sz, lo = cd->lh_offset;
            if (cs == 0xFFFFFFFF || us == 0xFFFFFFFF || lo == 0xFFFFFFFF) {
                const uint8_t* xtra = base + fn_end;
                uint16_t xrem = cd->xtra_len;
                while (xrem >= 4) {
                    uint16_t hdr, xsz;
                    memcpy(&hdr, xtra, 2);
                    memcpy(&xsz, xtra + 2, 2);
                    if (hdr == 0x0001) {
                        const uint8_t* xp = xtra + 4;
                        uint16_t idx = 0;
                        if (us == 0xFFFFFFFF && xsz - idx >= 8) { memcpy(&us, xp + idx, 8); idx += 8; }
                        if (cs == 0xFFFFFFFF && xsz - idx >= 8) { memcpy(&cs, xp + idx, 8); idx += 8; }
                        if (lo == 0xFFFFFFFF && xsz - idx >= 8) { memcpy(&lo, xp + idx, 8); idx += 8; }
                        break;
                    }
                    xtra += 4 + xsz;
                    xrem -= 4 + xsz;
                }
            }

            entries.push_back({ std::move(fn), cs, us, lo, cd->comp });
            off += sizeof(CDEntry) + cd->fn_len + cd->xtra_len + cd->cmt_len;
        }
        return !entries.empty();
    }
};

// ============================================================
// VARINT
// ============================================================

int32_t rvi(const uint8_t*& p, const uint8_t* end) {
    int32_t r = 0; int s = 0;
    while (p < end) {
        uint8_t b = *p++; r |= (int32_t)(b & 0x7F) << s;
        if (!(b & 0x80)) return r; s += 7;
        if (s > 35) return 0;
    }
    return r;
}

// ============================================================
// PALETTE
// ============================================================

std::vector<int32_t> read_pal(const uint8_t*& p, const uint8_t* end, bool block) {
    if (p >= end) return {};
    int bits = *p++;
    int sz = block ? 4096 : 64;
    int imax = block ? 8 : 3;

    if (bits == 0) {
        int32_t v = rvi(p, end);
        return std::vector<int32_t>(sz, v);
    }

    std::vector<int32_t> pal;
    if (bits <= imax) {
        int32_t n = rvi(p, end);
        if (n < 0 || n > 10000) return std::vector<int32_t>(sz, 0);
        pal.reserve(n);
        for (int32_t i = 0; i < n && p < end; i++) pal.push_back(rvi(p, end));
    }

    if (bits > 64) return std::vector<int32_t>(sz, 0);
    int epl = 64 / bits;
    int alen = (sz + epl - 1) / epl;
    uint64_t mask = bits >= 64 ? ~(uint64_t)0 : ((uint64_t)1 << bits) - 1;

    std::vector<int32_t> res; res.reserve(sz);
    for (int i = 0; i < alen && p + 8 <= end; i++) {
        uint64_t v; memcpy(&v, p, 8); p += 8;
        v = _byteswap_uint64(v);
        for (int j = 0; j < epl && (int)res.size() < sz; j++) {
            int shift = j * bits;
            if (shift >= 64) break;
            int32_t val = (int32_t)((v >> shift) & mask);
            res.push_back(pal.empty() ? val : ((size_t)val < pal.size() ? pal[val] : 0));
        }
    }
    while ((int)res.size() < sz) res.push_back(0);
    return res;
}

// ============================================================
// STATE ID MAP
// ============================================================

std::unordered_map<int32_t, std::string> SIM;

void add_range(int32_t s, int32_t e, const std::string& n) {
    for (int32_t i = s; i <= e; i++) SIM[i] = n;
}

void init_sim() {
    add_range(24968, 24968, "minecraft:sculk");
    add_range(24488, 24583, "minecraft:sculk_sensor");
    add_range(25099, 25106, "minecraft:sculk_shrieker");
    add_range(29390, 29390, "minecraft:reinforced_deepslate");
    add_range(29455, 29466, "minecraft:trial_spawner");
    add_range(29467, 29498, "minecraft:vault");
    add_range(25107, 25107, "minecraft:copper_block");
    add_range(25116, 25116, "minecraft:cut_copper");
    add_range(24073, 24073, "minecraft:tuff_bricks");
    add_range(24072, 24072, "minecraft:chiseled_tuff");
    add_range(6537, 6537, "minecraft:stone_bricks");
    add_range(6538, 6538, "minecraft:mossy_stone_bricks");
    add_range(6539, 6539, "minecraft:cracked_stone_bricks");
    add_range(6540, 6540, "minecraft:chiseled_stone_bricks");
    add_range(12494, 12494, "minecraft:end_stone_bricks");
    add_range(12410, 12410, "minecraft:purpur_block");
    add_range(12411, 12413, "minecraft:purpur_pillar");
    add_range(12414, 12493, "minecraft:purpur_stairs");
    add_range(7272, 7272, "minecraft:nether_bricks");
    add_range(12545, 12545, "minecraft:red_nether_bricks");
    add_range(7273, 7304, "minecraft:nether_brick_fence");
    add_range(7305, 7384, "minecraft:nether_brick_stairs");
    add_range(19460, 19460, "minecraft:blackstone");
    add_range(19871, 19871, "minecraft:polished_blackstone");
    add_range(20285, 20285, "minecraft:gilded_blackstone");
    add_range(19448, 19448, "minecraft:ancient_debris");
    add_range(19459, 19459, "minecraft:lodestone");
    add_range(10463, 10463, "minecraft:prismarine");
    add_range(10464, 10464, "minecraft:prismarine_bricks");
    add_range(10465, 10465, "minecraft:dark_prismarine");
    add_range(10724, 10724, "minecraft:sea_lantern");
    add_range(535, 537, "minecraft:sandstone");
    add_range(11307, 11307, "minecraft:smooth_sandstone");
    add_range(9357, 9357, "minecraft:orange_terracotta");
    add_range(9367, 9367, "minecraft:blue_terracotta");
    add_range(2353, 2353, "minecraft:mossy_cobblestone");
    add_range(21, 21, "minecraft:dark_oak_planks");
    add_range(148, 150, "minecraft:dark_oak_log");
    add_range(10044, 10123, "minecraft:dark_oak_stairs");
    add_range(113, 116, "minecraft:suspicious_sand");
    add_range(119, 122, "minecraft:suspicious_gravel");
    add_range(6541, 6541, "minecraft:packed_mud");
    add_range(6542, 6542, "minecraft:mud_bricks");
    add_range(21031, 21031, "minecraft:amethyst_block");
    add_range(21032, 21032, "minecraft:budding_amethyst");
    add_range(22316, 22316, "minecraft:calcite");
    add_range(26557, 26557, "minecraft:smooth_basalt");
    add_range(12546, 12548, "minecraft:bone_block");
    add_range(6741, 6772, "minecraft:iron_bars");
    add_range(7407, 7414, "minecraft:end_portal_frame");
    add_range(2354, 2354, "minecraft:obsidian");
    add_range(19449, 19449, "minecraft:crying_obsidian");
    add_range(5849, 5849, "minecraft:netherrack");
    add_range(2096, 2096, "minecraft:bookshelf");
    add_range(2873, 2873, "minecraft:spawner");
    add_range(2094, 2095, "minecraft:tnt");
}

// ============================================================
// PRESETS
// ============================================================

struct Preset { std::string label; std::unordered_set<std::string> blocks; };

Preset PRESETS[] = {
    {"Trial Chamber",  {"minecraft:trial_spawner", "minecraft:vault", "minecraft:copper_block",
                        "minecraft:cut_copper", "minecraft:tuff_bricks", "minecraft:chiseled_tuff"}},
    {"Deep Dark",      {"minecraft:sculk", "minecraft:sculk_sensor", "minecraft:sculk_shrieker",
                        "minecraft:reinforced_deepslate"}},
    {"Ancient City",   {"minecraft:reinforced_deepslate", "minecraft:sculk_shrieker"}},
    {"Stronghold",     {"minecraft:stone_bricks", "minecraft:mossy_stone_bricks",
                        "minecraft:cracked_stone_bricks", "minecraft:chiseled_stone_bricks",
                        "minecraft:end_portal_frame", "minecraft:iron_bars", "minecraft:bookshelf"}},
    {"End City",       {"minecraft:purpur_block", "minecraft:purpur_pillar", "minecraft:purpur_stairs",
                        "minecraft:end_stone_bricks"}},
    {"Nether Fortress", {"minecraft:nether_bricks", "minecraft:red_nether_bricks",
                        "minecraft:nether_brick_fence", "minecraft:nether_brick_stairs"}},
    {"Bastion Remnant", {"minecraft:blackstone", "minecraft:polished_blackstone",
                        "minecraft:gilded_blackstone", "minecraft:ancient_debris", "minecraft:lodestone"}},
    {"Ocean Monument",  {"minecraft:prismarine", "minecraft:dark_prismarine",
                        "minecraft:prismarine_bricks", "minecraft:sea_lantern"}},
    {"Desert Pyramid",  {"minecraft:chiseled_sandstone", "minecraft:blue_terracotta",
                        "minecraft:orange_terracotta", "minecraft:tnt"}},
    {"Jungle Temple",   {"minecraft:mossy_cobblestone", "minecraft:chiseled_stone_bricks"}},
    {"Dungeon",         {"minecraft:mossy_cobblestone", "minecraft:spawner"}},
    {"Woodland Mansion", {"minecraft:dark_oak_planks", "minecraft:dark_oak_stairs",
                        "minecraft:dark_oak_log"}},
    {"Geode",           {"minecraft:amethyst_block", "minecraft:budding_amethyst",
                        "minecraft:calcite", "minecraft:smooth_basalt"}},
    {"Ruined Portal",   {"minecraft:netherrack", "minecraft:obsidian", "minecraft:crying_obsidian"}},
    {"Trail Ruins",     {"minecraft:suspicious_gravel", "minecraft:packed_mud",
                        "minecraft:mud_bricks"}},
    {"Shipwreck",       {"minecraft:suspicious_sand"}},
    {"Fossil",          {"minecraft:bone_block"}}
};
const int NUM_PRESETS = sizeof(PRESETS) / sizeof(PRESETS[0]);

// ============================================================
// PACKET ITERATOR
// ============================================================

struct Packet { const uint8_t* data; size_t size; };

class PacketIter {
    const uint8_t* p, * end;
public:
    PacketIter(const uint8_t* b, const uint8_t* e) : p(b), end(e) {}
    std::optional<Packet> next() {
        if (p + 8 > end) return std::nullopt;
        uint32_t ts, len;
        memcpy(&ts, p, 4); memcpy(&len, p + 4, 4); p += 8;
        len = _byteswap_ulong(len);
        if (len == 0 || p + len > end) return std::nullopt;
        auto pk = Packet{ p, (size_t)len }; p += len;
        return pk;
    }
};

// ============================================================
// CHUNK PARSE
// ============================================================

struct BPos {
    int32_t x, y, z;
    bool operator==(const BPos& o) const { return x == o.x && y == o.y && z == o.z; }
};
struct BPHash {
    size_t operator()(const BPos& p) const {
        return ((size_t)p.x * 73856093) ^ ((size_t)p.y * 19349663) ^ ((size_t)p.z * 83492791);
    }
};

struct ChunkPos {
    int32_t x, z;
    bool operator==(const ChunkPos& o) const { return x == o.x && z == o.z; }
};
struct CPHash {
    size_t operator()(const ChunkPos& p) const {
        return ((size_t)p.x * 73856093) ^ ((size_t)p.z * 83492791);
    }
};

const int32_t WORLD_MIN_Y = -64;
const int CK_MIN = 0x20, CK_MAX = 0x3F;

void parse_chunk(const uint8_t* payload, size_t size,
    const std::unordered_set<std::string>& targets,
    std::unordered_map<std::string, std::unordered_set<BPos, BPHash>>& found,
    std::unordered_map<std::string, std::unordered_set<ChunkPos, CPHash>>& chunk_found)
{
    const uint8_t* p = payload;
    const uint8_t* end = p + size;
    if (p + 8 > end) return;

    int32_t cx, cz;
    memcpy(&cx, p, 4); memcpy(&cz, p + 4, 4);
    cx = (int32_t)_byteswap_ulong((uint32_t)cx);
    cz = (int32_t)_byteswap_ulong((uint32_t)cz);
    p += 8;
    rvi(p, end); // dimension type

    for (int i = 0; i < 3 && p < end; i++) {
        if (p >= end) break;
        p++;
        int32_t n = rvi(p, end);
        if (n < 0 || n > 0x100000) return;
        p += n * 8;
    }
    if (p >= end || p < payload) return;

    int32_t slen = rvi(p, end);
    if (slen <= 0 || slen > 0x100000) return;
    if (p + slen > end) return;

    const uint8_t* sp = p, * se = sp + slen;
    int sidx = 0;

    while (sp < se) {
        if (se - sp < 2) break;
        sp += 2;
        auto blk = read_pal(sp, se, true);
        if (blk.empty()) break;

        int yb = WORLD_MIN_Y + sidx * 16;
        for (int i = 0; i < (int)blk.size(); i++) {
            auto it = SIM.find(blk[i]);
            if (it == SIM.end() || !targets.count(it->second)) continue;
            BPos pos{ cx * 16 + (i & 15), yb + ((i >> 8) & 15), cz * 16 + ((i >> 4) & 15) };
            found[it->second].insert(pos);
            chunk_found[it->second].insert({ cx, cz });
        }
        read_pal(sp, se, false);
        sidx++;
        if (sidx > 24) break;
    }
}

// ============================================================
// SCAN
// ============================================================

static bool safe_zip_open(ZipReader* zip, const std::string& path) {
    return zip->open(path);
}

static bool safe_zip_read(ZipReader* zip, const std::string& name, std::vector<uint8_t>& out) {
    auto r = zip->read(name);
    if (!r) return false;
    out = std::move(*r);
    return true;
}

void run_scan(const std::string& path,
    const std::unordered_set<std::string>& targets,
    std::unordered_map<std::string, std::unordered_set<BPos, BPHash>>& found,
    std::unordered_map<std::string, std::unordered_set<ChunkPos, CPHash>>& chunk_found,
    const std::string& file_label = "")
{
    if (!file_label.empty()) pf(CYAN, "\n[%s]\n", file_label.c_str());
    pf(CYAN, "  Opening zip...\n"); fflush(stdout);
    ZipReader zip;
    if (!safe_zip_open(&zip, path)) { pf(RED, "Error: Cannot open file\n"); return; }
    pf(CYAN, "  Reading recording.tmcpr...\n"); fflush(stdout);
    std::vector<uint8_t> data;
    if (!safe_zip_read(&zip, "recording.tmcpr", data)) {
        pf(RED, "Error: Cannot read recording.tmcpr\n"); return;
    }
    pf(CYAN, "  Parsing packets...\n"); fflush(stdout);

    int packets = 0, chunks = 0;
    auto last = std::chrono::steady_clock::now();

    PacketIter it(data.data(), data.data() + data.size());
    while (auto pkt = it.next()) {
        packets++;
        const uint8_t* pkp = pkt->data;
        const uint8_t* pkend = pkp + pkt->size;
        int32_t pid = rvi(pkp, pkend);
        if (pid < CK_MIN || pid > CK_MAX) continue;
        chunks++;
        parse_chunk(pkp, pkend - pkp, targets, found, chunk_found);

        if (packets % 5000 == 0) {
            size_t tf = 0;
            for (auto& [_, s] : found) tf += s.size();
            auto now = std::chrono::steady_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count();
            if (ms > 0) pf(GRAY, "  packets: %d  chunks: %d  found: %zu  (%d pkt/s)\n",
                packets, chunks, tf, (int)(5000 * 1000 / ms));
            last = now;
        }
    }
    pf(GREEN, "  done. %d packets, %d chunks.\n", packets, chunks);
}

void run_scan_multiple(const std::vector<std::string>& paths,
    const std::unordered_set<std::string>& targets,
    std::unordered_map<std::string, std::unordered_set<BPos, BPHash>>& found,
    std::unordered_map<std::string, std::unordered_set<ChunkPos, CPHash>>& chunk_found)
{
    for (size_t i = 0; i < paths.size(); i++) {
        std::string label = "File " + std::to_string(i + 1) + "/" + std::to_string(paths.size());
        run_scan(paths[i], targets, found, chunk_found, label);
    }
}

// ============================================================
// STRUCTURE
// ============================================================

struct Structure {
    std::string name;
    std::unordered_set<ChunkPos, CPHash> chunks;
    BPos min_pos{ INT32_MAX, INT32_MAX, INT32_MAX };
    BPos max_pos{ INT32_MIN, INT32_MIN, INT32_MIN };
    // Какие типы блоков найдены внутри этой структуры
    std::unordered_set<std::string> block_types;
};

// ============================================================
// MERGE — все блоки всех типов объединяются в единые структуры
// ============================================================

void merge_all_into_structures(
    const std::unordered_map<std::string, std::unordered_set<ChunkPos, CPHash>>& chunk_found,
    const std::unordered_map<std::string, std::unordered_set<BPos, BPHash>>& found,
    std::vector<Structure>& structures,
    const std::string& struct_label)
{
    // 1. Собираем ВСЕ чанки из всех типов блоков в один пул
    std::unordered_set<ChunkPos, CPHash> all_chunks;
    for (const auto& [block_name, chunks] : chunk_found)
        for (const auto& cp : chunks)
            all_chunks.insert(cp);

    if (all_chunks.empty()) return;

    // 2. BFS по всему пулу чанков без разделения по типу блока
    std::unordered_set<ChunkPos, CPHash> visited;

    for (const auto& start : all_chunks) {
        if (visited.count(start)) continue;

        std::unordered_set<ChunkPos, CPHash> struct_chunks;
        std::vector<ChunkPos> queue;
        queue.push_back(start);
        struct_chunks.insert(start);
        visited.insert(start);

        while (!queue.empty()) {
            ChunkPos cur = queue.back();
            queue.pop_back();

            // 8-связность (включая диагонали)
            ChunkPos neighbors[] = {
                {cur.x - 1, cur.z},     {cur.x + 1, cur.z},
                {cur.x,     cur.z - 1}, {cur.x,     cur.z + 1},
                {cur.x - 1, cur.z - 1}, {cur.x + 1, cur.z - 1},
                {cur.x - 1, cur.z + 1}, {cur.x + 1, cur.z + 1}
            };
            for (const auto& nb : neighbors) {
                if (all_chunks.count(nb) && !visited.count(nb)) {
                    visited.insert(nb);
                    struct_chunks.insert(nb);
                    queue.push_back(nb);
                }
            }
        }

        // 3. Считаем bounding box по ВСЕМ блокам всех типов в этих чанках
        Structure s;
        s.name = struct_label;
        s.chunks = struct_chunks;

        for (const auto& [block_name, positions] : found) {
            bool block_type_present = false;
            for (const auto& pos : positions) {
                // Правильное деление для отрицательных координат
                int32_t pcx = pos.x >> 4;
                int32_t pcz = pos.z >> 4;
                if (!struct_chunks.count({ pcx, pcz })) continue;

                block_type_present = true;
                if (pos.x < s.min_pos.x) s.min_pos.x = pos.x;
                if (pos.y < s.min_pos.y) s.min_pos.y = pos.y;
                if (pos.z < s.min_pos.z) s.min_pos.z = pos.z;
                if (pos.x > s.max_pos.x) s.max_pos.x = pos.x;
                if (pos.y > s.max_pos.y) s.max_pos.y = pos.y;
                if (pos.z > s.max_pos.z) s.max_pos.z = pos.z;
            }
            if (block_type_present)
                s.block_types.insert(block_name);
        }

        if (s.min_pos.x == INT32_MAX) continue; // пустая — пропускаем
        structures.push_back(s);
    }
}

// ============================================================
// ВЫВОД СТРУКТУР
// ============================================================

void show_structures(const std::vector<Structure>& structures) {
    if (structures.empty()) { pf(RED, "No structures found.\n"); return; }

    // Группируем по имени (preset label)
    std::map<std::string, std::vector<const Structure*>> grouped;
    for (const auto& s : structures)
        grouped[s.name].push_back(&s);

    pf(CYAN, "\n=== FOUND STRUCTURES ===\n");

    for (auto& [name, list] : grouped) {
        // Сортируем по центру X
        std::sort(list.begin(), list.end(), [](const Structure* a, const Structure* b) {
            int ca = (a->min_pos.x + a->max_pos.x) / 2;
            int cb = (b->min_pos.x + b->max_pos.x) / 2;
            return ca < cb;
            });

        pf(YELLOW, "\n%s: %zu structure(s)\n", name.c_str(), list.size());

        for (size_t i = 0; i < list.size(); i++) {
            const auto& s = *list[i];
            int cx = (s.min_pos.x + s.max_pos.x) / 2;
            int cy = (s.min_pos.y + s.max_pos.y) / 2;
            int cz = (s.min_pos.z + s.max_pos.z) / 2;
            int sx = s.max_pos.x - s.min_pos.x + 1;
            int sy = s.max_pos.y - s.min_pos.y + 1;
            int sz = s.max_pos.z - s.min_pos.z + 1;

            pf(GRAY, "  Structure #%zu:\n", i + 1);
            pf(GRAY, "    Chunks: %zu\n", s.chunks.size());
            pf(GRAY, "    Bounding Box:\n");
            pf(GRAY, "      Min:    X=%-6d Y=%-6d Z=%d\n", s.min_pos.x, s.min_pos.y, s.min_pos.z);
            pf(GRAY, "      Max:    X=%-6d Y=%-6d Z=%d\n", s.max_pos.x, s.max_pos.y, s.max_pos.z);
            pf(GRAY, "      Center: X=%-6d Y=%-6d Z=%d\n", cx, cy, cz);
            pf(GRAY, "      Size:   %dx%dx%d\n", sx, sy, sz);

            // Показываем какие типы блоков внутри
            if (!s.block_types.empty()) {
                std::vector<std::string> types(s.block_types.begin(), s.block_types.end());
                std::sort(types.begin(), types.end());
                pf(GRAY, "    Blocks: ");
                for (size_t j = 0; j < types.size(); j++) {
                    if (j > 0) printf(", ");
                    // Убираем "minecraft:" префикс для краткости
                    const std::string& t = types[j];
                    size_t colon = t.find(':');
                    printf("%s", colon != std::string::npos ? t.c_str() + colon + 1 : t.c_str());
                }
                printf("\n");
            }
        }
    }

    size_t total = 0;
    for (auto& [_, list] : grouped) total += list.size();
    pf(GREEN, "\nTOTAL STRUCTURES: %zu\n", total);
}

// ============================================================
// СОХРАНЕНИЕ В ФАЙЛ
// ============================================================

void save_structures_to_file(const std::vector<Structure>& structures,
    const std::string& filename,
    const std::vector<std::string>& source_files,
    const std::string& scan_preset)
{
    std::ofstream file(filename);
    if (!file.is_open()) {
        pf(RED, "Error: Cannot open file for writing: %s\n", filename.c_str());
        return;
    }

    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    file << "# MCPR Structure Scan Results\n";
    file << "# Generated: " << std::put_time(std::localtime(&time), "%Y-%m-%d %H:%M:%S") << "\n";
    file << "# Source: ";
    for (size_t i = 0; i < source_files.size(); i++) {
        if (i > 0) file << ", ";
        file << source_files[i];
    }
    file << "\n";
    file << "# Preset: " << scan_preset << "\n\n";

    std::map<std::string, std::vector<const Structure*>> grouped;
    for (const auto& s : structures)
        grouped[s.name].push_back(&s);

    size_t total = 0;
    for (auto& [name, list] : grouped) {
        std::sort(list.begin(), list.end(), [](const Structure* a, const Structure* b) {
            return (a->min_pos.x + a->max_pos.x) < (b->min_pos.x + b->max_pos.x);
            });

        file << "[" << name << "] " << list.size() << " structure(s)\n";
        for (size_t i = 0; i < list.size(); i++) {
            const auto& s = *list[i];
            int cx = (s.min_pos.x + s.max_pos.x) / 2;
            int cy = (s.min_pos.y + s.max_pos.y) / 2;
            int cz = (s.min_pos.z + s.max_pos.z) / 2;
            int sx = s.max_pos.x - s.min_pos.x + 1;
            int sy = s.max_pos.y - s.min_pos.y + 1;
            int sz = s.max_pos.z - s.min_pos.z + 1;

            file << "  S" << (i + 1) << " | chunks=" << s.chunks.size();
            file << " | min=(" << s.min_pos.x << "," << s.min_pos.y << "," << s.min_pos.z << ")";
            file << " | max=(" << s.max_pos.x << "," << s.max_pos.y << "," << s.max_pos.z << ")";
            file << " | center=(" << cx << "," << cy << "," << cz << ")";
            file << " | size=(" << sx << "x" << sy << "x" << sz << ")";

            // Типы блоков
            if (!s.block_types.empty()) {
                std::vector<std::string> types(s.block_types.begin(), s.block_types.end());
                std::sort(types.begin(), types.end());
                file << " | blocks=";
                for (size_t j = 0; j < types.size(); j++) {
                    if (j > 0) file << ",";
                    const std::string& t = types[j];
                    size_t colon = t.find(':');
                    file << (colon != std::string::npos ? t.substr(colon + 1) : t);
                }
            }
            file << "\n";
        }
        file << "\n";
        total += list.size();
    }

    file << "# Total structures: " << total << "\n";
    file.close();
    pf(GREEN, "Results saved to: %s\n", filename.c_str());
}

// ============================================================
// MAIN
// ============================================================

LONG WINAPI CrashHandler(EXCEPTION_POINTERS* ep) {
    pf(RED, "\n*** CRASH: code=0x%08X at=0x%p ***\n",
        ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress);
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
        pf(RED, "    %s 0x%p\n",
            ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
            (void*)ep->ExceptionRecord->ExceptionInformation[1]);
    }
    fflush(stdout);
    return EXCEPTION_EXECUTE_HANDLER;
}

int main() {
    SetUnhandledExceptionFilter(CrashHandler);
    init_sim();
    HANDLE hc = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_CURSOR_INFO ci; GetConsoleCursorInfo(hc, &ci);
    ci.bVisible = FALSE; SetConsoleCursorInfo(hc, &ci);
    system("cls");

    pf(CYAN, "MCPR Scanner - C++ Edition\n\n");
    pf(WHITE, "MCPR file(s): "); setc(GRAY);
    std::string path_input; std::getline(std::cin, path_input);
    path_input.erase(0, path_input.find_first_not_of(" \t\r\n"));
    path_input.erase(path_input.find_last_not_of(" \t\r\n") + 1);
    if (path_input.empty()) return 0;

    // Парсим несколько файлов через запятую
    std::vector<std::string> paths;
    std::string current;
    for (char c : path_input) {
        if (c == ',') {
            if (!current.empty()) {
                size_t start = current.find_first_not_of(" \t");
                size_t end = current.find_last_not_of(" \t");
                if (start != std::string::npos && end != std::string::npos)
                    paths.push_back(current.substr(start, end - start + 1));
                current.clear();
            }
        }
        else {
            current += c;
        }
    }
    if (!current.empty()) {
        size_t start = current.find_first_not_of(" \t");
        size_t end = current.find_last_not_of(" \t");
        if (start != std::string::npos && end != std::string::npos)
            paths.push_back(current.substr(start, end - start + 1));
    }

    std::vector<std::string> valid_paths;
    for (const auto& p : paths) {
        if (fs::exists(p)) valid_paths.push_back(p);
        else pf(RED, "File not found: %s\n", p.c_str());
    }
    if (valid_paths.empty()) { system("pause"); return 1; }

    pf(WHITE, "Select preset:\n");
    pf(GRAY, "  [Favorites]\n");
    pf(YELLOW, "  1. Trial Chamber *\n");
    pf(YELLOW, "  2. Deep Dark *\n");
    pf(GRAY, "  [All Structures]\n");
    for (int i = 2; i < NUM_PRESETS; i++) pf(YELLOW, "  %d. %s\n", i + 1, PRESETS[i].label.c_str());
    pf(YELLOW, "  %d. ALL PRESETS\n", NUM_PRESETS + 1);
    pf(WHITE, "Choice [Enter=Trial Chamber]: "); setc(GRAY);
    std::string cs; std::getline(std::cin, cs);
    int choice = 1;
    if (!cs.empty()) { try { choice = std::stoi(cs); } catch (...) {} }
    if (choice < 1 || choice > NUM_PRESETS + 1) choice = 1;

    bool scan_all = (choice == NUM_PRESETS + 1);
    std::vector<int> preset_indices;
    if (scan_all) {
        for (int i = 0; i < NUM_PRESETS; i++) preset_indices.push_back(i);
    }
    else {
        preset_indices.push_back(choice - 1);
    }

    pf(GRAY, "\nFiles: %zu\n", valid_paths.size());
    if (scan_all) {
        pf(GRAY, "Preset: ALL (%d presets)\n", NUM_PRESETS);
    }
    else {
        auto& preset = PRESETS[preset_indices[0]];
        pf(GRAY, "Preset: %s\n", preset.label.c_str());
        std::string ts;
        std::vector<std::string> st(preset.blocks.begin(), preset.blocks.end());
        std::sort(st.begin(), st.end());
        for (auto& s : st) { if (!ts.empty()) ts += ", "; ts += s; }
        pf(GRAY, "Targets: %s\n", ts.c_str());
    }
    pf(GRAY, "\n");

    std::vector<Structure> all_structures;
    auto start_time = std::chrono::steady_clock::now();

    for (int idx : preset_indices) {
        auto& preset = PRESETS[idx];
        pf(CYAN, "\n=== Scanning: %s ===\n", preset.label.c_str());

        std::unordered_map<std::string, std::unordered_set<BPos, BPHash>> found;
        std::unordered_map<std::string, std::unordered_set<ChunkPos, CPHash>> chunk_found;

        if (valid_paths.size() == 1) {
            run_scan(valid_paths[0], preset.blocks, found, chunk_found);
        }
        else {
            run_scan_multiple(valid_paths, preset.blocks, found, chunk_found);
        }

        // Объединяем все блоки всех типов в единые структуры
        std::vector<Structure> structures;
        merge_all_into_structures(chunk_found, found, structures, preset.label);
        all_structures.insert(all_structures.end(), structures.begin(), structures.end());
    }

    auto end_time = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

    show_structures(all_structures);

    std::string output_filename = "scan_results.txt";
    std::string preset_name = scan_all ? "ALL PRESETS" : PRESETS[preset_indices[0]].label;
    save_structures_to_file(all_structures, output_filename, valid_paths, preset_name);

    pf(GRAY, "\nTime: %lld ms\n", (long long)ms);

    ci.bVisible = TRUE; SetConsoleCursorInfo(hc, &ci);
    system("pause");
    return 0;
}