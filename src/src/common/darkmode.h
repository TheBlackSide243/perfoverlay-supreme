#pragma once
#include <windows.h>

namespace po {

// Menu contestuali, tooltip e scrollbar in tema scuro per tutto il processo.
// Usa le API non documentate di uxtheme (come Notepad++ e altri): se mancano non succede nulla.
void EnableDarkModeForApp();

// Barra del titolo scura (Windows 10 1809+) e, su Windows 11, dello stesso colore dello sfondo.
void ApplyDarkTitleBar(HWND hwnd, COLORREF caption);

// Tema scuro per un singolo controllo comune (combobox, listbox, scrollbar).
void ApplyDarkControlTheme(HWND ctl, const wchar_t* theme);

}  // namespace po
