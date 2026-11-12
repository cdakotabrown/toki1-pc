/* missing_funcs.c — Auto-generated stubs for unresolved functions */
/* These need real implementations as the PC port progresses */
#include "types.h"

/* get_CD_State2 is get_CD_State2 (0x80046358; overlay_load_wait still
 * spells the Ghidra name).  It was an empty `void` stub, so the overlay
 * loader's `get_CD_State2() & 1` tested whatever was left in %eax. */
/* draw_and_disp_env_init — aliased to draw_and_disp_env_init in lowercase_aliases.c */
/* print_dr_env in pc-port */
/* print_di_env in pc-port */
/* sprite_init in src/main/semi-matched/ */
/* bg_init in src/main/semi-matched/ */
/* game_system_init in pc-port */
/* game_work_init1 in src/main/semi-matched/ */
/* game_work_init2 in pc-port */
/* func_80042660 in src/main/semi-matched/ */
/* sub_sub_step_up in pc-port */
/* sub_step_up — aliased to sub_step_up in lowercase_aliases.c */
/* sub_sub_step_up — aliased to sub_sub_step_up in lowercase_aliases.c */
/* goto_sub_sub_step in pc-port */
/* step_state_save_and_redirect in src/main/semi-matched/ */
/* pad_type_check in src/main/semi-matched/ */
/* mouse_pad_set in src/main/semi-matched/ */
/* goto_step — aliased to goto_step in lowercase_aliases.c */
/* goto_sub_step — aliased to goto_sub_step in lowercase_aliases.c */
/* goto_sub_sub_step — aliased to goto_sub_sub_step in lowercase_aliases.c */
/* pad_get in src/main/semi-matched/ */
/* konami_command_check in src/main/semi-matched/ */
/* pause_func in pc-port */
/* draw2d3d in pc-port */
/* back_clear_switch in pc-port */
/* palette_upload_enqueue in pc-port */
/* image_upload_queue_add_flags in pc-port */
/* palette_load_vram forwarded in src/main/pc-port/thunk_entry_aliases.c */
/* palette_upload_enqueue — aliased to palette_upload_enqueue in lowercase_aliases.c */
/* device_port_error_set in pc-port */
/* device_init_flag_get in pc-port */
/* device_state_init in pc-port */
/* func_80044AB0 in pc-port */
/* device_load_start in src/main/semi-matched/ */
/* device_load_cmd_send forwarded in src/main/pc-port/thunk_entry_aliases.c */
/* device_load_start in src/main/semi-matched/ */
/* now_trans_pointer in pc-port */
/* change_trans_pointer in pc-port */
/* CDQ_init in pc-port */
/* func_80045534: the real entry of cdq_command_enqueue (its header window
 * starts at 0x80045534; config/symbol_names.json records the prologue at
 * 0x8004555C, so gen_missing_funcs.py cannot see that the address is inside
 * the function and would stub it).  cd_command_issue calls this spelling.
 * Until 2026-10-15 a pc-port "loose translation" of the enqueue shadowed it. */
__attribute__((naked)) void func_80045534(void) { __asm__("jmp cdq_command_enqueue"); }
/* func_800457FC in src/main/semi-matched/ */
/* get_CD_l_com in pc-port */
/* cdq_result_word_get in pc-port */
/* get_XAsector in pc-port */
/* cd_command_tag_pack in pc-port */
/* block_checksum_check in pc-port */
/* keta_n in pc-port */
/* nibble_decimal_convert in src/main/semi-matched/ */
/* don_init in pc-port */
/* tpage_buf_clear — aliased to tpage_buf_clear in lowercase_aliases.c */
/* _sys_default_tpage_set in src/main/semi-matched/ */
/* load_tpage_buf_lock forwarded in src/main/pc-port/thunk_entry_aliases.c */
/* search_tpage in src/main/semi-matched/ */
/* search_tpage8 in src/main/semi-matched/ */
/* class_switch in pc-port */
/* class_clear_n in pc-port */
/* class_clear_n — aliased to class_clear_n in lowercase_aliases.c */
/* get_brightness in pc-port */
/* clear_brightness in pc-port */
/* set_brightness in pc-port */
/* prim_buf_init in pc-port */
/* prim_alloc in pc-port */
/* safe_env in pc-port */
/* func_80049B00..DF8: POLY_G4 color setters. On PS1 these 7 case
 * handlers share _sprite_set_box_shade's stack frame (jr-dispatched) to read
 * color and geometry args. In C this calling convention can't be
 * replicated, so pc-port/_sprite_set_box_shade.c inlines all 7 cases' logic.
 * These stubs remain for ABI compat but are never called via the
 * pc-port dispatch path. */
