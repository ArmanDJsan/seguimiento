/**
 * Visual Intelligence Bypass (VIB) - User Menu System
 * 
 * Provides an interactive console menu for system control
 * Menu language: Spanish
 */

#ifndef VIB_USER_MENU_H
#define VIB_USER_MENU_H

#include <string>
#include <functional>

namespace VIB {

/**
 * Menu option enumeration
 */
enum class MenuOption {
    TEST_FUNCIONAMIENTO = 1,  // Run system for 3 seconds
    INICIAR = 2,              // Run system until ESC
    CERRAR = 3,               // Exit program
    TOOLS = 4,                // Tools & Configuration
    INVALID = 0               // Invalid selection
};

/**
 * Result of a system run operation
 */
struct RunResult {
    bool success;
    std::string errorMessage;
    int framesProcessed;
    double elapsedSeconds;
};

/**
 * User menu class for VIB system
 */
class UserMenu {
public:
    UserMenu();
    ~UserMenu() = default;

    /**
     * Display the main menu and get user selection
     * @return Selected menu option
     */
    MenuOption ShowMainMenu();

    /**
     * Display success status after test/run
     * @param result Run result with metrics
     */
    void ShowSuccessStatus(const RunResult& result);

    /**
     * Display error status
     * @param errorMessage Error description
     */
    void ShowErrorStatus(const std::string& errorMessage);

    /**
     * Display "coming soon" message for Tools option
     */
    void ShowToolsComingSoon();

    /**
     * Clear the console screen
     */
    void ClearScreen();

    /**
     * Display exit message
     */
    void ShowExitMessage();

    /**
     * Wait for user to press any key
     */
    void WaitForKeyPress();

private:
    void PrintHeader();
    void PrintMenuOptions();
    int GetUserInput();
};

} // namespace VIB

#endif // VIB_USER_MENU_H
