// Offline checks only. No MMIO or physical link training.
#include "../NootedGreen/IntelDPLinkTraining.hpp"
#include <cassert>
#include <cstddef>
#include <cstdio>

static_assert(sizeof(TGLComboBufTransEntry) == 5, "Intel ICL table layout");
static_assert(offsetof(TGLComboBufTransEntry, dw2_swing_sel) == 0, "swing first");
static_assert(offsetof(TGLComboBufTransEntry, dw7_n_scalar) == 1, "N scalar second");
static_assert(offsetof(TGLComboBufTransEntry, dw4_cursor_coeff) == 2, "cursor third");
static_assert(offsetof(TGLComboBufTransEntry, dw4_post_cursor_2) == 3, "post2 fourth");
static_assert(offsetof(TGLComboBufTransEntry, dw4_post_cursor_1) == 4, "post1 fifth");

int main() {
    uint8_t swings[4] = {}, pres[4] = {};
    assert(!tgl_combo_phy_inputs_valid(4, nullptr, pres));
    assert(!tgl_combo_phy_inputs_valid(4, swings, nullptr));
    for (unsigned count = 0; count < 256; ++count)
        assert(tgl_combo_phy_inputs_valid(count, swings, pres) ==
               (count == 1 || count == 2 || count == 4));
    for (unsigned lane = 0; lane < 4; ++lane) {
        for (unsigned swing = 0; swing < 256; ++swing)
        for (unsigned pre = 0; pre < 256; ++pre) {
            swings[lane] = swing;
            pres[lane] = pre;
            assert(tgl_combo_phy_inputs_valid(4, swings, pres) == (swing + pre <= 3));
        }
        swings[lane] = pres[lane] = 0;
    }
    // Independent expected register payload from Linux TGL HBR level 0.
    const auto &first = tgl_combo_phy_trans_dp_hbr[0];
    assert((SWING_SEL_UPPER(first.dw2_swing_sel) |
            SWING_SEL_LOWER(first.dw2_swing_sel) | RCOMP_SCALAR_VAL) == 0x9098);
    assert(N_SCALAR(first.dw7_n_scalar) == 0x32000000);
    assert((CURSOR_COEFF(first.dw4_cursor_coeff) |
            POST_CURSOR_1(first.dw4_post_cursor_1) |
            POST_CURSOR_2(first.dw4_post_cursor_2)) == 0x3F);
    const TGLComboBufTransEntry *tables[] = {
        tgl_combo_phy_trans_dp_hbr, tgl_combo_phy_trans_dp_hbr2,
        adlp_combo_phy_trans_dp_hbr, adlp_combo_phy_trans_dp_hbr2,
        adlp_combo_phy_trans_edp_hbr2,
    };
    for (auto table : tables) for (unsigned i = 0; i < 10; ++i) {
        const auto &e = table[i];
        assert(e.dw2_swing_sel <= 0xF && e.dw7_n_scalar <= 0x7F);
        assert(e.dw4_cursor_coeff <= 0x3F && e.dw4_post_cursor_1 <= 0x3F);
        assert(e.dw4_post_cursor_2 == 0);
        assert(e.dw4_cursor_coeff + e.dw4_post_cursor_1 == 0x3F);
    }
    for (unsigned value = 0; value < 256; ++value)
        assert((SWING_SEL_UPPER(value) & ~SWING_SEL_UPPER_MASK) == 0);
    unsigned valid = 0;
    bool seen[10] = {};
    for (unsigned swing = 0; swing < 256; ++swing)
    for (unsigned pre = 0; pre < 256; ++pre) {
        int level = tgl_combo_phy_trans_index(swing, pre);
        assert(level >= 0 && level < 10);
        if (swing + pre <= 3) {
            assert(!seen[level]);
            seen[level] = true;
            ++valid;
        } else assert(level == 0);
    }
    assert(valid == 10);
    std::puts("PASS: 50 PHY table rows, register field layout/masks, 65536 level pairs");
}