/* func_80049B00 in src/main/semi-matched/ */
/* func_80049B40 in src/main/semi-matched/ */
/* func_80049B80 in src/main/semi-matched/ */
/* func_80049BC0 in src/main/semi-matched/ */
/* func_80049C0C in src/main/semi-matched/ */
/* func_80049C50 in src/main/semi-matched/ */
/* func_80049DF8 in src/main/semi-matched/ */
/* pn_switch in pc-port */
/* clut_gray_set in src/main/semi-matched/ */
/* move_255_line aliased in aliased_funcs.c */
/* srn_set → thunk_entry_aliases.c */
/* text_sys_init in pc-port */
/* k_disp_start in pc-port */
/* k_disp_switch in pc-port */
/* get_k_work in pc-port */
/* get_ksys in pc-port */
/* k_reset in pc-port */
/* k_sub_reset_point_set in pc-port */
/* k_sub_reset in pc-port */
/* k_sub_disp_start in pc-port */
/* k_disp_goto_line_end in src/main/semi-matched/ */
/* k_speed_set in pc-port */
/* get_k_speed in pc-port */
/* k_disp_inc in pc-port */
/* check_end_k in pc-port */
/* check_set_k in pc-port */
/* get_now_k in src/main/semi-matched/ */
/* vload_font in src/main/semi-matched/ */
/* screen_offset_adjust aliased in aliased_funcs.c */
/* menu_set in pc-port */
/* menu_check_1 in src/main/semi-matched/ */
/* menu_check_2 in src/main/semi-matched/ */
/* text_sys_init — aliased to text_sys_init in lowercase_aliases.c */
/* set_kanji_string in pc-port */
/* k_disp_start — aliased to k_disp_start in lowercase_aliases.c */
/* k_reset — aliased to k_reset in lowercase_aliases.c */
/* k_sub_reset_point_set — aliased to k_sub_reset_point_set in lowercase_aliases.c */
/* k_sub_reset — aliased to k_sub_reset in lowercase_aliases.c */
/* k_sub_disp_start — aliased to k_sub_disp_start in lowercase_aliases.c */
/* k_disp_inc — aliased to k_disp_inc in lowercase_aliases.c */
/* check_end_k — aliased to check_end_k in lowercase_aliases.c */
/* menu_set — aliased to menu_set in lowercase_aliases.c */
/* menu_check — aliased to menu_check in lowercase_aliases.c */
/* menu_cursor_move in pc-port */
/* menu_sel_find in src/main/semi-matched/ */
/* menu_bar_color in pc-port */
/* menu_bar_show in pc-port */
/* menu_bar_show — aliased to menu_bar_show in lowercase_aliases.c */
/* gnsx — aliased to gnsx in lowercase_aliases.c */
/* sndisp — aliased to sndisp in lowercase_aliases.c */
/* get_p_name aliased in aliased_funcs.c */
/* get_g_zyotai_s in pc-port */
/* get_g_zyotai_h in pc-port */
/* xa_wait_m aliased in aliased_funcs.c */
/* func_80052600 in src/main/semi-matched/ */
/* get_h_tokimeki in pc-port */
/* get_h_yuukou in pc-port */
/* birth_day_check_days in pc-port */
/* get_grp_point in src/main/semi-matched/ */
/* grp_table_calc in src/main/semi-matched/ */
/* basyo_range_check in pc-port */
/* basyo_girl_get in pc-port */
/* func_80053B94 in src/main/semi-matched/ */
/* Sw_Test in src/main/semi-matched/ */
/* card_step_set in pc-port */
/* card_step_set — aliased to card_step_set in lowercase_aliases.c */
/* card_step_next in pc-port */
/* card_step_init in pc-port */
/* card_step_next — aliased to card_step_next in lowercase_aliases.c */
/* card_state_clear in pc-port */
/* card_check_main in src/main/semi-matched/ */
/* card_file_scan_main in src/main/semi-matched/ */
/* card_file_read_main in pc-port */
/* card_file_write_main in pc-port */
/* card_file_read_main2 in pc-port */
/* card_slot_write_main in src/main/semi-matched/ */
/* card_file_delete aliased in aliased_funcs.c */
/* card_file_read aliased in aliased_funcs.c */
/* func_80054CA8 in src/main/semi-matched/ */
/* card_file_check in src/main/semi-matched/ */
/* save_game_set in src/main/semi-matched/ */
/* save_check in pc-port */
/* save_game_restore aliased in aliased_funcs.c */
/* save_name_set aliased in aliased_funcs.c */
/* save_name_set in src/main/semi-matched/ */
/* set_movie_offset in pc-port */
/* strNext aliased in aliased_funcs.c */
/* movie_main in src/main/semi-matched/ */
/* song_base_set aliased in aliased_funcs.c */
/* set_dec_bri in pc-port */
/* set_dec_bri — aliased to set_dec_bri in lowercase_aliases.c */
/* get_last_gamen_mode in pc-port */
/* dec_bg_reset in pc-port */
/* indexed_byte_toggle in pc-port */
/* indexed_byte_lsb_set in pc-port */
/* dec_bg_init in pc-port */
/* vram_full_move in src/main/semi-matched/ */
/* vram_full_move in src/main/semi-matched/ */
/* goto_step_bg_out aliased in aliased_funcs.c */
/* bg_change provided by src/main/matched/bg_change.c */
/* all_object_disp_off in pc-port */
/* InitMouse in pc-port */
/* RangeMouse in src/main/semi-matched/ */
/* SenseMouse in pc-port */
/* SetMouse in pc-port */
/* mouse_pos_clamp in src/main/semi-matched/ */
/* boot_phase_switch in src/main/semi-matched/ */
/* boot_data_load in src/main/semi-matched/ */
/* step34_main in src/main/semi-matched/ */
/* step34_mainff in src/main/semi-matched/ */
/* step34_main0 in src/main/semi-matched/ */
/* step34_init0 forwarded in src/main/pc-port/thunk_entry_aliases.c */
/* step34_init1 forwarded in src/main/pc-port/thunk_entry_aliases.c */
/* step34_ffinit in src/main/semi-matched/ */
/* step34_init2 aliased in aliased_funcs.c */
/* cursor_image_load in pc-port */
/* sprite_state_set in src/main/semi-matched/ */
/* step34_main1 in pc-port */
/* hotspot_check in pc-port */
/* hotspot_step_check in pc-port */
/* hotspot_table_check in src/main/semi-matched/ */
/* hotspot_table_check2 in src/main/semi-matched/ */
/* step34_disp forwarded in src/main/pc-port/thunk_entry_aliases.c */
/* icon_select_check in src/main/semi-matched/ */
/* cal_menu in pc-port */
/* cal_menu_exit in pc-port */
/* menu_step_main in src/main/semi-matched/ */
/* menu_step_maine0 in pc-port */
/* menu_step_mainff in src/main/semi-matched/ */
/* menu_step_mainf0 in src/main/semi-matched/ */
/* save_format_main in src/main/semi-matched/ */
/* save_file_select_main aliased in aliased_funcs.c */
/* save_file_write_main in src/main/semi-matched/ */
/* menu_step_main6 aliased in aliased_funcs.c */
/* save_msg_show in pc-port */
/* continue_game_init in src/main/semi-matched/ */
/* system_config_main in src/main/semi-matched/ */
/* func_8005DC10 in pc-port */
/* get_mascot_count in src/main/semi-matched/ */
/* system_config_speed_init aliased in aliased_funcs.c */
/* system_config_moji_speed aliased in aliased_funcs.c */
/* system_config_mouse_botton_mode aliased in aliased_funcs.c */
/* func_8005F170 in pc-port */
/* config_click_speed_exit in pc-port */
/* config_menu_mode aliased in aliased_funcs.c */
/* cursor_speed_init aliased in aliased_funcs.c */
/* cursor_speed aliased in aliased_funcs.c */
/* cursor_speed_exit in pc-port */
/* voice_mode_init aliased in aliased_funcs.c */
/* get_nenga_girl — aliased to get_nenga_girl in lowercase_aliases.c */
/* pre_syogatu_init1 aliased in aliased_funcs.c */
/* pre_syogatu in pc-port */
/* pre_syogatu_main aliased in aliased_funcs.c */
/* title_init_Light in src/main/semi-matched/ */
/* title_init_Coordinate aliased in aliased_funcs.c */
/* title_move_show in src/main/semi-matched/ */
/* title_show aliased in aliased_funcs.c */
/* cursor_elem in pc-port */
/* movie_play_main aliased in aliased_funcs.c */
/* bustup_timer_clamp in pc-port */
/* bustup_face_set in src/main/semi-matched/ */
/* bustup_kuchi_ani in src/main/semi-matched/ */
/* bustup_speech in pc-port */
/* parameter_change in src/main/matched/ */
/* parameter_disp_switch in pc-port */
/* bustup_speech — aliased to bustup_speech in lowercase_aliases.c */
/* bustup_wink — aliased to bustup_wink in lowercase_aliases.c */
/* parameter_disp_switch — aliased to parameter_disp_switch in lowercase_aliases.c */
/* parameter_show_init — aliased to parameter_show_init in lowercase_aliases.c */
/* parameter_show in pc-port */
/* hizuke_disp_switch in pc-port */
/* hizuke_init in pc-port */
/* hizuke_show in pc-port */
/* message_disp_switch in pc-port */
/* hizuke_disp_switch — aliased to hizuke_disp_switch in lowercase_aliases.c */
/* hizuke_init — aliased to hizuke_init in lowercase_aliases.c */
/* message_disp_switch — aliased to message_disp_switch in lowercase_aliases.c */
/* icon_disp_switch in pc-port */
/* icon_can_use_set in pc-port */
/* icon_can_use_set — aliased to icon_can_use_set in lowercase_aliases.c */
/* get_icon_can_use in pc-port */
/* icon_init in pc-port */
/* icon_init — aliased to icon_init in lowercase_aliases.c */
/* func_80065478 in src/main/semi-matched/ */
/* icon_mem_card_switch in pc-port */
/* mes_window_switch in pc-port */
/* basyo_init — aliased to basyo_init in lowercase_aliases.c */
/* get_season in pc-port */
/* get_nf in pc-port */
/* update_anim_state in pc-port */
/* get_season — compatibility alias to get_season in lowercase_aliases.c */
/* back_scroll_init in src/main/semi-matched/ */
/* season_music_read aliased in aliased_funcs.c */
/* season_bgm_set in pc-port */
/* mascot_disp_switch in pc-port */
/* mascot_disp_switch — aliased to mascot_disp_switch in lowercase_aliases.c */
/* mascot_move in pc-port */
/* bustup_flag_on in pc-port */
/* cal_class_init aliased in aliased_funcs.c */
/* kuchi_level_set in src/main/semi-matched/ */
/* bustup2_flag_on — aliased to bustup2_flag_on in lowercase_aliases.c */
/* bustup_flag_on — aliased to bustup_flag_on in lowercase_aliases.c */
/* schedule_mark_on aliased in aliased_funcs.c */
/* biorhythm_curve_disp aliased in aliased_funcs.c */
/* cal_base_show in pc-port */
/* magazine_base_show in pc-port */
/* cal_sprite_init in src/main/semi-matched/ */
/* data_save_load_class_init aliased in aliased_funcs.c */
/* album_info_disp in pc-port */
/* magazine_flag_set in src/main/semi-matched/ */
/* config_screen_init aliased in aliased_funcs.c */
/* reserved_date_check in pc-port */
/* get_chr_slot_cnt aliased in aliased_funcs.c */
/* join_club_class_init aliased in aliased_funcs.c */
/* step_directional_scroll_once in pc-port */
/* set_flag_b3_high_bit in pc-port */
/* biorhythm_sf_set in pc-port */
/* biorhythm_sf2 in pc-port */
/* get_pass_days in src/main/semi-matched/ */
/* yazirushi_disp_switch in pc-port */
/* mes_arrow_anim in pc-port */
/* rec_table_trim in src/main/semi-matched/ */
/* get_pad_str in pc-port */
/* get_pad_str2 in pc-port */
/* last_date_spot_timer_dec in src/main/semi-matched/ */
/* schedule_buf_clear in src/main/semi-matched/ */
/* season_mess_clear in src/main/semi-matched/ */
/* season_flag_set in pc-port */
/* season_last_date_spot_clear in src/main/semi-matched/ */
/* basyo_flag_set in src/main/semi-matched/ */
/* bukatu_reset in src/main/semi-matched/ */
/* schedule_birthday_add in pc-port */
/* schedule_make in src/main/semi-matched/ */
/* touzyou_check aliased in aliased_funcs.c */
/* touzyou_basyo_check aliased in aliased_funcs.c */
/* get_param_top_girl in pc-port */
/* girls_event_pick aliased in aliased_funcs.c */
/* step_directional_scroll_once — aliased to step_directional_scroll_once in lowercase_aliases.c */
/* cursor_init — aliased to cursor_init in lowercase_aliases.c */
/* mes_arrow_anim in src/main/semi-matched/ */
/* last_date_spot_timer_dec in src/main/semi-matched/ */
/* restore_bgm — aliased to restore_bgm in lowercase_aliases.c */
/* card_wait_check — aliased to card_wait_check in lowercase_aliases.c */
/* icon_schedule_set — aliased to icon_schedule_set in lowercase_aliases.c */
/* geko_judge aliased in aliased_funcs.c */
/* date_sasoi_judge aliased in aliased_funcs.c */
/* schedule_girls_gakko aliased in aliased_funcs.c */
/* girl_counter_periodic_decrement in src/main/semi-matched/ */
/* syoushin_up aliased in aliased_funcs.c */
/* schedule_main in pc-port */
/* girls_param_clamp in src/main/semi-matched/ */
/* girl2_top_check aliased in aliased_funcs.c */
/* chr_flag_clear aliased in aliased_funcs.c */
/* device_load_wait in src/main/semi-matched/ */
/* return_schedule — aliased to return_schedule in lowercase_aliases.c */
/* byouki_kega in pc-port */
/* byouki_noiroze in pc-port */
/* byouki_sick in pc-port */
/* byouki_kega_kaifuku in pc-port */
/* byouki_noiroze_kaifuku in pc-port */
/* byouki_main7 in pc-port */
/* week_day in pc-port */
/* week_day_init in pc-port */
/* week_day_main in pc-port */
/* week_day_bg_load → thunk_entry_aliases.c */
/* week_day_bustup_set → thunk_entry_aliases.c */
/* week_day_load in pc-port */
/* week_day_bustup_init in pc-port */
/* week_day_main0 → thunk_entry_aliases.c */
/* vacation_day_init → thunk_entry_aliases.c */
/* vacation_day_screen_init in src/main/semi-matched/ */
/* vacation_day_bustup_read in src/main/semi-matched/ */
/* holiday in pc-port */
/* holiday_screen_init in src/main/semi-matched/ */
/* tel_code_check in src/main/semi-matched/ */
/* get_holiday_bg_sector in pc-port */
/* func_800760BC aliased in aliased_funcs.c */
/* machiawase_init in src/main/semi-matched/ */
/* machiawase_select in src/main/semi-matched/ */
/* machiawase_show in src/main/semi-matched/ */
/* machiawase_main in src/main/semi-matched/ */
/* holiday_girl_pick aliased in aliased_funcs.c */
/* basyo_bg_load in src/main/semi-matched/ */
/* holiday_girl_init aliased in aliased_funcs.c */
/* holiday_girl_exit in src/main/semi-matched/ */
/* magazine_step in src/main/semi-matched/ */
/* holiday_tel_exit in src/main/semi-matched/ */
/* overlay_work_clear in pc-port */
/* overlay_load_main in pc-port */
/* overlay_load_disp in src/main/semi-matched/ */
/* overlay_load_init in src/main/semi-matched/ */
/* ending_load_gate aliased in aliased_funcs.c */
/* scene_entry_set in src/main/semi-matched/ */
/* tt_step_check in pc-port */
/* ending_step_check in pc-port */
/* scene_date_check in pc-port */
/* ending_main in pc-port */
/* ending_card_step in src/main/semi-matched/ */
/* SD_CmdPut in pc-port */
/* SD_VabLoadNext in src/main/semi-matched/ */
/* SD_VabLoad in src/main/semi-matched/ */
/* SD_FlagsPoll in src/main/semi-matched/ */
/* SD_CmdDispatch in src/main/semi-matched/ */
/* SD_VabLoadCmd aliased in aliased_funcs.c */
/* func_8007A3E4 → thunk_entry_aliases.c */
/* SD_ModeCmd in src/main/semi-matched/ */
/* func_8007A64C aliased in aliased_funcs.c */
/* serial_vol_base_set in src/main/semi-matched/ */
/* SD_XACmd aliased in aliased_funcs.c */
/* SD_SerialClear in pc-port */
/* serial_cmd in pc-port */
/* serial_fade_set in src/main/semi-matched/ */
/* seq_vol_fade in src/main/semi-matched/ */
/* seq_restart in src/main/semi-matched/ */
/* seq_cmd in src/main/semi-matched/ */
/* func_8007B070 in pc-port */
/* seq_vol_fade_set in src/main/semi-matched/ */
/* cmd_que_put in src/main/semi-matched/ */
/* cmd_que_get in src/main/semi-matched/ */
/* SD_BufferTableInit in src/main/semi-matched/ */
/* SD_VABAddrSet aliased in aliased_funcs.c */
/* SD_StateInit in src/main/semi-matched/ */
/* SD_SEQTableSet in src/main/semi-matched/ */
/* SD_SEQTableSet in src/main/semi-matched/ */
/* SD_StereoModeSet aliased in aliased_funcs.c */
/* SD_Exit in src/main/semi-matched/ */
/* SD_VabLoadReq in pc-port */
/* SD_FlagToggle in pc-port */
/* SD_CDLevelRead in src/main/semi-matched/ */
/* func_8007C36C in src/main/semi-matched/ */
/* SD_CDLevelPeak aliased in aliased_funcs.c */
/* SD_CDLevelAvg in src/main/semi-matched/ */
/* SD_CDLevelGet in pc-port */
/* bust_up_ptr_set in src/main/semi-matched/ */
/* bustup_addr_set in src/main/semi-matched/ */
/* bustup_addr_set2 in src/main/semi-matched/ */
/* bustup_addr_set3 in src/main/semi-matched/ */
/* _wipe_main in src/main/semi-matched/ */
/* fade_up_init in pc-port */
/* fade_up in src/main/semi-matched/ */
/* fade_down_init in pc-port */
/* fade_down in src/main/semi-matched/ */
/* fade_in_step in pc-port */
/* fade_out_step in pc-port */
/* sprite_fade in src/main/semi-matched/ */
/* seq_restart in src/main/semi-matched/ */
/* seq_start_req — aliased to seq_start_req in lowercase_aliases.c */
/* seq_ctrl — aliased to seq_ctrl in lowercase_aliases.c */
/* seq_vol_fade_set in src/main/semi-matched/ */
/* cmd_que_clear — aliased to cmd_que_clear in lowercase_aliases.c */
/* serifu_cmd_exec — aliased to serifu_cmd_exec in lowercase_aliases.c */
/* serifu_skip in pc-port */
/* serifu_main aliased in aliased_funcs.c */
/* func_800829FC in src/main/semi-matched/ */
/* msg_part_reorder in src/main/semi-matched/ */
/* msg_order_set in src/main/semi-matched/ */
/* menu3_init aliased in aliased_funcs.c */
/* menu3_select aliased in aliased_funcs.c */
/* menu2_init aliased in aliased_funcs.c */
/* girl_season_load in src/main/semi-matched/ */
/* k_disp_wait in pc-port */
/* girl_menu_set aliased in aliased_funcs.c */
/* girl_menu_select aliased in aliased_funcs.c */
/* sprite_rgb_set in pc-port */
/* date_sprite_switch in src/main/semi-matched/ */
/* bustup_face_set2 in src/main/semi-matched/ */
/* Vblnk_Timer_Init in pc-port */
/* Vblnk_Timer in pc-port */
/* DecDCTvlc in pc-port */
/* MDEC_reset aliased in aliased_funcs.c */
/* MDEC_in_sync forwarded in src/main/pc-port/thunk_entry_aliases.c */
/* MDEC_report aliased in aliased_funcs.c */
/* MDEC_vlc in src/main/semi-matched/ */
/* MDEC_vlc2 in src/main/semi-matched/ */
/* ivlc_dct_dc_size in src/main/semi-matched/ */
/* CdInit aliased in aliased_funcs.c */
/* CdReady — aliased to CdReady in lowercase_aliases.c */
/* CdSyncCallback in pc-port */
/* CdReadyCallback in pc-port */
/* CdReadCallback in pc-port */
/* CdControlF in src/main/semi-matched/ */
/* CdControlB in pc-port */
/* CdDataCallback in pc-port */
/* CdReadSync in pc-port */
/* CdRead2 aliased in aliased_funcs.c */
/* CdIntToPos in pc-port */
/* CdPosToInt in src/main/semi-matched/ */
/* StCdInterrupt in src/main/semi-matched/ */
/* dma_execute in src/main/semi-matched/ */
/* StFreeRing in src/main/semi-matched/ */
/* StGetNext in src/main/semi-matched/ */
/* mem2mem — aliased to mem2mem in lowercase_aliases.c */
/* dma_execute in src/main/semi-matched/ */
/* CD_searchdir aliased in aliased_funcs.c */
/* CD_cachefile aliased in aliased_funcs.c */
/* getintr in src/main/semi-matched/ */
/* CD_ready in src/main/semi-matched/ */
/* CD_vol aliased in aliased_funcs.c */
/* CD_init in pc-port */
/* cd_init_retry in src/main/semi-matched/ */
/* cd_read_retry aliased in aliased_funcs.c */
/* CD_datasync aliased in aliased_funcs.c */
/* CD_getsector aliased in aliased_funcs.c */
/* data_ready_callback in src/main/semi-matched/ */
/* StClearRing in src/main/semi-matched/ */
/* StSetMask in pc-port */
/* SsSetMVol in pc-port */
/* SsSetTableSize in src/main/semi-matched/ */
/* SsSetTickMode in src/main/semi-matched/ */
/* _wait in pc-port */
/* SsSetSerialAttr in src/main/semi-matched/ */
/* SsSetSerialVol in src/main/semi-matched/ */
/* Snd_SetVol in src/main/semi-matched/ */
/* SpuVmAlloc in src/main/semi-matched/ */
/* SpuVmKeyOnNow in src/main/semi-matched/ */
/* SpuVmKeyOffNow in pc-port */
/* SpuVmDoAllocate in src/main/semi-matched/ */
/* vmNoiseOn in src/main/semi-matched/ */
/* vmNoiseOn2 in src/main/semi-matched/ */
/* vmNoiseOff in pc-port */
/* note2pitch2 in src/main/semi-matched/ */
/* varargs_spill1 in pc-port */
/* varargs_spill3 in pc-port */
/* SpuVmFlush in pc-port */
/* SpuVmKeyOn in src/main/semi-matched/ */
/* SpuVmSetSeqVol in pc-port */
/* SpuVmGetSeqVol in pc-port */
/* SsUtAllKeyOff in src/main/semi-matched/ */
/* SsUtSetReverbType in src/main/semi-matched/ */
/* SsUtSetReverbDepth in pc-port */
/* SsUtSetReverbFeedback in pc-port */
/* SsUtSetReverbDelay in pc-port */
/* SsSeqOpen forwarded in src/main/pc-port/thunk_entry_aliases.c */
/* SsUtGetVabHdr in src/main/semi-matched/ */
/* Snd_SetPlayMode in src/main/semi-matched/ */
/* SpuVmSetSeqVol — aliased to SpuVmSetSeqVol in lowercase_aliases.c */
/* SpuVmGetSeqVol — aliased to SpuVmGetSeqVol in lowercase_aliases.c */
/* SsSeqStop in src/main/semi-matched/ */
/* Snd_stop in pc-port */
/* SsSeqPause in src/main/semi-matched/ */
/* SsSeqReplay in src/main/semi-matched/ */
/* SsVoKeyOff in pc-port */
/* SsVoKeyOn in src/main/semi-matched/ */
/* SsUtFlush in pc-port */
/* SsSetReservedVoice in pc-port */
/* SsSetAutoKeyOffMode in pc-port */
/* SsSetMono in pc-port */
/* SsSetStereo in pc-port */
/* SsVabClose in src/main/semi-matched/ */
/* SsSeqCalledTbyT in src/main/semi-matched/ */
/* SpuVmVSetUp in pc-port */
/* InitSoundSeq in src/main/semi-matched/ */
/* InitSoundSep in src/main/semi-matched/ */
/* Snd_pause in src/main/semi-matched/ */
/* Snd_replay in src/main/semi-matched/ */
/* Snd_decrescendo in src/main/semi-matched/ */
/* func_800922EC in pc-port */
/* func_800926F0 in src/main/semi-matched/ */
/* Snd_tempo in src/main/semi-matched/ */
/* GetSeqData in src/main/semi-matched/ */
/* NoteOn in src/main/semi-matched/ */
/* SetProgramChange in src/main/semi-matched/ */
/* SetControlChange in src/main/semi-matched/ */
/* NoteOn in src/main/semi-matched/ */
/* SetProgramChange in src/main/semi-matched/ */
/* SetControlChange in src/main/semi-matched/ */
/* ContModulation aliased in aliased_funcs.c */
/* ContPortamento aliased in aliased_funcs.c */
/* ContResetAll aliased in aliased_funcs.c */
/* ContNrpn1 in src/main/semi-matched/ */
/* ContNrpn2 in src/main/semi-matched/ */
/* ContRpn1 in src/main/semi-matched/ */
/* ContRpn2 in src/main/semi-matched/ */
/* ContDataEntry in src/main/semi-matched/ */
/* func_80093F8C in pc-port */
/* SetPitchBend in src/main/semi-matched/ */
/* func_80094638 in pc-port */
/* GetMetaEvent in src/main/semi-matched/ */
/* ReadDeltaValue in pc-port */
/* SsUtGetProgAtr in src/main/semi-matched/ */
/* SsUtSetVagAtr in pc-port */
/* SsUtGetVagAtr in src/main/semi-matched/ */
/* ReadDeltaValue — aliased to ReadDeltaValue in lowercase_aliases.c */
/* SpuVmDamperOn in pc-port */
/* SpuVmDamperOff in pc-port */
/* Snd_nextseq in src/main/semi-matched/ */
/* SsUtResolveADSR in pc-port */
/* SsUtBuildADSR in pc-port */
/* _spu_setInTransfer in pc-port */
/* _spu_getInTransfer in pc-port */
/* SpuSetTransferStartAddr in src/main/semi-matched/ */
/* SpuRead in pc-port */
/* _spu_init in src/main/semi-matched/ */
/* _spu_close — alias to _spu_close in aliased_funcs.c */
/* _spu_setDelayW in pc-port */
/* _spu_write — alias to _spu_write in aliased_funcs.c */
/* _spu_writeByIO in src/main/semi-matched/ */
/* _spu_read in pc-port */
/* _spu_w in src/main/semi-matched/ */
/* _spu_setDelayR — aliased to _spu_setDelayR in lowercase_aliases.c */
/* _spu_ioctl in pc-port */
/* _spu_setVoiceAttr in src/main/semi-matched/ */
/* func_80097274 in pc-port */
/* SpuInitMalloc in src/main/semi-matched/ */
/* SpuMalloc in src/main/semi-matched/ */
/* _SpuIsInAllocateArea in src/main/semi-matched/ */
/* _SpuIsInAllocateArea_ in pc-port */
/* SpuFree forwarded in src/main/pc-port/thunk_entry_aliases.c */
/* SpuWrite in pc-port */
/* SpuSetReverb aliased in aliased_funcs.c */
/* _memcpy in pc-port */
/* SpuSetReverbModeParam in pc-port */
/* func_80098970 in src/main/semi-matched/ */
/* gpu_init in src/main/semi-matched/ */
/* GsInitGraph, GsDefDispBuff, GsInit3D, GsInitVcount, GsSetWorkBase
 * run their matched implementations */
