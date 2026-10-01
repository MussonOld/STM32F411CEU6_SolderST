/**
 * @file    screen_menu.h
 * @brief   Экран сервисного меню (App/Screen, внутренний модуль): заменяет собой
 *          весь главный экран, пока InputFSM_GetScreenMode() == SCREEN_MODE_SERVICE.
 *          Содержимое и навигация — menu.h.
 */
#ifndef SCREEN_MENU_H
#define SCREEN_MENU_H

/** @brief Сконфигурировать строки TextField заголовка и пунктов меню. Вызывать из Screen_Init(). */
void ScreenMenu_Init(void);

/** @brief Обновить строки меню по текущему состоянию Menu_* (вызывать каждый Screen_Update() в режиме меню). */
void ScreenMenu_Render(void);

#endif /* SCREEN_MENU_H */
