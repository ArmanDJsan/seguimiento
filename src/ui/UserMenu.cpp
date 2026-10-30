/**
 * UserMenu.cpp
 * 
 * Implementación del menú de usuario interactivo
 */

#include "UserMenu.h"
#include <iostream>
#include <string>
#include <limits>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

MenuOption UserMenu::ShowMainMenu() {
    ClearScreen();
    DisplayBanner();
    PrintMenuOptions();
    
    int choice = GetUserChoice();
    return IntToMenuOption(choice);
}

void UserMenu::ClearScreen() {
    // Usar comando del sistema para limpiar consola en Windows
    system("cls");
}

void UserMenu::DisplayBanner() {
    std::cout << "\n";
    std::cout << "  ======================================================================\n";
    std::cout << "  ||                                                                  ||\n";
    std::cout << "  ||     VIB - Visual Intelligence Bypass v2.0                        ||\n";
    std::cout << "  ||     Sistema de Seguimiento de Bolas - BIGBOX                     ||\n";
    std::cout << "  ||                                                                  ||\n";
    std::cout << "  ======================================================================\n";
    std::cout << "\n";
}

void UserMenu::PrintMenuOptions() {
    std::cout << "  +--------------------------------------------------------------------+\n";
    std::cout << "  |                      MENU PRINCIPAL                               |\n";
    std::cout << "  +--------------------------------------------------------------------+\n";
    std::cout << "  |                                                                    |\n";
    std::cout << "  |   [1] REALIZAR TEST DE ESTRES / DIAGNOSTICO                       |\n";
    std::cout << "  |       Ejecuta todos los tests con frames reales durante 1 segundo |\n";
    std::cout << "  |       Si todo OK, se detiene automaticamente                      |\n";
    std::cout << "  |                                                                    |\n";
    std::cout << "  |   [2] INICIAR (RUNNING MODE)                                      |\n";
    std::cout << "  |       Inicia el sistema en modo normal de operacion               |\n";
    std::cout << "  |       Tests + Configuracion + Captura continua                    |\n";
    std::cout << "  |                                                                    |\n";
    std::cout << "  |   [3] CERRAR SISTEMA                                              |\n";
    std::cout << "  |       Cierra la aplicacion                                        |\n";
    std::cout << "  |                                                                    |\n";
    std::cout << "  |   [4] TOOLS & CONFIGURATION                                       |\n";
    std::cout << "  |       (Proximamente)                                              |\n";
    std::cout << "  |                                                                    |\n";
    std::cout << "  |   [5] TEST RADAR 1 (CAMARA 13) - VERIFICAR ESFERAS [LEGACY]       |\n";
    std::cout << "  |       Captura solo del Radar 1, detecta esferas con inferencia    |\n";
    std::cout << "  |       Muestra conteo de esferas detectadas en tiempo real         |\n";
    std::cout << "  |                                                                    |\n";
    std::cout << "  |   [6] SPHERE VERIFIER TEST (RECOMENDADO)                          |\n";
    std::cout << "  |       Test simplificado usando SphereVerifier                     |\n";
    std::cout << "  |       - Seleccion de camara (1-16)                                |\n";
    std::cout << "  |       - Verificacion de presencia de esferas                      |\n";
    std::cout << "  |       - Captura de posiciones                                     |\n";
    std::cout << "  |                                                                    |\n";
    std::cout << "  |   [7] COREOGRAFIA VMIX UTC (SOLO VMIX)                             |\n";
    std::cout << "  |       Ejecuta la coreografia de replay convertida desde UTC        |\n";
    std::cout << "  |       Solo requiere conexion vMix TCP                              |\n";
    std::cout << "  |                                                                    |\n";
    std::cout << "  |   [8] MODO PISTA AUTOMATIZADA                                      |\n";
    std::cout << "  |       Control automatizado de pista fisica (1 camara PTZ)         |\n";
    std::cout << "  |       - Verifica esferas en salida                                 |\n";
    std::cout << "  |       - Controla compuertas y ascensor (ESP32)                    |\n";
    std::cout << "  |                                                                    |\n";
    std::cout << "  |   [9] LIVE PREVIEW - PRUEBA DE DETECCION                          |\n";
    std::cout << "  |       Muestra video en vivo con detecciones YOLO en tiempo real   |\n";
    std::cout << "  |       - NDI: VIB_Pista_Auto (video)                               |\n";
    std::cout << "  |       - Consola: detecciones en vivo                              |\n";
    std::cout << "  |       - ESC para salir                                            |\n";
    std::cout << "  |                                                                    |\n";
    std::cout << "  |   [10] CAPTURA PARA ENTRENAMIENTO                                 |\n";
    std::cout << "  |       Video NDI continuo + guarda frames para etiquetar           |\n";
    std::cout << "  |       - NDI: VIB_Training (video)                                 |\n";
    std::cout << "  |       - ESPACIO: guardar frame actual                             |\n";
    std::cout << "  |       - ESC: salir                                                |\n";
    std::cout << "  |                                                                    |\n";
    std::cout << "  |   [11] TEST ORDEN DE LLEGADA                                      |\n";
    std::cout << "  |       Deteccion continua de la camara de meta (motor dedicado)    |\n";
    std::cout << "  |       - Boxes con color/clase y orden de izquierda a derecha      |\n";
    std::cout << "  |       - NDI: VIB_Llegada_Raw / VIB_Llegada_Boxes                  |\n";
    std::cout << "  |       - ESC para salir                                            |\n";
    std::cout << "  |                                                                    |\n";
    std::cout << "  +--------------------------------------------------------------------+\n";
    std::cout << "\n";
}