/* valiable_init in src/main/semi-matched/ */
/* GsSortClear in src/main/semi-matched/ */
/* GsGetActiveBuff in pc-port */
/* GsSetDrawBuffOffset is implemented in src/main/pc-port/GsSetDrawBuffOffset.c */
/* GsSortSprite in src/main/semi-matched/ */
/* GsSortFastSprite in src/main/semi-matched/ */
/* GsSortBg in src/main/semi-matched/ */
/* GsSetRefView2 in src/main/semi-matched/ */
/* select_max_param in src/main/semi-matched/ */
/* len_param in pc-port */
/* GsGetLws aliased in aliased_funcs.c */
/* GsInitCoordinate2 in src/main/semi-matched/ */
/* func_8009B2B0 in src/main/semi-matched/ */
/* func_8009B538 in pc-port */
/* func_8009BB00 in src/main/semi-matched/ */
/* TmdF3 aliased in aliased_funcs.c */
/* TmdG3 aliased in aliased_funcs.c */
/* TmdF4 aliased in aliased_funcs.c */
/* TmdG4 aliased in aliased_funcs.c */
/* TmdTF3 aliased in aliased_funcs.c */
/* TmdTF4 aliased in aliased_funcs.c */
/* GsSwapDispBuff in src/main/semi-matched/ */
/* GsSetProjection in pc-port */
/* GsSetNearClip in pc-port */
/* GsSetFarClip in pc-port */
/* GsSetFlatLight in src/main/semi-matched/ */
/* GsSetAmbient in pc-port */
/* GsSetLightMode aliased in aliased_funcs.c */
/* GsMapModelingData in src/main/semi-matched/ */
/* GsSetDrawBuffClip in src/main/semi-matched/ */
/* Gslight_normal aliased in aliased_funcs.c */
/* Gslight_normal_fog in src/main/semi-matched/ */
/* Gslight_mate aliased in aliased_funcs.c */
/* gte_flat_mat3 in pc-port */
/* gte_texture_flat_mat3 in pc-port */
/* gte_gouraud_mat3 in pc-port */
/* gte_texture_gouraud_mat3 in pc-port */
/* gte_flat_mat_fog3 in pc-port */
/* gte_texture_flat_mat_fog3 in pc-port */
/* gte_gouraud_mat_fog3 in pc-port */
/* gte_texture_gouraud_mat_fog3 in pc-port */
/* gte_rotate_z_matrix aliased in aliased_funcs.c */
/* gte_scale_matrix in src/main/semi-matched/ */
/* _mk_xpndsp — alias to SsSeqCalledTbyT in aliased_funcs.c */
/* _mk_normsp in src/main/semi-matched/ */
/* func_8009E680 in pc-port */
/* gte_rotate_x_matrix aliased in aliased_funcs.c */
/* set_jtb in pc-port */
/* GetGraphType in pc-port */
/* SetDispMask aliased in aliased_funcs.c */
/* checkRECT in src/main/semi-matched/ */
/* SetDrawMode in pc-port */
/* SetDrawEnv in src/main/semi-matched/ */
/* get_mode in pc-port */
/* get_ofs in pc-port */
/* get_tw in pc-port */
/* _addque2 in src/main/semi-matched/ */
/* _reset in src/main/semi-matched/ */
/* func_800A2974 (memset) in src/main/matched/memset.c */
/* GetClut in pc-port */
/* AddPrim in pc-port */
/* SetShadeTex in pc-port */
/* GPU primitive tag setters (SetPolyG3 ... SetLineG2): matched C in
 * src/main/matched/, folded by src/main/pc-port/prim_arg_fold.c */
