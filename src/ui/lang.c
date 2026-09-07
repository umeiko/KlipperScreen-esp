/*
 * UI 多语言翻译表。key 为源码里的简体中文字面量，运行时精确匹配。
 * 新增 UI 中文串后在此追加一行即可；表未覆盖的串原样显示。
 * 某语言译文留 NULL 时回退显示简体中文 key。
 */
#include "lang.h"
#include "app_settings.h"
#include <string.h>

static ui_lang_t cur = UI_LANG_ZH;

/* 语言注册表：顺序即设置面板下拉框顺序，与 ui_lang_t 枚举一一对应 */
static const struct { ui_lang_t lang; const char *code; const char *name; } langs[] = {
    { UI_LANG_ZH,    "zh", "简体中文" },
    { UI_LANG_EN,    "en", "English"  },
    { UI_LANG_ZH_TW, "tw", "繁體中文" },
    { UI_LANG_FR,    "fr", "Français" },
    { UI_LANG_IT,    "it", "Italiano" },
};

typedef struct {
    const char *key;          /* 简体中文（即源码字面量） */
    const char *en, *tw, *fr, *it;
} dict_entry_t;

/* clang-format off */
static const dict_entry_t dict[] = {
    /* 面板标题 / 主菜单            English                    繁體中文                Français                       Italiano */
    {"打印状态",        "Job Status",          "列印狀態",          "Statut d'impression",       "Stato stampa"},
    {"温度控制",        "Temperature",         "溫度控制",          "Température",               "Temperatura"},
    {"移动",            "Move",                "移動",              "Déplacer",                  "Muovi"},
    {"挤出",            "Extrude",             "擠出",              "Extruder",                  "Estrudi"},
    {"打印文件",        "Print Files",         "列印檔案",          "Fichiers d'impression",     "File di stampa"},
    {"设置",            "Settings",            "設定",              "Paramètres",                "Impostazioni"},
    {"无线网络",        "WiFi",                "無線網路",          "Wi-Fi",                     "Wi-Fi"},
    {"Moonraker 连接",  "Moonraker",           "Moonraker 連線",    "Moonraker",                 "Moonraker"},
    {"文件详情",        "File Detail",         "檔案詳情",          "Détail du fichier",         "Dettagli file"},
    {"文件",            "Files",               "檔案",              "Fichiers",                  "File"},
    {"温度",            "Temperature",         "溫度",              "Température",               "Temperatura"},
    {"主菜单",          "Main Menu",           "主選單",            "Menu principal",            "Menu principale"},
    {"切换打印机",      "Switch Printer",      "切換印表機",        "Changer d'imprimante",      "Cambia stampante"},
    {"打印机 %d",       "Printer %d",          "印表機 %d",         "Imprimante %d",             "Stampante %d"},
    /* 打印机状态 */
    {"空闲",            "Standby",             "待機",              "En veille",                 "In attesa"},
    {"打印中",          "Printing",            "列印中",            "Impression en cours",       "Stampa in corso"},
    {"已暂停",          "Paused",              "已暫停",            "En pause",                  "In pausa"},
    {"打印完成",        "Complete",            "列印完成",          "Terminé",                   "Completata"},
    {"打印出错",        "Print Error",         "列印錯誤",          "Erreur d'impression",       "Errore di stampa"},
    {"已取消",          "Cancelled",           "已取消",            "Annulé",                    "Annullata"},
    {"未连接",          "Offline",             "未連線",            "Hors ligne",                "Offline"},
    {"Klipper 异常",    "Klipper Error",       "Klipper 異常",      "Erreur Klipper",            "Errore Klipper"},
    /* 按钮 */
    {"暂停",            "Pause",               "暫停",              "Pause",                     "Pausa"},
    {"继续",            "Resume",              "繼續",              "Reprendre",                 "Riprendi"},
    {"取消",            "Cancel",              "取消",              "Annuler",                   "Annulla"},
    {"急停",            "E-Stop",              "急停",              "Arrêt urgence",             "Emergenza"},
    {"确定",            "OK",                  "確定",              "OK",                        "OK"},
    {"确认",            "Confirm",             "確認",              "Confirmer",                 "Conferma"},
    {"删除",            "Delete",              "刪除",              "Supprimer",                 "Elimina"},
    {"确认删除?",       "Confirm?",            "確認刪除?",         "Confirmer ?",               "Confermi?"},
    {"重启",            "Restart",             "重新啟動",          "Redémarrer",                "Riavvia"},
    {"重启下位机",      "Restart MCU",         "重啟下位機",        "Redémarrer MCU",            "Riavvia MCU"},
    {"重启中",          "Restarting...",       "重啟中…",           "Redémarrage…",              "Riavvio…"},
    {"打印",            "Print",               "列印",              "Imprimer",                  "Stampa"},
    {"重新扫描",        "Rescan",              "重新掃描",          "Rescanner",                 "Riscansiona"},
    {"保存并连接",      "Save & Connect",      "儲存並連線",        "Enregistrer & connecter",  "Salva e connetti"},
    {"全部冷却",        "Cooldown All",        "全部冷卻",          "Tout refroidir",            "Raffredda tutto"},
    {"冷却",            "Cooldown",            "冷卻",              "Refroidir",                 "Raffredda"},
    {"全部轴归位",      "Home All",            "全部軸歸位",        "Origine tout",              "Home tutto"},
    {"轴归位",          "Home",                "軸歸位",            "Origine",                   "Home"},
    {"全部",            "All",                 "全部",              "Tout",                      "Tutto"},
    {"装料",            "Load",                "進料",              "Charger",                   "Carica"},
    {"退料",            "Unload",              "退料",              "Décharger",                 "Scarica"},
    {"回抽",            "Retract",             "回抽",              "Rétracter",                 "Ritrai"},
    /* 提示 / toast */
    {"开始打印",        "Print started",       "開始列印",          "Impression lancée",         "Stampa avviata"},
    {"已取消打印",      "Print cancelled",     "已取消列印",        "Impression annulée",        "Stampa annullata"},
    {"已删除",          "Deleted",             "已刪除",            "Supprimé",                  "Eliminato"},
    {"已急停（M112）",  "E-Stop sent (M112)",  "已急停（M112）",    "Arrêt urgence envoyé (M112)","Emergenza inviata (M112)"},
    {"已发送重启指令",  "Restart command sent","已送出重啟指令",    "Redémarrage envoyé",        "Riavvio inviato"},
    {"正在打印中，无法开始新任务", "Busy printing, cannot start", "列印中，無法開始新任務",
     "Impression en cours",                                        "Stampa in corso, attendi"},
    {"没有可重启的文件", "No file to restart",  "沒有可重啟的檔案",  "Aucun fichier à relancer",  "Nessun file da riavviare"},
    {"喷嘴温度过低，无法挤出",     "Nozzle too cold to extrude", "噴嘴溫度過低，無法擠出",
     "Buse trop froide",                                           "Ugello troppo freddo"},
    {"连接失败，请检查密码",       "Connection failed, check password", "連線失敗，請檢查密碼",
     "Échec connexion, vérifiez le mot de passe",                  "Connessione fallita, controlla password"},
    {"已保存，正在连接",           "Saved, connecting",          "已儲存，連線中",
     "Enregistré, connexion…",                                     "Salvato, connessione…"},
    {"保存失败",                  "Save failed",                 "儲存失敗",
     "Échec enregistrement",                                       "Salvataggio fallito"},
    {"请先填写主机地址",           "Enter host address first",   "請先填寫主機位址",
     "Saisissez l'adresse hôte",                                   "Inserisci l'indirizzo host"},
    {"获取失败，请检查连接",       "Fetch failed, check connection", "取得失敗，請檢查連線",
     "Échec récupération, vérifiez la connexion",                  "Recupero fallito, controlla connessione"},
    /* 状态行 */
    {"加载中…",         "Loading…",            "載入中…",           "Chargement…",               "Caricamento…"},
    {"暂无 GCode 文件", "No GCode files",      "暫無 GCode 檔案",   "Aucun fichier GCode",       "Nessun file GCode"},
    {"未连接 Moonraker","Moonraker offline",   "未連線 Moonraker",  "Moonraker hors ligne",      "Moonraker offline"},
    {"扫描中…",         "Scanning…",           "掃描中…",           "Recherche…",                "Scansione…"},
    {"未发现网络",      "No networks found",   "未發現網路",        "Aucun réseau trouvé",       "Nessuna rete trovata"},
    {"扫描失败，点列表上方重试", "Scan failed, tap above to retry", "掃描失敗，點列表上方重試",
     "Échec du scan, touchez ci-dessus",                           "Scansione fallita, tocca sopra"},
    {"连接中…",         "Connecting…",         "連線中…",           "Connexion…",                "Connessione…"},
    {"离线（自动重连中）", "Offline (reconnecting)", "離線（自動重連中）", "Hors ligne (reconnexion)", "Offline (riconnessione)"},
    {"未配置",          "Not configured",      "未配置",            "Non configuré",             "Non configurato"},
    {"未设置",          "Not set",             "未設定",            "Non défini",                "Non impostato"},
    {"已连接",          "Connected",           "已連線",            "Connecté",                  "Connesso"},
    {"已连接 %dms",     "Connected %dms",      "已連線 %dms",       "Connecté %dms",             "Connesso %dms"},
    {"已连接 %s",       "Connected %s",        "已連線 %s",         "Connecté à %s",             "Connesso a %s"},
    {"正在连接 %s",     "Connecting %s",       "正在連線 %s",       "Connexion à %s",            "Connessione a %s"},
    {"连接到 %s",       "Connect to %s",       "連線到 %s",         "Se connecter à %s",         "Connetti a %s"},
    {"连接超时",        "Connection timeout",  "連線逾時",          "Délai de connexion",        "Timeout connessione"},
    {"挤出中…",         "Extruding…",          "擠出中…",           "Extrusion…",                "Estrusione…"},
    {"回抽中…",         "Retracting…",         "回抽中…",           "Rétraction…",               "Retrazione…"},
    {"喷嘴加热中",      "Nozzle heating",      "噴嘴加熱中",        "Buse en chauffe",           "Ugello in riscaldamento"},
    {"热床加热中",      "Bed heating",         "熱床加熱中",        "Lit en chauffe",            "Piatto in riscaldamento"},
    {"喷嘴已关闭",      "Nozzle off",          "噴嘴已關閉",        "Buse éteinte",              "Ugello spento"},
    {"热床已关闭",      "Bed off",             "熱床已關閉",        "Lit éteint",                "Piatto spento"},
    /* 表单 / 设置项 */
    {"Moonraker 主机（IP 或域名）", "Host (IP or name)", "主機（IP 或網域名稱）",
     "Hôte (IP ou nom)",                                           "Host (IP o nome)"},
    {"端口",            "Port",                "連接埠",            "Port",                      "Porta"},
    {"主机",            "Host",                "主機",              "Hôte",                      "Host"},
    {"API Key（可留空）", "API Key (optional)","API Key（可留空）", "Clé API (optionnel)",       "Chiave API (opzionale)"},
    {"密码",            "Password",            "密碼",              "Mot de passe",              "Password"},
    {"加密",            "Secured",             "加密",              "Sécurisé",                  "Protetta"},
    {"开放",            "Open",                "開放",              "Ouvert",                    "Aperta"},
    {"喷嘴目标温度",    "Nozzle target",       "噴嘴目標溫度",      "Cible buse",                "Target ugello"},
    {"热床目标温度",    "Bed target",          "熱床目標溫度",      "Cible lit",                 "Target piatto"},
    {"主题",            "Theme",               "主題",              "Thème",                     "Tema"},
    {"显示设置",        "Display",             "顯示設定",          "Affichage",                 "Display"},
    {"反色",            "Invert colors",       "反色",              "Inverser les couleurs",     "Inverti colori"},
    {"旋转 180°",       "Rotate 180°",         "旋轉 180°",         "Rotation 180°",             "Ruota 180°"},
    {"深色",            "Dark",                "深色",              "Sombre",                    "Scuro"},
    {"浅色",            "Light",               "淺色",              "Clair",                     "Chiaro"},
    {"背光",            "Backlight",           "背光",              "Rétroéclairage",            "Retroilluminazione"},
    {"版本",            "Version",             "版本",              "Version",                   "Versione"},
    {"语言",            "Language",            "語言",              "Langue",                    "Lingua"},
    {"状态",            "Status",              "狀態",              "État",                      "Stato"},
    {"自动息屏",        "Screen Off",          "自動熄屏",          "Extinction écran",          "Spegnimento schermo"},
    {"15秒",            "15s",                 "15秒",              "15 s",                      "15 s"},
    {"30秒",            "30s",                 "30秒",              "30 s",                      "30 s"},
    {"1分钟",           "1 min",               "1分鐘",             "1 min",                     "1 min"},
    {"5分钟",           "5 min",               "5分鐘",             "5 min",                     "5 min"},
    {"15分钟",          "15 min",              "15分鐘",            "15 min",                    "15 min"},
    {"30分钟",          "30 min",              "30分鐘",            "30 min",                    "30 min"},
    {"1小时",           "1 hour",              "1小時",             "1 h",                       "1 h"},
    {"永不",            "Never",               "永不",              "Jamais",                    "Mai"},
    /* 确认对话框 / 格式化串 */
    {"确认急停？\n打印机将立即停止所有运动和加热",
     "E-Stop?\nAll motion and heaters stop immediately",
     "確認急停？\n印表機將立即停止所有移動與加熱",
     "Arrêt urgence ?\nMouvements et chauffages stoppés",
     "Emergenza?\nMovimenti e riscaldatori fermi subito"},
    {"确认重启下位机？\n（FIRMWARE_RESTART）",
     "Restart MCU?\n(FIRMWARE_RESTART)",
     "確認重啟下位機？\n（FIRMWARE_RESTART）",
     "Redémarrer le MCU ?\n(FIRMWARE_RESTART)",
     "Riavviare l'MCU?\n(FIRMWARE_RESTART)"},
    {"已用 %s\n剩余 %s", "Elapsed %s\nLeft %s", "已用 %s\n剩餘 %s",
     "Écoulé %s\nRestant %s",                                      "Trascorso %s\nRimanente %s"},
    {"喷嘴 %d°C（挤出需 ≥ %d°C）", "Nozzle %d°C (min %d°C)", "噴嘴 %d°C（擠出需 ≥ %d°C）",
     "Buse %d°C (min %d°C)",                                       "Ugello %d°C (min %d°C)"},
};
/* clang-format on */

