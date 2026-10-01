/**
 * @file    screen_layout.h
 * @brief   ПРИВАТНЫЙ заголовок модулей App/Screen (screen.c, screen_channel.c,
 *          screen_icons.c, screen_menu.c): индексы строк TextField, геометрия и
 *          цвета экрана. Только макросы/enum — никакого кода и состояния; снаружи
 *          App/Screen не включается (публичный интерфейс — screen.h).
 *          Описание раскладки — в шапке screen.c.
 */
#ifndef SCREEN_LAYOUT_H
#define SCREEN_LAYOUT_H

#include <stdint.h>
#include "display.h"
#include "channel.h"
#include "sleep_icon.h"
#include "presleep_icon.h"
#include "solder_icon.h"
#include "desolder_icon.h"

/* ---- Индексы строк TextField ---- */
enum {
    LINE_INFO = 0,
    LINE_SOLDER_TITLE,
    LINE_SOLDER_CURRENT,      /* число, шрифт Comic_60_dig — непусто только в CHANNEL_CONTENT_NORMAL */
    LINE_SOLDER_FAULT_MSG,    /* 1-я строка "Обрыв"/"КЗ"/"ERR", шрифт AntiquaB_18_uni — непусто только в CHANNEL_CONTENT_FAULT */
    LINE_SOLDER_FAULT_MSG2,   /* 2-я строка ("RTD"/"нагревателя"/"ADS1220") — см. print_fault_message_2line() */
    LINE_SOLDER_DISABLED_MSG, /* "ВЫКЛ", шрифт AntiquaB_32_uni — непусто только в CHANNEL_CONTENT_DISABLED */
    LINE_SOLDER_TARGET,       /* уставка (ВРЕМЕННОЕ отладочное поле), опущена на SCREEN_TARGET_SHIFT_Y — в строку пресетов */
    LINE_SOLDER_SLEEP_TEMP,   /* текущая температура в PRESLEEP/SLEEP (AntiquaB_32_uni) — на прежнем месте уставки, под иконкой сна */
    LINE_DESOLDER_TITLE,
    LINE_DESOLDER_CURRENT,
    LINE_DESOLDER_FAULT_MSG,
    LINE_DESOLDER_FAULT_MSG2,
    LINE_DESOLDER_DISABLED_MSG,
    LINE_DESOLDER_TARGET,
    LINE_DESOLDER_SLEEP_TEMP,
    LINE_PRESET_1,
    LINE_PRESET_2,
    LINE_PRESET_3,
    LINE_INFO_SLEEP_SOLDER,   /* инфозона слева — таймер сна паяльника */
    LINE_INFO_SLEEP_DESOLDER, /* инфозона справа — таймер сна отсоса */
    LINE_MENU_TITLE,          /* сервисное меню — "Настройка Паяльник/Отсос" */
    LINE_MENU_ITEM_0,         /* сервисное меню — 8 строк списка (максимум для уровня User);
                                 при MENU_STATE_EXPERT_WARNING строки 0-2 заняты текстом
                                 предупреждения, см. ScreenMenu_Render() */
    LINE_MENU_ITEM_1,
    LINE_MENU_ITEM_2,
    LINE_MENU_ITEM_3,
    LINE_MENU_ITEM_4,
    LINE_MENU_ITEM_5,
    LINE_MENU_ITEM_6,
    LINE_MENU_ITEM_7,
};

/* ---- Геометрия ---- */
#define SCREEN_WIDTH        (320U)
#define SCREEN_HEIGHT       (240U)
#define SCREEN_INFO_HEIGHT  (30U)
#define SCREEN_DIVIDER_X0   (159U)
#define SCREEN_DIVIDER_X1   (160U) /* разделитель 2px шириной: X0..X1 включительно */

#define SCREEN_TITLE_Y      (36U)
/* Высота титульной полосы — высота более высокой из двух иконок заголовков
 * (Desolder, 31px при ширине 120px), чтобы CURRENT-блок начинался на одном
 * и том же Y в обеих половинах экрана; иконка Solder (17px) центрируется по
 * вертикали в той же полосе, не растягиваясь. */
#define SCREEN_TITLE_HEIGHT ((DESOLDER_ICON_BITMAP_H > SOLDER_ICON_BITMAP_H) ? DESOLDER_ICON_BITMAP_H : SOLDER_ICON_BITMAP_H)

/* Центры половин экрана — используются TextField_PrintfCentered() для
 * температур, реальная ширина текста меряется на лету (не типовая). */