/* InitGeom in src/main/semi-matched/ */
/* rsin in pc-port */
/* sin_1 in src/main/semi-matched/ */
/* rcos in pc-port */
/* RotMatrix in src/main/semi-matched/ */
/* MulMatrix in pc-port */
/* MulMatrix2 in src/main/semi-matched/ */
/* ApplyMatrix in src/main/semi-matched/ */
/* ScaleMatrix in src/main/semi-matched/ */
/* SetRotMatrix in src/main/semi-matched/ */
/* SetLightMatrix in src/main/semi-matched/ */
/* SetColorMatrix in src/main/semi-matched/ */
/* SetTransMatrix in src/main/semi-matched/ */
/* SquareRoot0 in src/main/semi-matched/ */
/* SetBackColor in src/main/semi-matched/ */
/* SetFarColor in src/main/semi-matched/ */
/* SetGeomScreen in src/main/semi-matched/ */
/* MulMatrix0 in src/main/semi-matched/ */
/* ApplyMatrixLV in src/main/semi-matched/ */
/* PushMatrix in src/main/semi-matched/ */
/* PopMatrix in src/main/semi-matched/ */
/* GsTMDfastNF3 in src/main/semi-matched/ */
/* GsTMDfastF3NL in src/main/semi-matched/ */
/* GsTMDfastF3L in src/main/semi-matched/ */
/* GsTMDfastF3LFG in src/main/semi-matched/ */
/* GsTMDfastNF4 in src/main/semi-matched/ */
/* GsTMDfastF4NL in src/main/semi-matched/ */
/* GsTMDfastF4L in src/main/semi-matched/ */
/* GsTMDfastF4LFG in src/main/semi-matched/ */
/* GsTMDfastNG3 in src/main/semi-matched/ */
/* GsTMDfastG3NL in src/main/semi-matched/ */
/* GsTMDfastG3L in src/main/semi-matched/ */
/* GsTMDfastG3LFG in src/main/semi-matched/ */
/* GsTMDfastTNF3 in src/main/semi-matched/ */
/* GsTMDfastTF3NL in src/main/semi-matched/ */
/* GsTMDfastTF3L in src/main/semi-matched/ */
/* GsTMDfastTF3LFG in src/main/semi-matched/ */
/* GsTMDfastTNG3 in src/main/semi-matched/ */
/* GsTMDfastTG3NL in src/main/semi-matched/ */
/* GsTMDfastTG3L in src/main/semi-matched/ */
/* GsTMDfastTG3LFG in src/main/semi-matched/ */
/* GsTMDfastNG4 in src/main/semi-matched/ */
/* GsTMDfastG4NL in src/main/semi-matched/ */
/* GsTMDfastG4L in src/main/semi-matched/ */
/* GsTMDfastG4LFG in src/main/semi-matched/ */
/* GsTMDfastTNF4 in src/main/semi-matched/ */
/* GsTMDfastTF4NL in src/main/semi-matched/ */
/* GsTMDfastTF4L in src/main/semi-matched/ */
/* GsTMDfastTF4LFG in src/main/semi-matched/ */
/* GsTMDfastTNG4 in src/main/semi-matched/ */
/* GsTMDfastTG4NL in src/main/semi-matched/ */
/* GsTMDfastTG4L in src/main/semi-matched/ */
/* GsTMDfastTG4LFG in src/main/semi-matched/ */
/* GsTMDdivF3L in src/main/semi-matched/ */
/* GsTMDdivF3LFG in src/main/semi-matched/ */
/* GsTMDdivF3NL in src/main/semi-matched/ */
/* GsTMDdivNF3 in src/main/semi-matched/ */
/* GsTMDdivF4L in src/main/semi-matched/ */
/* GsTMDdivF4LFG in src/main/semi-matched/ */
/* GsTMDdivF4NL in src/main/semi-matched/ */
/* GsTMDdivNF4 in src/main/semi-matched/ */
/* GsTMDdivG3L in src/main/semi-matched/ */
/* GsTMDdivG3LFG in src/main/semi-matched/ */
/* GsTMDdivG3NL in src/main/semi-matched/ */
/* GsTMDdivNG3 in src/main/semi-matched/ */
/* GsTMDdivTF3L in src/main/semi-matched/ */
/* GsTMDdivTF3LFG in src/main/semi-matched/ */
/* GsTMDdivTF3NL in src/main/semi-matched/ */
/* GsTMDdivTNF3 in src/main/semi-matched/ */
/* GsTMDdivTG3L in src/main/semi-matched/ */
/* GsTMDdivTG3LFG in src/main/semi-matched/ */
/* GsTMDdivTG3NL in src/main/semi-matched/ */
/* GsTMDdivTNG3 in src/main/semi-matched/ */
/* GsTMDdivG4L in src/main/semi-matched/ */
/* GsTMDdivG4LFG in src/main/semi-matched/ */
/* GsTMDdivG4NL in src/main/semi-matched/ */
/* GsTMDdivNG4 in src/main/semi-matched/ */
/* GsTMDdivTF4L in src/main/semi-matched/ */
/* GsTMDdivTF4LFG in src/main/semi-matched/ */
/* GsTMDdivTF4NL in src/main/semi-matched/ */
/* GsTMDdivTNF4 in src/main/semi-matched/ */
/* GsTMDdivTG4L in src/main/semi-matched/ */
/* GsTMDdivTG4LFG in src/main/semi-matched/ */
/* GsTMDdivTG4NL in src/main/semi-matched/ */
/* GsTMDdivTNG4 in src/main/semi-matched/ */
/* gte_scope_f3 in src/main/semi-matched/ */
/* gte_scope_tf3 in src/main/semi-matched/ */
/* gte_scope_g3 in src/main/semi-matched/ */
/* gte_scope_tg3 in src/main/semi-matched/ */
/* gte_flat3 in src/main/semi-matched/ */
/* gte_texture_flat3 in src/main/semi-matched/ */
/* gte_gouraud3 in src/main/semi-matched/ */
/* gte_texture_gouraud3 in src/main/semi-matched/ */
/* ResetCallback aliased in aliased_funcs.c */
/* InterruptCallback aliased in aliased_funcs.c */
/* DMACallback aliased in aliased_funcs.c */
/* CheckCallback in pc-port */
/* SetIntrMask in pc-port */
/* trapIntr in src/main/semi-matched/ */
/* memclr in pc-port */
/* VSync aliased in aliased_funcs.c */
/* v_wait aliased in aliased_funcs.c */
/* GetVideoMode in pc-port */
/* startIntrVSync aliased in aliased_funcs.c */
/* startIntrDMA aliased in aliased_funcs.c */
/* dma_intr_table_clear in pc-port */
/* InitCARD → bios_trampolines.c */
/* StartCARD → bios_trampolines.c */
/* _bu_init → bios_trampolines.c */
/* _card_auto → bios_trampolines.c */
/* _card_status → bios_trampolines.c */
/* _card_info → bios_trampolines.c */
/* _card_load → bios_trampolines.c */
/* _card_clear in pc-port */
/* _new_card → bios_trampolines.c */
/* _card_write → bios_trampolines.c */
/* func_800B4820 → bios_trampolines.c */
/* func_800B4830 → bios_trampolines.c */
/* func_800B4840 → bios_trampolines.c */
/* func_800B4850 → bios_trampolines.c */
/* func_800B4860 → bios_trampolines.c */
/* func_800B4870 in pc-port */
/* func_800B4880 → bios_trampolines.c */
/* func_800B4890 → bios_trampolines.c */
/* func_800B48A0 → bios_trampolines.c */
/* func_800B48B0 → bios_trampolines.c */
/* func_800B48C0 → bios_trampolines.c */
/* func_800B48D0 in src/main/semi-matched/ */
/* func_800B5650 → bios_trampolines.c */
/* func_800B5660 → bios_trampolines.c */
/* func_800B5670 → bios_trampolines.c */
/* func_800B5680 → bios_trampolines.c */
/* func_800B5690 → bios_trampolines.c */
/* func_800B56A0 → bios_trampolines.c */
/* SetSp in src/main/semi-matched/ */
/* InitHeap → bios_trampolines.c */
/* GetGp in src/main/semi-matched/ */
/* InitPAD → bios_trampolines.c */
/* StartPAD → bios_trampolines.c */
/* ChangeClearPAD → bios_trampolines.c */
/* EnterCriticalSection in src/main/semi-matched/ */
/* FlushCache → bios_trampolines.c */
/* ExitCriticalSection in src/main/semi-matched/ */
/* OpenEvent → bios_trampolines.c */
/* EnableEvent → bios_trampolines.c */
/* SetRCnt in src/main/semi-matched/ */
/* GetRCnt in src/main/semi-matched/ */
/* StartRCnt in src/main/semi-matched/ */
/* StopRCnt in src/main/semi-matched/ */
/* ResetRCnt in src/main/semi-matched/ */
/* GetSp in src/main/semi-matched/ */
/* TestEvent → bios_trampolines.c */
/* CloseEvent in pc-port */
/* func_800B5920 → bios_trampolines.c */
/* format → bios_trampolines.c */
/* func_800B5940 → bios_trampolines.c */
/* delete → bios_trampolines.c */
/* func_800B5960 → bios_trampolines.c */
/* func_800B5970 → bios_trampolines.c */
/* _96_init → bios_trampolines.c */
/* LoadExec → bios_trampolines.c */
/* _96_remove → bios_trampolines.c */
/* DeliverEvent → bios_trampolines.c */
/* WaitEvent → bios_trampolines.c */
/* DisableEvent → bios_trampolines.c */
/* GPU_cw → bios_trampolines.c */
/* HookEntryInt → bios_trampolines.c */
/* ChangeClearRCnt → bios_trampolines.c */
/* ResetEntryInt → bios_trampolines.c */
/* PAD_init in src/main/semi-matched/ */
/* PAD_dr in src/main/semi-matched/ */
/* StopPAD in src/main/semi-matched/ */
/* func_800BB3C0 in src/main/semi-matched/ */
/* get_cs in src/main/semi-matched/ */
/* get_ce in src/main/semi-matched/ */
/* GetTPage — aliased to GetTPage in lowercase_aliases.c */
/* GetClut — aliased to GetClut in lowercase_aliases.c */
/* AddPrim — aliased to AddPrim in lowercase_aliases.c */
/* SetSemiTrans — aliased to SetSemiTrans in lowercase_aliases.c */
/* SetShadeTex — aliased to SetShadeTex in lowercase_aliases.c */
/* InitGeom in src/main/semi-matched/ */
/* rsin — aliased to rsin in lowercase_aliases.c */
/* func_800b4830 in src/main/semi-matched/ */
/* func_800b4860 in src/main/semi-matched/ */
/* func_800b4880 in src/main/semi-matched/ */
/* func_800b4890 in src/main/semi-matched/ */
/* func_800b48a0 in src/main/semi-matched/ */
/* func_800b48d0 in src/main/semi-matched/ */
/* func_800b5650 in src/main/semi-matched/ */
/* func_800b5920 in src/main/semi-matched/ */
/* func_800b5940 in src/main/semi-matched/ */
/* pad_trig_w (pad_trig), now_icon_b (now_icon), step_counter_u
 * (step_counter) are PS1 DATA, PROVIDE()d in missing_aliases.ld.  As empty
 * functions here title_fade_out read code bytes for step_counter and its
 * 128-frame fade ended after one frame (audit_pc_stubs.py DATA-AS-FUNC). */