int UserMenu::GetUserChoice() {
    std::cout << "  Seleccione una opcion (1-11): ";
    
    int choice = -1;
    std::cin >> choice;
    
    // Limpiar el buffer de entrada
    if (std::cin.fail()) {
        std::cin.clear();
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        return -1;
    }
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    
    return choice;
}

MenuOption UserMenu::IntToMenuOption(int choice) {
    switch (choice) {
        case 1: return MenuOption::STRESS_TEST_DIAGNOSTIC;
        case 2: return MenuOption::RUNNING_MODE;
        case 3: return MenuOption::CLOSE_SYSTEM;
        case 4: return MenuOption::TOOLS_CONFIG;
        case 5: return MenuOption::RADAR_TEST_MODE;
        case 6: return MenuOption::SPHERE_VERIFIER_TEST;
        case 7: return MenuOption::VMIX_UTC_CHOREOGRAPHY;
        case 8: return MenuOption::AUTOMATED_TRACK;
        case 9: return MenuOption::LIVE_PREVIEW;
        case 10: return MenuOption::TRAINING_CAPTURE;
        case 11: return MenuOption::ARRIVAL_TEST;
        default: return MenuOption::INVALID;
    }
}

void UserMenu::ShowToolsConfigStub() {
    ClearScreen();
    DisplayBanner();
    
    std::cout << "  +--------------------------------------------------------------------+\n";
    std::cout << "  |                  TOOLS & CONFIGURATION                            |\n";
    std::cout << "  +--------------------------------------------------------------------+\n";
    std::cout << "  |                                                                    |\n";
    std::cout << "  |   Esta funcionalidad estara disponible proximamente.              |\n";
    std::cout << "  |                                                                    |\n";
    std::cout << "  |   Funciones planificadas:                                         |\n";
    std::cout << "  |   - Calibracion de camaras                                        |\n";
    std::cout << "  |   - Configuracion de VideoHub                                     |\n";
    std::cout << "  |   - Ajustes de inferencia                                         |\n";
    std::cout << "  |   - Diagnosticos avanzados                                        |\n";
    std::cout << "  |                                                                    |\n";
    std::cout << "  +--------------------------------------------------------------------+\n";
    std::cout << "\n";
    
    WaitForEnter();
}

void UserMenu::WaitForEnter() {
    std::cout << "  Presione ENTER para continuar...";
    std::cin.get();
}

void UserMenu::ShowGoodbye() {
    ClearScreen();
    DisplayBanner();
    
    std::cout << "  +--------------------------------------------------------------------+\n";
    std::cout << "  |                                                                    |\n";
    std::cout << "  |                  Sistema VIB cerrado correctamente                 |\n";
    std::cout << "  |                                                                    |\n";
    std::cout << "  |                         Hasta pronto!                              |\n";
    std::cout << "  |                                                                    |\n";
    std::cout << "  +--------------------------------------------------------------------+\n";
    std::cout << "\n";
}