#define SCREEN_HALF_CENTER_LEFT_X   ((SCREEN_DIVIDER_X0) / 2U)
#define SCREEN_HALF_CENTER_RIGHT_X  (SCREEN_DIVIDER_X1 + 1U + SCREEN_HALF_CENTER_LEFT_X)

/* Иконки заголовков каналов (растры SolderIcon_Bitmap/DesolderIcon_Bitmap, см.
 * solder_icon.h/desolder_icon.h) центрируются по X тем же center_x, что и
 * CURRENT/TARGET своей половины, и по Y — внутри общей титульной полосы
 * (SCREEN_TITLE_HEIGHT), а не по общему верхнему краю: иконки разной высоты
 * (17 и 31px при ширине 120px) иначе не совпадали бы по центру. */
#define SCREEN_SOLDER_ICON_X   ((uint16_t)(SCREEN_HALF_CENTER_LEFT_X  - SOLDER_ICON_BITMAP_W   / 2U))
#define SCREEN_DESOLDER_ICON_X ((uint16_t)(SCREEN_HALF_CENTER_RIGHT_X - DESOLDER_ICON_BITMAP_W / 2U))
#define SCREEN_SOLDER_ICON_Y   ((uint16_t)(SCREEN_TITLE_Y + (SCREEN_TITLE_HEIGHT - SOLDER_ICON_BITMAP_H)   / 2U + 5U)) /* +5px — ручная подгонка по месту */
#define SCREEN_DESOLDER_ICON_Y ((uint16_t)(SCREEN_TITLE_Y + (SCREEN_TITLE_HEIGHT - DESOLDER_ICON_BITMAP_H) / 2U))

#define SCREEN_PRESETS_Y (210U)
/* Пресеты:
 *  - preset1: 10px от левого края (TextField_Printf, фиксированный x)
 *  - preset2: центрирован на оси разделителя (TextField_PrintfCentered)
 *  - preset3: правый край в 10px от правого края экрана (TextField_PrintfRightAligned) */
#define SCREEN_PRESET1_X (10U)
#define SCREEN_PRESET2_CENTER_X (SCREEN_DIVIDER_X0 + 1U)
#define SCREEN_PRESET3_RIGHT_EDGE_X (SCREEN_WIDTH - 10U)

/* Разделитель НЕ доходит до строки пресетов — та зона общая (одна строка
 * на экран). Останавливаем линию с небольшим отступом сверху от SCREEN_PRESETS_Y. */
#define SCREEN_DIVIDER_Y1   (SCREEN_PRESETS_Y - 6U)

/* Гейдж мощности — узкая вертикальная колонка у разделителя,
 * "как у спиртового термометра": заполняется СНИЗУ ВВЕРХ на долю
 * Control_GetSmoothedPowerPct(ch)/100 от своей высоты, остаток сверху —
 * тусклый "трек" (видна вся шкала целиком даже при 0%, не только пустое
 * место). Та же вертикальная протяжённость, что и у статического
 * разделителя (SCREEN_INFO_HEIGHT..SCREEN_DIVIDER_Y1) — визуально одна
 * группа с ним. */
#define SCREEN_POWER_BAR_WIDTH  (8U)
#define SCREEN_POWER_BAR_GAP    (3U)  /* зазор от разделителя */
#define SCREEN_POWER_BAR_Y0     (SCREEN_INFO_HEIGHT)
#define SCREEN_POWER_BAR_Y1     (SCREEN_DIVIDER_Y1)
#define SCREEN_POWER_BAR_HEIGHT ((uint16_t)(SCREEN_POWER_BAR_Y1 - SCREEN_POWER_BAR_Y0 + 1U))
#define SCREEN_SOLDER_POWER_BAR_X0   ((uint16_t)(SCREEN_DIVIDER_X0 - SCREEN_POWER_BAR_GAP - SCREEN_POWER_BAR_WIDTH))
#define SCREEN_DESOLDER_POWER_BAR_X0 ((uint16_t)(SCREEN_DIVIDER_X1 + 1U + SCREEN_POWER_BAR_GAP))
#define COLOR_POWER_FILL  DISPLAY_RGB565(255, 90, 0)  /* тёплый оранжевый — "греет" */
#define COLOR_POWER_TRACK DISPLAY_RGB565(40, 40, 40)  /* тусклый трек — видна вся шкала, а не голый фон */

/* Текущая+целевая температура центрируются по вертикали в промежутке между
 * низом заголовка и верхом строки пресетов (тот же отступ 6px, что и у
 * разделителя). CURRENT_HEIGHT/TARGET_HEIGHT — высоты шрифтов, GAP — зазор. */