/* ContPortaTime aliased in aliased_funcs.c */
/* DrawOTag aliased in aliased_funcs.c */
/* ClearOTagR aliased in aliased_funcs.c */

/* Overlay cross-references and missing functions */

/* Remaining stubs — functions not in matched/semi-matched */
void gte_flat_fog3(void) { }
void gte_texture_flat_fog3(void) { }
void gte_gouraud_fog3(void) { }
void gte_texture_gouraud_fog3(void) { }

/* Referenced by semi-matched C landed 2026-08-23, no body on either side yet.
 * wipe_tile_disp — called by the 0x8007C968 transition step (case 6).
 * gte_texture_gouraud3 — gte_texture_gouraud3, hand-written Sony GTE assembly
 *                 (LIBGTE 2.5 TMD member); terminal asm_only, so it will
 *                 never gain a C body. pc/hal/gte.c should provide it. */
void wipe_tile_disp(int arg0) { (void)arg0; }
void gte_texture_gouraud3(void) { }


/* Unshadowing 2026-10-18 (owner directive: delete the shadows, use the PS1
 * code, fix the fallout after).  The GTE packet builders below are terminal
 * asm_only (Sony LIBGTE TMD members, config/c_unmatchable.txt), so the port
 * has to supply them.
 *
 * 2026-10-29 (measured, scene 18 / title): every one of these used to be a
 * bare `return 0`, and GsSortObject4 writes `GsOUT_PACKET_P = TmdF3(...)`.
 * The first 3D object of the first frame therefore collapsed the packet
 * pointer to NULL, which silently discarded every primitive after it in the
 * frame (DrawOTag: 257 nodes, 1 primitive, blank title; scene 64 never shows
 * it because it draws no 3D objects).  They now pass the packet cursor
 * through.
 *
 * The eight unlit builders (NF3/NF4/NG3/NG4/TNF3/TNF4/TNG3/TNG4) are real
 * since 2026-10-30: src/main/pc-port/tmd_gte_builders.c.  The lit families
 * below still emit NO geometry: their colour maths needs a measured
 * lighting contract.  Until that lands, lit models are invisible but the
 * rest of the frame survives.
 *
 * Argument position of the packet cursor differs by family: the TmdF3/F4/G3/
 * G4 dispatchers forward it as the FOURTH argument (prim, vtx, normal table,
 * packet), while GsSortObject4's pattern-2 arms pass it THIRD (prim, vtx,
 * packet, count).  Getting this wrong reintroduces the NULL. */
