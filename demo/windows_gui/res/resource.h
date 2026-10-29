// tg_capture_gui 资源 ID（对话框模板在 tg_capture_gui.rc，可直接用 VS2022 资源视图编辑）
#ifndef TGUI_RESOURCE_H
#define TGUI_RESOURCE_H

#ifndef IDC_STATIC
#define IDC_STATIC              (-1)
#endif

#define IDI_APP                 101
#define IDD_MAIN                102

// ① 视频源
#define IDC_SRC_MONITOR         1001
#define IDC_SRC_WINDOW          1002
#define IDC_TARGET_LABEL        1003
#define IDC_MONITOR_LIST        1004
#define IDC_WINDOW_LIST         1005
#define IDC_WINDOW_REFRESH      1006
#define IDC_CAP_MODE            1007
#define IDC_PERF_MODE           1008
#define IDC_SRC_FILE            1009
#define IDC_VFILE_PATH          1010
#define IDC_VFILE_BROWSE        1011
#define IDC_VFILE_LOOP          1012
#define IDC_VFILE_AUDIO         1013
#define IDC_CAP_MODE_LABEL      1014

// ② 音频源
#define IDC_MIC_ENABLE          1020
#define IDC_MIC_DEVICE          1021
#define IDC_MIC_MUTE            1022
#define IDC_SPK_ENABLE          1023
#define IDC_SPK_DEVICE          1024
#define IDC_SPK_MUTE            1025
#define IDC_AUDIO_REFRESH       1026
#define IDC_MIC_VOLUME          1027
#define IDC_MIC_LEVEL           1028
#define IDC_SPK_VOLUME          1029
#define IDC_SPK_LEVEL           1030
#define IDC_FILE_ENABLE         1031
#define IDC_FILE_PATH           1032
#define IDC_FILE_BROWSE         1033
#define IDC_FILE_LOOP           1034
#define IDC_FILE_VOLUME         1035
#define IDC_FILE_LEVEL          1036
#define IDC_MASTER_LEVEL        1037

// ③ 编码与输出
#define IDC_OUTPUT_URL          1040
#define IDC_OUTPUT_BROWSE       1041
#define IDC_MUXER               1042
#define IDC_RESOLUTION          1043
#define IDC_FPS                 1044
#define IDC_VCODEC              1045
#define IDC_VBITRATE            1046
#define IDC_GOP                 1047
#define IDC_ACODEC              1048
#define IDC_ABITRATE            1049
#define IDC_ROTATION            1050
#define IDC_FLIP                1051
#define IDC_A_TIMEOUT           1052
#define IDC_V_TIMEOUT           1053
#define IDC_RES_APPLY           1054

// ④ 画面控制
#define IDC_PAUSE_PIC           1060
#define IDC_PAUSE_PIC_BROWSE    1061
#define IDC_PAUSE_PIC_APPLY     1062
#define IDC_LOGO_PATH           1063
#define IDC_LOGO_BROWSE         1064
#define IDC_LOGO_INDEX          1065
#define IDC_LOGO_POS            1066
#define IDC_LOGO_X              1067
#define IDC_LOGO_Y              1068
#define IDC_LOGO_ADD            1069
#define IDC_LOGO_UPDATE         1070
#define IDC_LOGO_REMOVE         1071

// 状态 / 日志 / 底部按钮
#define IDC_STATUS_GROUP        1080
#define IDC_STATUS              1081
#define IDC_STATS               1082
#define IDC_LOG_GROUP           1083
#define IDC_LOG                 1084
#define IDC_LOG_LEVEL           1085
#define IDC_LOG_CLEAR           1086
#define IDC_OPEN_LOG_DIR        1087
#define IDC_START               1090
#define IDC_PAUSE_VIDEO         1091
#define IDC_OPEN_OUTPUT_DIR     1092
#define IDC_PREVIEW             1093

// 预览
#define IDC_PREVIEW_GROUP       1100
#define IDC_PREVIEW_VIEW        1101

// ⑤ 叠加层
#define IDC_OVL_GROUP           1109
#define IDC_OVL_LIST            1110
#define IDC_OVL_STATUS          1111
#define IDC_OVL_ID              1112
#define IDC_OVL_KIND            1113
#define IDC_OVL_PATH            1114
#define IDC_OVL_BROWSE          1115
#define IDC_OVL_X               1116
#define IDC_OVL_Y               1117
#define IDC_OVL_W               1118
#define IDC_OVL_H               1119
#define IDC_OVL_OPACITY         1120
#define IDC_OVL_Z               1121
#define IDC_OVL_VISIBLE         1122
#define IDC_OVL_LOOP            1123
#define IDC_OVL_STAGE           1124
#define IDC_OVL_REMOVE          1125
#define IDC_OVL_COMMIT          1126
#define IDC_OVL_DISCARD         1127

#endif  // TGUI_RESOURCE_H