#define SCREEN_CURRENT_HEIGHT (67U)
#define SCREEN_TARGET_HEIGHT  (18U)
#define SCREEN_TEMP_GAP       (10U)
#define SCREEN_TEMP_BAND_TOP    (SCREEN_TITLE_Y + SCREEN_TITLE_HEIGHT + 2U)
#define SCREEN_TEMP_BAND_BOTTOM (SCREEN_DIVIDER_Y1)
#define SCREEN_TEMP_BLOCK_HEIGHT (SCREEN_CURRENT_HEIGHT + SCREEN_TEMP_GAP + SCREEN_TARGET_HEIGHT)
#define SCREEN_CURRENT_Y ((uint16_t)(SCREEN_TEMP_BAND_TOP + \
    ((SCREEN_TEMP_BAND_BOTTOM - SCREEN_TEMP_BAND_TOP) - SCREEN_TEMP_BLOCK_HEIGHT) / 2U))
/* Прежнее место уставки (сразу под CURRENT-полосой) теперь занимает текущая
 * температура в фазах сна (PRESLEEP и SLEEP): иконка сна стоит на месте
 * большого числа, а сама температура — под ней (LINE_x_SLEEP_TEMP).
 * Уставка (ВРЕМЕННОЕ отладочное поле) опущена на SCREEN_TARGET_SHIFT_Y вниз, в
 * строку пресетов: по X она между пресетами (центр половины 79/240 против
 * пресетов слева 10.., по оси 160 и справа до 310), по Y низ шрифта 18
 * совпадает с низом пресетов (шрифт 24 на SCREEN_PRESETS_Y).
 * Блок CURRENT для центрирования (SCREEN_TEMP_BLOCK_HEIGHT) НЕ меняется —
 * положение полосы CURRENT (SCREEN_CURRENT_Y) остаётся прежним. */
#define SCREEN_TARGET_SHIFT_Y     (50U)
#define SCREEN_SLEEP_TEMP_HEIGHT  (32U) /* высота AntiquaB_32_uni */
#define SCREEN_SLEEP_TEMP_Y ((uint16_t)(SCREEN_CURRENT_Y + SCREEN_CURRENT_HEIGHT + SCREEN_TEMP_GAP))
#define SCREEN_TARGET_Y     ((uint16_t)(SCREEN_SLEEP_TEMP_Y + SCREEN_TARGET_SHIFT_Y))
_Static_assert(SCREEN_SLEEP_TEMP_Y + SCREEN_SLEEP_TEMP_HEIGHT <= SCREEN_DIVIDER_Y1,
               "sleep temperature must end above the bottom of the divider/presets gap");
_Static_assert(SCREEN_TARGET_Y + SCREEN_TARGET_HEIGHT <= SCREEN_HEIGHT,
               "target field must fit on the screen");

/* Реальная высота рамки шрифта Comic_60_dig (Comic_60_dig.height), НЕ равна
 * номинальным 67px полосы CURRENT (SCREEN_CURRENT_HEIGHT): цифры занимают в
 * рамке строки ~1..47 из 49. Если рисовать число от верха полосы, оно
 * оказывается на (67-49)/2 = 9px выше центра полосы, по которому
 * центрируются иконки (крестик, смайлики сна) и "ВЫКЛ". Поэтому число
 * ставится так, чтобы рамка шрифта стояла по центру полосы. Значение обязано
 * совпадать с Comic_60_dig.height — Screen_Init() проверяет это assert()-ом. */
#define SCREEN_CURRENT_FONT_HEIGHT (49U)
#define SCREEN_CURRENT_TEXT_Y ((uint16_t)(SCREEN_CURRENT_Y + (SCREEN_CURRENT_HEIGHT - SCREEN_CURRENT_FONT_HEIGHT) / 2U))

/* Аварийное сообщение всегда в 2 строки (2 слова в одну не помещаются) —
 * FAULT_MSG стоит на позиции SCREEN_CURRENT_Y,
 * FAULT_MSG2 — сразу под ней. SCREEN_TITLE_HEIGHT — высота шрифта
 * AntiquaB_18_uni (тем же шрифтом выводятся обе строки), небольшой зазор
 * между ними. До SCREEN_TARGET_Y ещё много места (CURRENT-блок посчитан
 * под 67px строку Comic_60_dig, а тут всего 2×18px) — не пересекается. */
#define SCREEN_FAULT_MSG2_GAP (2U)
#define SCREEN_FAULT_MSG2_Y ((uint16_t)(SCREEN_CURRENT_Y + SCREEN_TITLE_HEIGHT + SCREEN_FAULT_MSG2_GAP))