#define TMD_FAST_PT4(name)                                                  \
    void *name(void *prim, void *vtx, void *normal, void *out, s32 count,     \
               s32 shift, s32 ot)                                          \
    {                                                                       \
        (void)prim; (void)vtx; (void)normal; (void)count; (void)shift;       \
        (void)ot;                                                           \
        return out;                                                         \
    }

TMD_FAST_PT4(GsTMDfastF3LFG)
TMD_FAST_PT4(GsTMDfastF3NL)
TMD_FAST_PT4(GsTMDfastF4L)
TMD_FAST_PT4(GsTMDfastF4LFG)
TMD_FAST_PT4(GsTMDfastF4NL)
TMD_FAST_PT4(GsTMDfastG3LFG)
TMD_FAST_PT4(GsTMDfastG3NL)
TMD_FAST_PT4(GsTMDfastG4LFG)
TMD_FAST_PT4(GsTMDfastTF3LFG)
TMD_FAST_PT4(GsTMDfastTF3NL)
TMD_FAST_PT4(GsTMDfastTF4LFG)
TMD_FAST_PT4(GsTMDfastTG4L)
TMD_FAST_PT4(GsTMDfastTG4LFG)
TMD_FAST_PT4(GsTMDfastTG4NL)

#undef TMD_FAST_PT4

