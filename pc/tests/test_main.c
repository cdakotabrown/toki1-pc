/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc/tests/test_main.c — minimal C test harness for HAL components.
 *
 * Each test_*() function returns 0 on success, non-zero on failure.
 * main() runs all tests and returns the count of failures (so CI can
 * fail on any non-zero exit).
 *
 * Adding a new test: write a function `test_xxx()`, add it to the
 * RUN(...) list in main(), declare it via `extern int test_xxx(void)`.
 */
#include <stdio.h>
#include <stdlib.h>

extern int test_palette_layout(void);
extern int test_model_layout(void);
extern int test_calendar_layout(void);
extern int test_option_rows(void);
extern int test_tt_animation(void);
extern int test_sprite_slot_anim(void);
extern int test_gs_ot_width(void);
extern int test_gs_object_width(void);
extern int test_mdec_native(void);
extern int test_mdec_rgb24(void);
extern int test_mdec_vlc_eob(void);
extern int test_mdec_truncated_codes(void);
extern int test_file_id_lba(void);
extern int test_nbn_toc(void);
extern int test_pad_boot_as_pad(void);
extern int test_pad_mouse_detect_and_move(void);
extern int test_pad_mouse_clamp_and_carry(void);
extern int test_pad_mouse_buttons(void);
extern int test_pad_mouse_switch_and_card_busy(void);
extern int test_pad_mouse_policy(void);
extern int test_pad_mouse_helpers(void);
extern int test_pad_mouse_absolute(void);
extern int test_pad_port2(void);
extern int test_settings_defaults(void);
extern int test_settings_parse(void);
extern int test_settings_overrides_and_layout(void);
extern int test_settings_load_create_and_harness(void);
extern int test_settings_binding_remap(void);
extern int test_present_scaling_math(void);
extern int test_present_png_window_state(void);
extern int test_settings_state_section(void);
extern int test_settings_menu_keys(void);
extern int test_settings_edit_text(void);
extern int test_settings_save_keys(void);
extern int test_settings_menu_model(void);
extern int test_settings_menu_notes(void);
extern int test_pad_hold_off(void);
extern int test_save_browser_pages(void);
extern int test_save_browser_actions(void);
extern int test_save_browser_resume_prompt(void);
extern int test_save_settings_and_png_read(void);
extern int test_status_keys(void);
extern int test_controls_legend(void);
extern int test_pad_map(void);
extern int test_rewind_codec(void);
extern int test_rewind_chain(void);
extern int test_seen_text_set(void);
extern int test_assist_settings(void);
extern int test_crt_presets_and_settings(void);
extern int test_crt_scanlines(void);
extern int test_crt_mask_pattern(void);
extern int test_crt_brightness_compensation(void);
extern int test_crt_reference_and_glow(void);
extern int test_crt_warp_and_vignette(void);
extern int test_translation_sha1(void);
extern int test_translation_apply(void);
extern int test_translation_prepare(void);
extern int test_lang_japanese_is_noop(void);
extern int test_ui_utf8_and_glyphs(void);
extern int test_ui_text_measure(void);
extern int test_ui_text_fit(void);
extern int test_ui_text_wrap(void);
extern int test_ui_text_spans(void);
extern int test_lang_setting(void);

static int g_total = 0;
static int g_failed = 0;

#define RUN(test) do { \
    g_total++; \
    printf("[TEST] %-30s ", #test); \
    fflush(stdout); \
    int r = test(); \
    if (r == 0) printf("PASS\n"); \
    else { printf("FAIL (%d)\n", r); g_failed++; } \
} while (0)

/* Tests that read the retail disc's files (pc/assets/raw).  CI has no disc,
   so with PC_TESTS_NO_DISC=1 they are EXCLUDED, said out loud, and not
   counted; locally they run, and fail without the assets. */
static int g_excluded = 0;
#define RUN_DISC(test) do { \
    const char *nd_ = getenv("PC_TESTS_NO_DISC"); \
    if (nd_ && nd_[0] == '1') { \
        printf("[TEST] %-30s EXCLUDED (needs the retail disc)\n", #test); \
        g_excluded++; \
    } else RUN(test); \
} while (0)

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    RUN(test_palette_layout);
    RUN(test_model_layout);
    RUN(test_calendar_layout);
    RUN(test_option_rows);
    RUN(test_tt_animation);
    RUN(test_sprite_slot_anim);
    RUN(test_gs_ot_width);
    RUN(test_gs_object_width);
    RUN(test_mdec_vlc_eob);
    RUN(test_mdec_truncated_codes);
    RUN_DISC(test_mdec_native);
    RUN_DISC(test_mdec_rgb24);
    RUN_DISC(test_file_id_lba);
    RUN_DISC(test_nbn_toc);
    RUN(test_pad_boot_as_pad);
    RUN(test_pad_mouse_detect_and_move);
    RUN(test_pad_mouse_clamp_and_carry);
    RUN(test_pad_mouse_buttons);
    RUN(test_pad_mouse_switch_and_card_busy);
    RUN(test_pad_mouse_policy);
    RUN(test_pad_mouse_helpers);
    RUN(test_pad_mouse_absolute);
    RUN(test_pad_port2);
    RUN(test_settings_defaults);
    RUN(test_settings_parse);
    RUN(test_settings_overrides_and_layout);
    RUN(test_settings_load_create_and_harness);
    RUN(test_settings_binding_remap);
    RUN(test_present_scaling_math);
    RUN(test_present_png_window_state);
    RUN(test_settings_state_section);
    RUN(test_settings_menu_keys);
    RUN(test_settings_edit_text);
    RUN(test_settings_save_keys);
    RUN(test_settings_menu_model);
    RUN(test_settings_menu_notes);
    RUN(test_pad_hold_off);
    RUN(test_save_browser_pages);
    RUN(test_save_browser_actions);
    RUN(test_save_browser_resume_prompt);
    RUN(test_save_settings_and_png_read);
    RUN(test_status_keys);
    RUN(test_controls_legend);
    RUN(test_pad_map);
    RUN(test_rewind_codec);
    RUN(test_rewind_chain);
    RUN(test_seen_text_set);
    RUN(test_assist_settings);
    RUN(test_crt_presets_and_settings);
    RUN(test_crt_scanlines);
    RUN(test_crt_mask_pattern);
    RUN(test_crt_brightness_compensation);
    RUN(test_crt_reference_and_glow);
    RUN(test_crt_warp_and_vignette);
    RUN(test_translation_sha1);
    RUN(test_translation_apply);
    RUN(test_translation_prepare);
    RUN(test_lang_japanese_is_noop);
    RUN(test_lang_setting);
    RUN(test_ui_utf8_and_glyphs);
    RUN(test_ui_text_measure);
    RUN(test_ui_text_fit);
    RUN(test_ui_text_wrap);
    RUN(test_ui_text_spans);

    printf("\n[TEST] %d/%d passed, %d failed\n",
           g_total - g_failed, g_total, g_failed);
    if (g_excluded)
        printf("[TEST] %d excluded (PC_TESTS_NO_DISC=1: they need the retail disc)\n",
               g_excluded);
    return g_failed;
}