/* "ВЫКЛ" (CHANNEL_CONTENT_DISABLED) — одна строка AntiquaB_32_uni,
 * вертикально по центру той же 67px CURRENT-полосы (реальная высота глифов
 * шрифта может не совпадать с номиналом 32px из его имени). */
#define SCREEN_DISABLED_HEIGHT (32U)
#define SCREEN_DISABLED_MSG_Y ((uint16_t)(SCREEN_CURRENT_Y + (SCREEN_CURRENT_HEIGHT - SCREEN_DISABLED_HEIGHT) / 2U))

/* Крестик "инструмент не подключен" (CHANNEL_CONTENT_IDLE, см.
 * ScreenIcons_DrawIdleCross()) — растр, вычисляется на лету (не хранится константой,
 * в отличие от SLEEP_ICON_BITMAP — тут это просто тест "около диагонали",
 * ручками эту сетку не набирали). Квадрат, вписанный в CURRENT-полосу
 * (67px), с запасом по бокам для обеих половин экрана (~79px). */
#define SCREEN_IDLE_CROSS_SIZE      (48U)
#define SCREEN_IDLE_CROSS_THICKNESS (4U)  /* полутолщина луча, см. ScreenIcons_DrawIdleCross() */
#define SCREEN_IDLE_CROSS_Y ((uint16_t)(SCREEN_CURRENT_Y + (SCREEN_CURRENT_HEIGHT - SCREEN_IDLE_CROSS_SIZE) / 2U))
#define COLOR_IDLE_CROSS DISPLAY_RGB565(120, 120, 120) /* пониженная контрастность — тусклее обычного текста */

/* Иконка SLEEP (спящий смайлик, растр SleepIcon_Bitmap — см. sleep_icon.h)
 * в CHANNEL_CONTENT_ASLEEP. Тот же принцип
 * центрирования по Y, что и у крестика IDLE (вписана в 67px CURRENT-полосу,
 * своя высота из sleep_icon.h вместо SCREEN_IDLE_CROSS_SIZE); по X —
 * центрируется на center_x своей половины экрана, как и крестик. */
#define SCREEN_ASLEEP_ICON_Y ((uint16_t)(SCREEN_CURRENT_Y + (SCREEN_CURRENT_HEIGHT - SLEEP_ICON_BITMAP_H) / 2U))

/* Иконка PRESLEEP (зевающий смайлик, PreSleepIcon_Bitmap 58x66 — см.
 * presleep_icon.h) на месте большого числа. Той же высоты, что и иконка
 * SLEEP (66px), и по Y центрируется по CURRENT-полосе по той же формуле. */
#define SCREEN_PRESLEEP_ICON_Y ((uint16_t)(SCREEN_CURRENT_Y + (SCREEN_CURRENT_HEIGHT - PRESLEEP_ICON_BITMAP_H) / 2U))
_Static_assert(SCREEN_PRESLEEP_ICON_Y >= SCREEN_TEMP_BAND_TOP,
               "presleep icon must not overlap the title strip");
_Static_assert(SCREEN_PRESLEEP_ICON_Y + PRESLEEP_ICON_BITMAP_H <= SCREEN_SLEEP_TEMP_Y,
               "presleep icon must not overlap the sleep-phase temperature");

#define SCREEN_INFO_X (10U)
#define SCREEN_INFO_Y (6U)
/* Инфозона разбита на три поля по x: сообщение EEPROM по центру,
 * иконка+таймер сна паяльника — у ПРАВОГО края своей (левой) половины,
 * т.е. перед разделителем; иконка+таймер сна отсоса — у правого края
 * своей (правой) половины, т.е. у правого края экрана. Таймер всегда
 * выравнивается по правому краю (TextField_PrintfRightAligned), иконка —
 * ПЕРЕСЧИТЫВАЕТСЯ на каждое обновление по ФАКТИЧЕСКОЙ ширине текущего
 * текста таймера (см. update_sleep_status()), а не по одной статичной
 * позиции под "худший случай" (иначе широкий текст, например "Предсон", перекрыл бы область иконки и стёр её).
 * Пересчёт по факту на каждый вызов исключает наложение — зазор
 * SLEEP_ICON_GAP_X между иконкой и текстом всегда одинаковый, независимо
 * от длины текста. Работает независимо для каждого инструмента (своя
 * половина экрана, свои координаты — см. вызовы update_sleep_status()).
 * Сообщение EEPROM выводится от самого левого края (SCREEN_INFO_X), а не
 * от центра — иначе оно наезжало бы на таймер паяльника (см.
 * TextField_ConfigureLine(LINE_INFO, ...) ниже). */