void ui_lang_set(ui_lang_t l)
{
    if (l >= 0 && l < UI_LANG_COUNT) cur = l;
}
ui_lang_t ui_lang_get(void) { return cur; }

unsigned ui_lang_count(void) { return sizeof(langs) / sizeof(langs[0]); }

const char *ui_lang_code(ui_lang_t l)
{
    for (unsigned i = 0; i < ui_lang_count(); i++)
        if (langs[i].lang == l) return langs[i].code;
    return "zh";
}

const char *ui_lang_name(ui_lang_t l)
{
    for (unsigned i = 0; i < ui_lang_count(); i++)
        if (langs[i].lang == l) return langs[i].name;
    return langs[0].name;
}

ui_lang_t ui_lang_from_code(const char *code)
{
    if (code)
        for (unsigned i = 0; i < ui_lang_count(); i++)
            if (strcmp(langs[i].code, code) == 0) return langs[i].lang;
    return UI_LANG_ZH;
}

static const char *entry_tr(const dict_entry_t *d, ui_lang_t l)
{
    switch (l) {
    case UI_LANG_EN:    return d->en;
    case UI_LANG_ZH_TW: return d->tw;
    case UI_LANG_FR:    return d->fr;
    case UI_LANG_IT:    return d->it;
    default:            return NULL;
    }
}

const char *ui_tr(const char *zh)
{
    if (cur == UI_LANG_ZH || !zh) return zh;
    for (unsigned i = 0; i < sizeof(dict) / sizeof(dict[0]); i++) {
        if (strcmp(dict[i].key, zh) == 0) {
            const char *t = entry_tr(&dict[i], cur);
            return t ? t : zh;
        }
    }
    return zh;
}

void ui_lang_load(void)
{
    char lang[8] = "zh";
    settings_load_language(lang, sizeof(lang));
    cur = ui_lang_from_code(lang);
}
