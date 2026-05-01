/**
 * UserMenu.h
 * 
 * Menú de usuario interactivo para VIB
 * Permite seleccionar entre diferentes modos de operación:
 * - Test de Estrés / Diagnóstico
 * - Modo Running (operación normal)
 * - Cerrar Sistema
 * - Tools & Configuration (stub)
 */

#pragma once

#include <string>

/**
 * Opciones del menú principal
 */
enum class MenuOption {
    STRESS_TEST_DIAGNOSTIC = 1,
    RUNNING_MODE = 2,
    CLOSE_SYSTEM = 3,
    TOOLS_CONFIG = 4,
    RADAR_TEST_MODE = 5,        // Test de inferencia con Radar 1 (cámara 13)
    SPHERE_VERIFIER_TEST = 6,   // Test usando SphereVerifier (reemplazo de opción 5)
    VMIX_UTC_CHOREOGRAPHY = 7,  // Ejecutar coreografía vMix UTC (solo vMix TCP)
    INVALID = -1
};

/**
 * UserMenu - Interfaz de menú de consola
 */
class UserMenu {
public:
    UserMenu() = default;
    ~UserMenu() = default;
    
    /**
     * Mostrar el menú principal y obtener selección del usuario
     * @return Opción seleccionada por el usuario
     */
    MenuOption ShowMainMenu();
    
    /**
     * Mostrar mensaje de stub para Tools & Configuration
     */
    void ShowToolsConfigStub();
    
    /**
     * Limpiar la pantalla de la consola
     */
    void ClearScreen();
    
    /**
     * Mostrar el banner de VIB
     */
    void DisplayBanner();
    
    /**
     * Esperar a que el usuario presione ENTER
     */
    void WaitForEnter();
    
    /**
     * Mostrar mensaje de despedida
     */
    void ShowGoodbye();

private:
    /**
     * Imprimir las opciones del menú
     */
    void PrintMenuOptions();
    
    /**
     * Obtener la selección del usuario (entrada numérica)
     * @return Número seleccionado (1-4) o -1 si inválido
     */
    int GetUserChoice();
    
    /**
     * Convertir número a MenuOption
     */
    MenuOption IntToMenuOption(int choice);
};