#define SCREEN_INFO_SLEEP_SOLDER_TEXT_RIGHT_EDGE_X   (SCREEN_DIVIDER_X0 - 6U)
#define SCREEN_INFO_SLEEP_DESOLDER_TEXT_RIGHT_EDGE_X (SCREEN_WIDTH - 10U)

/* ---- Иконка "циферблат" перед таймером сна (белая; x пересчитывается
 * динамически по факту текста в update_sleep_status(), см. выше — y и
 * размер статичны) ---- */
#define SLEEP_ICON_W     (14U)
#define SLEEP_ICON_H     (14U)
#define SLEEP_ICON_GAP_X (4U)  /* зазор между иконкой и текстом таймера */
#define SLEEP_ICON_Y     (SCREEN_INFO_Y + 2U) /* вертикально примерно по центру строки таймера (шрифт 18) */
#define COLOR_SLEEP_ICON DISPLAY_RGB565(255, 255, 255)

/* ---- Экран сервисного меню (заменяет собой весь главный экран целиком,
 * пока InputFSM_GetScreenMode() == SCREEN_MODE_SERVICE) ---- */
#define SCREEN_MENU_TITLE_X   (20U)
#define SCREEN_MENU_TITLE_Y   (14U)
#define SCREEN_MENU_ITEM_X    (20U)
#define SCREEN_MENU_ITEM_Y0   (40U)
#define SCREEN_MENU_ITEM_STEP (25U)
#define SCREEN_MENU_ITEM_ROWS (8U) /* максимум пунктов — уровень User (8: с "Заставкой"); Expert — 7 */
_Static_assert(SCREEN_MENU_ITEM_Y0 + (SCREEN_MENU_ITEM_ROWS - 1U) * SCREEN_MENU_ITEM_STEP + 18U <= SCREEN_HEIGHT,
               "последняя строка меню (шрифт 18) должна помещаться на экране");

/* ---- Цвета ---- */
#define COLOR_BG               DISPLAY_RGB565(0, 0, 0)
#define COLOR_ACTIVE_CURRENT   DISPLAY_RGB565(255, 255, 255)
#define COLOR_INACTIVE_CURRENT DISPLAY_RGB565(90, 90, 90)
#define COLOR_ACTIVE_TARGET    DISPLAY_RGB565(180, 180, 180)
#define COLOR_INACTIVE_TARGET  DISPLAY_RGB565(60, 60, 60)
/* Текущая температура в фазах сна (LINE_x_SLEEP_TEMP) — серая, тусклее
 * обычного белого числа: канал не работает, число вторично к иконке.
 * Активный/неактивный канал различаются яркостью, как и везде. */
#define COLOR_ACTIVE_SLEEP_TEMP   DISPLAY_RGB565(150, 150, 150)
#define COLOR_INACTIVE_SLEEP_TEMP DISPLAY_RGB565(60, 60, 60)
#define COLOR_ACTIVE_PRESETS   DISPLAY_RGB565(255, 210, 0)
#define COLOR_ACTIVE_TITLE     DISPLAY_RGB565(255, 255, 255)
#define COLOR_INACTIVE_TITLE   DISPLAY_RGB565(90, 90, 90)
#define COLOR_FAULT            DISPLAY_RGB565(255, 40, 40) /* заголовок/текущая температура при неисправности инструмента */
#define COLOR_DIVIDER          DISPLAY_RGB565(100, 100, 100)
#define COLOR_INFO             DISPLAY_RGB565(255, 255, 255)

#define COLOR_SLEEP_AWAKE    COLOR_INFO                       /* белый — обычный обратный отсчёт до PRESLEEP */
#define COLOR_SLEEP_PRESLEEP DISPLAY_RGB565(255, 210, 0)      /* жёлтый */
#define COLOR_SLEEP_SLEEP    DISPLAY_RGB565(255, 40, 40)      /* красный */

#define COLOR_MENU_TITLE    DISPLAY_RGB565(255, 255, 255)
#define COLOR_MENU_NORMAL   DISPLAY_RGB565(180, 180, 180)
#define COLOR_MENU_CURSOR   DISPLAY_RGB565(255, 210, 0)  /* выбранный пункт, не редактируется */
#define COLOR_MENU_EDITING  DISPLAY_RGB565(255, 80, 80)  /* выбранный пункт, редактируется прямо сейчас */

#endif /* SCREEN_LAYOUT_H */