/* TODO(psyz): the matched twin is an analysis shell (the cop2/GTE class) */
s32 RCpolyF4A(void) { return 0; }
/* TODO(psyz): the matched twin is an analysis shell (the cop2/GTE class) */
s32 RCpolyFT3A(void) { return 0; }
/* TODO(psyz): the matched twin is an analysis shell (the cop2/GTE class) */
s32 RCpolyFT4A(void) { return 0; }
/* TODO(psyz): the matched twin is an analysis shell (the cop2/GTE class) */
s32 RCpolyG3A(void) { return 0; }
/* TODO(psyz): the matched twin is an analysis shell (the cop2/GTE class) */
s32 RCpolyG4A(void) { return 0; }
/* TODO(psyz): the matched twin is an analysis shell (the cop2/GTE class) */
s32 RCpolyGT3A(void) { return 0; }
/* TODO(psyz): the matched twin is an analysis shell (the cop2/GTE class) */
s32 RCpolyGT4A(void) { return 0; }
/* TODO(psyz): the matched twin is an analysis shell (the cop2/GTE class) */
s32 func_80049370_reset(void) { return 0; }
/* TODO(psyz): the matched twin is an analysis shell (the cop2/GTE class) */
s32 vector_normalize(void) { return 0; }
/* (week_day_bg_load, MDEC_out_sync, basyo_show, bustup_modes20_step,
 * cal_bio_disp: the matched C names the body by another symbol; the
 * entries are forwarded in src/main/pc-port/thunk_entry_aliases.c.
 * pc/tools/audit_pc_stubs.py finds this class.) */
