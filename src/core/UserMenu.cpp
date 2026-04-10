/**
 * Visual Intelligence Bypass (VIB) - User Menu Implementation
 */

#include "UserMenu.h"
#include "../utils/Logger.h"
#include <iostream>
#include <cstdio>
#include <conio.h>
#include <Windows.h>
#include <iomanip>

namespace VIB {

UserMenu::UserMenu() {
    // Set console to UTF-8 for Spanish characters
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
}

void UserMenu::ClearScreen() {
    // Use Windows console API for reliable clearing
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    DWORD count;
    DWORD cellCount;
    COORD homeCoords = {0, 0};

    if (GetConsoleScreenBufferInfo(hConsole, &csbi)) {
        cellCount = csbi.dwSize.X * csbi.dwSize.Y;
        FillConsoleOutputCharacter(hConsole, ' ', cellCount, homeCoords, &count);
        FillConsoleOutputAttribute(hConsole, csbi.wAttributes, cellCount, homeCoords, &count);
        SetConsoleCursorPosition(hConsole, homeCoords);
    } else {
        // Fallback: print newlines
        std::cout << std::string(50, '\n');
    }
}

void UserMenu::PrintHeader() {
    std::cout << "\n";
    std::cout << "  ╔════════════════════════════════════════════════════════╗\n";
    std::cout << "  ║                                                        ║\n";
    std::cout << "  ║       VISUAL INTELLIGENCE BYPASS (VIB) v2.0            ║\n";
    std::cout << "  ║                  MENU PRINCIPAL                        ║\n";
    std::cout << "  ║                                                        ║\n";
    std::cout << "  ╠════════════════════════════════════════════════════════╣\n";
}

void UserMenu::PrintMenuOptions() {
    std::cout << "  ║                                                        ║\n";
    std::cout << "  ║   [1] TEST DE FUNCIONAMIENTO (3 seg)                   ║\n";
    std::cout << "  ║                                                        ║\n";
    std::cout << "  ║   [2] INICIAR (RUNNING MODE)                           ║\n";
    std::cout << "  ║                                                        ║\n";
    std::cout << "  ║   [3] CERRAR SISTEMA                                   ║\n";
    std::cout << "  ║                                                        ║\n";
    std::cout << "  ║   [4] TOOLS & CONFIGURATION                            ║\n";
    std::cout << "  ║                                                        ║\n";
    std::cout << "  ╚════════════════════════════════════════════════════════╝\n";
    std::cout << "\n";
    std::cout << "  Seleccione una opcion [1-4]: ";
}

int UserMenu::GetUserInput() {
    int input = 0;
    std::cin >> input;
    
    // Clear the input buffer
    std::cin.clear();
    std::cin.ignore(10000, '\n');
    
    return input;
}

MenuOption UserMenu::ShowMainMenu() {
    ClearScreen();
    PrintHeader();
    PrintMenuOptions();
    
    int selection = GetUserInput();
    
    switch (selection) {
        case 1:
            return MenuOption::TEST_FUNCIONAMIENTO;
        case 2:
            return MenuOption::INICIAR;
        case 3:
            return MenuOption::CERRAR;
        case 4:
            return MenuOption::TOOLS;
        default:
            return MenuOption::INVALID;
    }
}

void UserMenu::ShowSuccessStatus(const RunResult& result) {
    ClearScreen();
    std::cout << "\n";
    std::cout << "  ╔════════════════════════════════════════════════════════╗\n";
    std::cout << "  ║                    RESULTADO                           ║\n";
    std::cout << "  ╠════════════════════════════════════════════════════════╣\n";
    std::cout << "  ║                                                        ║\n";
    std::cout << "  ║   [OK] ESTADO: OK                                      ║\n";
    std::cout << "  ║                                                        ║\n";
    
    // Format elapsed time with fixed precision
    std::ostringstream timeStr;
    timeStr << std::fixed << std::setprecision(2) << result.elapsedSeconds;
    std::string formattedTime = timeStr.str();
    
    std::cout << "  ║   Tiempo transcurrido: " << formattedTime << " segundos";
    // Calculate padding based on actual formatted string length
    size_t contentLen = 24 + formattedTime.length() + 9; // "Tiempo transcurrido: " + time + " segundos"
    size_t totalWidth = 56; // Width inside the box
    for (size_t i = contentLen; i < totalWidth; ++i) std::cout << " ";
    std::cout << "║\n";
    
    std::cout << "  ║                                                        ║\n";
    std::cout << "  ╚════════════════════════════════════════════════════════╝\n";
    std::cout << "\n";
    std::cout << "  Presione cualquier tecla para volver al menu...";
    
    Logger::Info("Sistema ejecutado correctamente - " + 
                 std::to_string(result.elapsedSeconds) + " segundos");
}

void UserMenu::ShowErrorStatus(const std::string& errorMessage) {
    ClearScreen();
    std::cout << "\n";
    std::cout << "  ╔════════════════════════════════════════════════════════╗\n";
    std::cout << "  ║                    RESULTADO                           ║\n";
    std::cout << "  ╠════════════════════════════════════════════════════════╣\n";
    std::cout << "  ║                                                        ║\n";
    std::cout << "  ║   [X] ESTADO: ERROR                                    ║\n";
    std::cout << "  ║                                                        ║\n";
    std::cout << "  ╠════════════════════════════════════════════════════════╣\n";
    std::cout << "  ║                                                        ║\n";
    
    // Word-wrap error message to fit in box (max ~50 chars per line)
    const size_t maxLineLen = 50;
    std::string remaining = errorMessage;
    while (!remaining.empty()) {
        std::string line = remaining.substr(0, maxLineLen);
        if (remaining.length() > maxLineLen) {
            // Try to break at space
            size_t lastSpace = line.rfind(' ');
            if (lastSpace != std::string::npos && lastSpace > 20) {
                line = remaining.substr(0, lastSpace);
                remaining = remaining.substr(lastSpace + 1);
            } else {
                remaining = remaining.substr(maxLineLen);
            }
        } else {
            remaining.clear();
        }
        
        std::cout << "  ║   " << line;
        // Pad to align
        for (size_t i = line.length(); i < 52; ++i) std::cout << " ";
        std::cout << "║\n";
    }
    
    std::cout << "  ║                                                        ║\n";
    std::cout << "  ╚════════════════════════════════════════════════════════╝\n";
    std::cout << "\n";
    std::cout << "  Presione cualquier tecla para volver al menu...";
    
    Logger::Error("Sistema reporto error: " + errorMessage);
}

void UserMenu::ShowToolsComingSoon() {
    ClearScreen();
    std::cout << "\n";
    std::cout << "  ╔════════════════════════════════════════════════════════╗\n";
    std::cout << "  ║              TOOLS & CONFIGURATION                     ║\n";
    std::cout << "  ╠════════════════════════════════════════════════════════╣\n";
    std::cout << "  ║                                                        ║\n";
    std::cout << "  ║   Esta funcionalidad estara disponible                 ║\n";
    std::cout << "  ║   en una proxima version.                              ║\n";
    std::cout << "  ║                                                        ║\n";
    std::cout << "  ║   PROXIMAMENTE                                         ║\n";
    std::cout << "  ║                                                        ║\n";
    std::cout << "  ╚════════════════════════════════════════════════════════╝\n";
    std::cout << "\n";
    std::cout << "  Presione cualquier tecla para volver al menu...";
    
    Logger::Info("Usuario accedio a Tools - proximamente disponible");
}

void UserMenu::ShowExitMessage() {
    ClearScreen();
    std::cout << "\n";
    std::cout << "  ╔════════════════════════════════════════════════════════╗\n";
    std::cout << "  ║                                                        ║\n";
    std::cout << "  ║   Cerrando Visual Intelligence Bypass...               ║\n";
    std::cout << "  ║                                                        ║\n";
    std::cout << "  ║   Gracias por usar VIB v2.0                            ║\n";
    std::cout << "  ║                                                        ║\n";
    std::cout << "  ╚════════════════════════════════════════════════════════╝\n";
    std::cout << "\n";
    
    Logger::Info("Usuario cerro el sistema desde menu");
}

void UserMenu::WaitForKeyPress() {
    // Use _getch() for Windows to wait for any key
    _getch();
}

} // namespace VIB
