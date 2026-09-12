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
    {"打印机连接设置",  "Printer Connection",  "印表機連線設定",    "Connexion imprimante",      "Connessione stampante"},
    {"机器模式",        "Machine Mode",        "機器模式",          "Type de machine",           "Tipo di macchina"},
    {"连接方式",        "Connection Mode",     "連線方式",          "Mode de connexion",         "Modalità connessione"},
    {"连接设置",        "Connection Setup",    "連線設定",          "Configuration",             "Configura connessione"},
    {"拓竹连接",        "Bambu Connection",    "拓竹連線",          "Connexion Bambu",           "Connessione Bambu"},
    {"拓竹",            "Bambu",               "拓竹",              "Bambu",                     "Bambu"},
    {"云端监视",        "Cloud Monitor",       "雲端監視",          "Surveillance cloud",        "Monitor cloud"},
    {"局域网控制",      "LAN Control",         "區域網路控制",      "Contrôle LAN",              "Controllo LAN"},
    {"状态 / 温度 / 进度","Status / temps / progress","狀態 / 溫度 / 進度", "État / temp. / progression", "Stato / temp. / progresso"},
    {"当前",            "Current",             "目前",              "Actuel",                    "Attuale"},
    {"文件详情",        "File Detail",         "檔案詳情",          "Détail du fichier",         "Dettagli file"},
    {"详情",            "Details",             "詳情",              "Détails",                   "Dettagli"},
    {"显示",            "Display",             "顯示",              "Affichage",                 "Display"},
    {"网络",            "Network",             "網路",              "Réseau",                    "Rete"},
    {"连接",            "Connect",             "連線",              "Connexion",                 "Connessione"},
    {"模式",            "Mode",                "模式",              "Mode",                      "Modalità"},
    {"方式",            "Method",              "方式",              "Méthode",                   "Metodo"},
    {"文件",            "Files",               "檔案",              "Fichiers",                  "File"},
    {"温度",            "Temperature",         "溫度",              "Température",               "Temperatura"},
    {"主菜单",          "Main Menu",           "主選單",            "Menu principal",            "Menu principale"},
    {"切换打印机",      "Switch Printer",      "切換印表機",        "Changer d'imprimante",      "Cambia stampante"},
    {"打印机 %d",       "Printer %d",          "印表機 %d",         "Imprimante %d",             "Stampante %d"},
    {"打印机名称",      "Printer Name",        "印表機名稱",        "Nom de l'imprimante",       "Nome stampante"},
    {"云端",            "Cloud",               "雲端",              "Cloud",                     "Cloud"},
    {"当前任务",        "Current Job",         "目前工作",          "Tâche actuelle",            "Lavoro attuale"},
    {"暂无任务",        "No active job",       "暫無工作",          "Aucune tâche",              "Nessun lavoro"},
    {"已用",            "Elapsed",             "已用",              "Écoulé",                    "Trascorso"},
    {"剩余",            "Remaining",           "剩餘",              "Restant",                   "Rimanente"},
    {"喷嘴",            "Nozzle",              "噴嘴",              "Buse",                      "Ugello"},
    {"热床",            "Bed",                 "熱床",              "Plateau",                   "Piatto"},
    /* 打印机状态 */
    {"空闲",            "Standby",             "待機",              "En veille",                 "In attesa"},
    {"打印中",          "Printing",            "列印中",            "Impression en cours",       "Stampa in corso"},
    {"已暂停",          "Paused",              "已暫停",            "En pause",                  "In pausa"},
    {"打印完成",        "Complete",            "列印完成",          "Terminé",                   "Completata"},
    {"打印出错",        "Print Error",         "列印錯誤",          "Erreur d'impression",       "Errore di stampa"},
    {"已取消",          "Cancelled",           "已取消",            "Annulé",                    "Annullata"},
    {"未连接",          "Offline",             "未連線",            "Hors ligne",                "Offline"},
    {"Klipper 异常",    "Klipper Error",       "Klipper 異常",      "Erreur Klipper",            "Errore Klipper"},
    {"打印机异常",      "Printer Error",       "印表機異常",        "Erreur imprimante",         "Errore stampante"},
    {"未连接拓竹",      "Bambu offline",       "未連線拓竹",        "Bambu hors ligne",          "Bambu offline"},
    {"尚未登录",        "Not signed in",       "尚未登入",          "Non connecté",              "Accesso non effettuato"},
    {"尚未连接打印机",  "Printer not connected","尚未連線印表機",    "Imprimante non connectée",  "Stampante non connessa"},
    {"请完成登录",      "Complete sign-in",    "請完成登入",        "Terminez la connexion",     "Completa l'accesso"},
    {"请选择打印机",    "Select a printer",    "請選擇印表機",      "Choisissez une imprimante", "Seleziona una stampante"},
    {"已登录 · 请选择打印机", "Signed in · Select a printer", "已登入 · 請選擇印表機", "Connecté · Choisissez une imprimante", "Accesso · Seleziona stampante"},
    {"已登录 · 正在刷新", "Signed in · Refreshing", "已登入 · 正在重新整理", "Connecté · Actualisation", "Accesso · Aggiornamento"},
    {"已登录 · 打印机离线", "Signed in · Printer offline", "已登入 · 印表機離線", "Connecté · Imprimante hors ligne", "Accesso · Stampante offline"},
    /* 按钮 */
    {"暂停",            "Pause",               "暫停",              "Pause",                     "Pausa"},
    {"继续",            "Resume",              "繼續",              "Reprendre",                 "Riprendi"},
    {"取消",            "Cancel",              "取消",              "Annuler",                   "Annulla"},
    {"急停",            "E-Stop",              "急停",              "Arrêt urgence",             "Emergenza"},
    {"确定",            "OK",                  "確定",              "OK",                        "OK"},
    {"确认",            "Confirm",             "確認",              "Confirmer",                 "Conferma"},
    {"切换",            "Switch",              "切換",              "Changer",                   "Cambia"},
    {"删除",            "Delete",              "刪除",              "Supprimer",                 "Elimina"},
    {"确认删除?",       "Confirm?",            "確認刪除?",         "Confirmer ?",               "Confermi?"},
    {"重启",            "Restart",             "重新啟動",          "Redémarrer",                "Riavvia"},
    {"重启下位机",      "Restart MCU",         "重啟下位機",        "Redémarrer MCU",            "Riavvia MCU"},
    {"重启中",          "Restarting...",       "重啟中…",           "Redémarrage…",              "Riavvio…"},
    {"打印",            "Print",               "列印",              "Imprimer",                  "Stampa"},
    {"重新扫描",        "Rescan",              "重新掃描",          "Rescanner",                 "Riscansiona"},
    {"保存并连接",      "Save & Connect",      "儲存並連線",        "Enregistrer & connecter",  "Salva e connetti"},
    {"登录 Bambu 账号", "Sign in to Bambu",    "登入 Bambu 帳號",   "Se connecter à Bambu",      "Accedi a Bambu"},
    {"扫描局域网",      "Scan LAN",            "掃描區域網路",      "Scanner le LAN",            "Scansiona LAN"},
    {"手动输入",        "Enter Manually",      "手動輸入",          "Saisie manuelle",           "Inserimento manuale"},
    {"填写或扫描打印机","Enter or scan printer","填寫或掃描印表機", "Saisir ou scanner",         "Inserisci o scansiona"},
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
    {"打印机名称已保存",          "Printer name saved",          "印表機名稱已儲存",
     "Nom de l'imprimante enregistré",                            "Nome stampante salvato"},
    {"已切换到云端监视",          "Cloud monitoring selected",   "已切換到雲端監視",
     "Surveillance cloud activée",                                "Monitor cloud selezionato"},
    {"已切换到局域网控制",        "LAN control selected",        "已切換到區域網路控制",
     "Contrôle LAN activé",                                       "Controllo LAN selezionato"},
    {"账号登录将在桌面版接入",    "Desktop sign-in comes next",  "帳號登入將在桌面版接入",
     "Connexion bientôt sur ordinateur",                          "Accesso desktop in arrivo"},
    {"局域网扫描将在下一步接入",  "LAN scan comes next",         "區域網路掃描將在下一步接入",
     "Scan LAN à la prochaine étape",                             "Scansione LAN nel prossimo passo"},
    {"手动填写 IP、序列号和访问码","Enter IP, serial and access code","手動填寫 IP、序號和存取碼",
     "Saisir IP, série et code d'accès",                          "Inserisci IP, seriale e codice"},
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
    {"实时状态已同步",  "Live status synced",  "即時狀態已同步",    "État en direct synchronisé", "Stato live sincronizzato"},
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
    {"拓竹云登录",      "Bambu Cloud sign-in", "拓竹雲端登入",      "Connexion Bambu Cloud",    "Accesso Bambu Cloud"},
    {"正在登录",        "Signing in…",         "正在登入…",         "Connexion…",               "Accesso…"},
    {"已登录",          "Signed in",           "已登入",            "Connecté",                 "Accesso effettuato"},
    {"登录失败",        "Sign-in failed",      "登入失敗",          "Échec de connexion",       "Accesso non riuscito"},
    {"双重验证",        "Two-factor check",    "雙重驗證",          "Double authentification",  "Verifica a due fattori"},
    {"邮箱验证",        "Email verification", "信箱驗證",          "Vérification e-mail",      "Verifica e-mail"},
    {"验证码登录",      "Code sign-in",        "驗證碼登入",        "Connexion par code",       "Accesso con codice"},
    {"桌面版功能",      "Desktop feature",     "桌面版功能",        "Fonction bureau",          "Funzione desktop"},
    {"账号地区  中国",  "Account region  China", "帳號地區  中國", "Région du compte  Chine",  "Regione account  Cina"},
    {"账号地区  全球",  "Account region  Global", "帳號地區  全球", "Région du compte  Monde",  "Regione account  Globale"},
    {"中国区",          "China region",         "中國區",            "Région Chine",             "Regione Cina"},
    {"全球区",          "Global region",        "全球區",            "Région monde",             "Regione globale"},
    {"邮箱验证码",      "Email code",          "信箱驗證碼",        "Code e-mail",              "Codice e-mail"},
    {"手机号",          "Phone number",        "手機號碼",          "Numéro de téléphone",      "Numero di telefono"},
    {"短信验证码",      "SMS code",            "簡訊驗證碼",        "Code SMS",                 "Codice SMS"},
    {"密码登录",        "Password sign-in",    "密碼登入",          "Connexion par mot de passe", "Accesso con password"},
    {"验证码",          "Verification code",   "驗證碼",            "Code de vérification",     "Codice di verifica"},
    {"确认登录",        "Sign in",             "確認登入",          "Se connecter",             "Accedi"},
    {"重新登录",        "Start over",          "重新登入",          "Recommencer",              "Ricomincia"},
    {"退出登录",        "Sign out",            "登出",              "Se déconnecter",           "Esci"},
    {"刷新",            "Refresh",             "重新整理",          "Actualiser",               "Aggiorna"},
    {"在线",            "Online",              "線上",              "En ligne",                 "Online"},
    {"离线",            "Offline",             "離線",              "Hors ligne",               "Offline"},
    {"请先输入邮箱",    "Enter your email first", "請先輸入信箱",   "Saisissez d'abord l'e-mail", "Inserisci prima l'e-mail"},
    {"请先输入手机号",  "Enter your phone number first", "請先輸入手機號碼", "Saisissez d'abord le numéro", "Inserisci prima il numero"},
    {"请先输入密码",    "Enter your password first", "請先輸入密碼", "Saisissez d'abord le mot de passe", "Inserisci prima la password"},
    {"请先输入验证码",  "Enter the code first", "請先輸入驗證碼",    "Saisissez d'abord le code", "Inserisci prima il codice"},
    {"登录任务正在运行", "A sign-in request is already running", "登入工作正在執行", "Une connexion est déjà en cours", "Accesso già in corso"},
    {"刷新任务正在运行", "A refresh is already running", "重新整理正在執行", "Une actualisation est en cours", "Aggiornamento già in corso"},
    {"验证码提交失败",  "Could not submit the code", "驗證碼提交失敗", "Échec d'envoi du code",     "Invio del codice non riuscito"},
    {"已选择打印机",    "Printer selected",     "已選擇印表機",      "Imprimante sélectionnée",  "Stampante selezionata"},
    {"请输入拓竹账号",  "Enter your Bambu account", "請輸入拓竹帳號", "Saisissez votre compte Bambu", "Inserisci il tuo account Bambu"},
    {"已恢复保存的登录", "Saved sign-in restored", "已恢復儲存的登入", "Connexion enregistrée restaurée", "Accesso salvato ripristinato"},
    {"已登录，但打印机列表获取失败", "Signed in, but printers could not be loaded", "已登入，但無法取得印表機清單", "Connecté, mais liste indisponible", "Accesso riuscito, elenco non disponibile"},
    {"登录成功，账号下没有找到打印机", "Signed in, no printers found", "登入成功，帳號下找不到印表機", "Connecté, aucune imprimante trouvée", "Accesso riuscito, nessuna stampante"},
    {"登录成功，找到 %d 台打印机", "Signed in, found %d printers", "登入成功，找到 %d 台印表機", "Connecté, %d imprimantes trouvées", "Accesso riuscito, trovate %d stampanti"},
    {"登录令牌过长，无法保存", "Sign-in token is too long to save", "登入權杖過長，無法儲存", "Jeton trop long pour être enregistré", "Token troppo lungo da salvare"},
    {"登录成功，但加密令牌保存失败", "Signed in, but the encrypted token could not be saved", "登入成功，但無法儲存加密權杖", "Connecté, mais jeton non enregistré", "Accesso riuscito, token non salvato"},
    {"已登录，但无法取得云端监视身份", "Signed in, but cloud monitoring is unavailable", "已登入，但無法取得雲端監視身分", "Connecté, mais surveillance indisponible", "Accesso riuscito, monitoraggio non disponibile"},
    {"登录被拓竹服务拒绝", "Sign-in was rejected by Bambu", "登入遭拓竹服務拒絕", "Connexion refusée par Bambu", "Accesso rifiutato da Bambu"},
    {"无法读取登录响应", "Could not read the sign-in response", "無法讀取登入回應", "Réponse de connexion illisible", "Impossibile leggere la risposta"},
    {"拓竹要求双重验证，但没有返回验证会话", "Bambu requires 2FA but returned no verification session", "拓竹要求雙重驗證，但未傳回驗證工作階段", "Bambu exige la 2FA sans session valide", "Bambu richiede 2FA senza sessione valida"},
    {"请输入验证器应用中的 6 位验证码", "Enter the 6-digit authenticator code", "請輸入驗證器應用程式中的 6 位驗證碼", "Saisissez le code à 6 chiffres", "Inserisci il codice a 6 cifre"},
    {"验证码已发送，请输入验证码", "Code sent; enter the verification code", "驗證碼已傳送，請輸入驗證碼", "Code envoyé ; saisissez-le", "Codice inviato; inseriscilo"},
    {"登录响应中没有令牌", "No token in the sign-in response", "登入回應中沒有權杖", "Aucun jeton dans la réponse", "Nessun token nella risposta"},
    {"无法初始化 Windows 网络服务", "Could not initialize Windows networking", "無法初始化 Windows 網路服務", "Impossible d'initialiser le réseau Windows", "Impossibile inizializzare la rete Windows"},
    {"无法连接拓竹登录服务", "Could not reach the Bambu sign-in service", "無法連線拓竹登入服務", "Service de connexion Bambu inaccessible", "Servizio di accesso Bambu non raggiungibile"},
    {"验证码已发送到手机，请输入验证码", "Code sent to your phone; enter it", "驗證碼已傳送至手機，請輸入驗證碼", "Code envoyé au téléphone ; saisissez-le", "Codice inviato al telefono; inseriscilo"},
    {"验证码已发送到邮箱，请输入验证码", "Code sent to your email; enter it", "驗證碼已傳送至信箱，請輸入驗證碼", "Code envoyé par e-mail ; saisissez-le", "Codice inviato via e-mail; inseriscilo"},
    {"验证码发送失败", "Could not send the verification code", "驗證碼傳送失敗", "Échec de l'envoi du code", "Invio del codice non riuscito"},
    {"验证码不正确",    "Incorrect verification code", "驗證碼不正確", "Code de vérification incorrect", "Codice di verifica errato"},
    {"无法建立双重验证会话", "Could not start the 2FA session", "無法建立雙重驗證工作階段", "Impossible de démarrer la session 2FA", "Impossibile avviare la sessione 2FA"},
    {"验证器验证码不正确", "Incorrect authenticator code", "驗證器驗證碼不正確", "Code d'authentification incorrect", "Codice autenticatore errato"},
    {"验证码已通过，但登录服务没有返回令牌", "Code accepted, but no sign-in token was returned", "驗證碼已通過，但登入服務未傳回權杖", "Code accepté, mais aucun jeton reçu", "Codice accettato, ma nessun token ricevuto"},
    {"正在刷新打印机列表…", "Refreshing printer list…", "正在重新整理印表機清單…", "Actualisation des imprimantes…", "Aggiornamento elenco stampanti…"},
    {"正在连接拓竹云服务…", "Connecting to Bambu Cloud…", "正在連線拓竹雲端服務…", "Connexion au cloud Bambu…", "Connessione a Bambu Cloud…"},
    {"无法启动登录任务", "Could not start the sign-in task", "無法啟動登入工作", "Impossible de démarrer la connexion", "Impossibile avviare l'accesso"},
    {"内存不足，无法完成操作", "Out of memory; could not finish", "記憶體不足，無法完成操作", "Mémoire insuffisante, opération impossible", "Memoria insufficiente, operazione non riuscita"},
    {"账号登录目前只在 Windows 桌面版提供", "Account sign-in is currently available on Windows desktop only", "帳號登入目前僅在 Windows 桌面版提供", "Connexion disponible uniquement sous Windows", "Accesso disponibile solo su Windows"},
    {"已连接，正在读取打印机状态…", "Connected, reading printer status…", "已連線，正在讀取印表機狀態…", "Connecté, lecture de l'état…", "Connesso, lettura stato…"},
    {"无法取得云端实时监视身份，请重新登录", "Cloud monitor credentials unavailable; sign in again", "無法取得雲端即時監視身分，請重新登入", "Identifiants cloud indisponibles ; reconnectez-vous", "Credenziali cloud non disponibili; accedi di nuovo"},
    {"正在连接拓竹实时状态…", "Connecting to Bambu live status…", "正在連線拓竹即時狀態…", "Connexion à l'état Bambu en direct…", "Connessione allo stato Bambu…"},
    {"云端实时连接被拒绝，请重新登录", "Cloud live connection rejected; sign in again", "雲端即時連線遭拒，請重新登入", "Connexion cloud refusée ; reconnectez-vous", "Connessione cloud rifiutata; accedi di nuovo"},
    {"实时连接中断，正在重试…", "Live connection lost; retrying…", "即時連線中斷，正在重試…", "Connexion perdue ; nouvelle tentative…", "Connessione persa; nuovo tentativo…"},
    {"登录信息不完整，请退出后重新登录", "Sign-in data incomplete; sign out and sign in again", "登入資訊不完整，請登出後重新登入", "Données incomplètes ; reconnectez-vous", "Dati incompleti; esci e accedi di nuovo"},
    {"无法启动实时状态任务", "Could not start the live status task", "無法啟動即時狀態工作", "Impossible de démarrer l'état en direct", "Impossibile avviare lo stato live"},
    {"登录后可监看温度 / 进度 / AMS / 故障", "Sign in to monitor temperatures / progress / AMS / errors",
     "登入後可監看溫度 / 進度 / AMS / 故障", "Connectez-vous pour suivre températures / progression / AMS / erreurs",
     "Accedi per monitorare temperature, progresso, AMS ed errori"},
    {"需要打印机 IP、序列号和访问码", "Printer IP, serial and access code required",
     "需要印表機 IP、序號和存取碼", "IP, numéro de série et code d'accès requis",
     "Servono IP, seriale e codice di accesso"},
    {"云端监视 · 温度只读", "Cloud Monitor · Temperatures read only", "雲端監視 · 溫度唯讀",
     "Cloud · Températures en lecture seule", "Cloud · Temperature in sola lettura"},
    {"云端监视 · 仅查看打印状态", "Cloud Monitor · Job status only", "雲端監視 · 僅查看列印狀態",
     "Cloud · État d'impression uniquement", "Cloud · Solo stato stampa"},
    {"云端监视 · 只读", "Cloud Monitor · Read only", "雲端監視 · 唯讀",
     "Surveillance cloud · Lecture seule", "Monitor cloud · Sola lettura"},
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
    {"是否切换为 %s 型号？", "Switch to %s?", "是否切換為 %s 型號？",
     "Passer au modèle %s ?",                                    "Passare al modello %s?"},
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
