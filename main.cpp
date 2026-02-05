#include <iostream>
#include <fstream>
#include <windows.h>
#include <string>
#include <direct.h>     // for _mkdir
#include <errno.h>      // for errno and EEXIST

typedef void(__stdcall* lpOut32)(short, short);
typedef short(__stdcall* lpInp32)(short);

#define EC_SC   0x66
#define EC_DATA 0x62
#define EC_WRITE_CMD 0x81

// Use plain strings for paths (compatible with old compilers)
const std::string STATE_DIR  = std::string(getenv("APPDATA")) + "\\LedToggle";
const std::string STATE_FILE = STATE_DIR + "\\state.bin";
const std::string LOG_FILE   = STATE_DIR + "\\error.log";

bool WaitEC(lpInp32 gInp32, int retries = 3) {
    while (retries-- > 0) {
        int timeout = 300;
        while ((gInp32(EC_SC) & 0x02) && timeout > 0) {
            Sleep(1);
            --timeout;
        }
        if (timeout > 0) return true;
        Sleep(10);  // brief backoff
    }
    return false;
}

bool WriteReg(lpOut32 gOut32, lpInp32 gInp32, BYTE addr, BYTE val) {
    if (!WaitEC(gInp32)) return false;
    gOut32(EC_SC, EC_WRITE_CMD);
    if (!WaitEC(gInp32)) return false;
    gOut32(EC_DATA, addr);
    if (!WaitEC(gInp32)) return false;
    gOut32(EC_DATA, val);
    return true;
}

bool IsElevated() {
    BOOL isElevated = FALSE;
    HANDLE token = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        TOKEN_ELEVATION elevation;
        DWORD size = sizeof(TOKEN_ELEVATION);
        if (GetTokenInformation(token, TokenElevation, &elevation, size, &size)) {
            isElevated = elevation.TokenIsElevated;
        }
        CloseHandle(token);
    }
    return isElevated != FALSE;
}

void LogError(const std::string& msg) {
    std::ofstream log(LOG_FILE.c_str(), std::ios::app);
    if (log.is_open()) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        log << "[" 
            << st.wYear << "-" << st.wMonth << "-" << st.wDay << " "
            << st.wHour << ":" << st.wMinute << ":" << st.wSecond 
            << "] " << msg << std::endl;
        log.close();
    }
}

int main() {
    if (!IsElevated()) {
        std::cerr << "This program must be run as Administrator.\n";
        return 1;
    }

    // Create directory if it doesn't exist
    int dirResult = _mkdir(STATE_DIR.c_str());
    if (dirResult == 0 || errno == EEXIST) {
        // Directory created or already exists
        SetFileAttributesA(STATE_DIR.c_str(), FILE_ATTRIBUTE_HIDDEN);
    } else {
        LogError("Failed to create state directory: " + STATE_DIR);
        std::cerr << "Cannot create state directory.\n";
        return 1;
    }

    // Simple mutex to prevent multiple instances
    HANDLE mutex = CreateMutexA(NULL, TRUE, "Global\\LedToggleSingleInstance");
    if (mutex == NULL || GetLastError() == ERROR_ALREADY_EXISTS) {
        std::cerr << "Another instance of this program is already running.\n";
        if (mutex) CloseHandle(mutex);
        return 1;
    }

    // Load InpOut DLL from the same directory as the executable
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    
    std::string exeDir = exePath;
    size_t lastSlash = exeDir.find_last_of("\\/");
    if (lastSlash != std::string::npos) {
        exeDir = exeDir.substr(0, lastSlash);
    }
    
    std::string dllPath = exeDir + "\\InpOutx64.dll";   // change to InpOut32.dll if using 32-bit version
    
    HINSTANCE hDll = LoadLibraryA(dllPath.c_str());
    if (!hDll) {
        LogError("Failed to load InpOut DLL from: " + dllPath);
        std::cerr << "Cannot load InpOut DLL. Make sure InpOutx64.dll is in the same folder as this .exe\n";
        ReleaseMutex(mutex);
        CloseHandle(mutex);
        return 1;
    }

    lpOut32 gOut32 = (lpOut32)GetProcAddress(hDll, "Out32");
    lpInp32 gInp32 = (lpInp32)GetProcAddress(hDll, "Inp32");
    if (!gOut32 || !gInp32) {
        LogError("Failed to get Out32 or Inp32 function from DLL");
        std::cerr << "Cannot find Out32/Inp32 functions in DLL.\n";
        FreeLibrary(hDll);
        ReleaseMutex(mutex);
        CloseHandle(mutex);
        return 1;
    }

    bool shouldTurnOff = false;  // default: lights are ON

    std::ifstream in(STATE_FILE.c_str(), std::ios::binary);
    if (in.is_open()) {
        in >> shouldTurnOff;
        in.close();
    }

    bool success = true;

    if (shouldTurnOff) {
        // Turn lights ON
        success &= WriteReg(gOut32, gInp32, 0x0C, 0x80);
        success &= WriteReg(gOut32, gInp32, 0xA0, 0x80);
        shouldTurnOff = false;
    } else {
        // Turn lights OFF
        success &= WriteReg(gOut32, gInp32, 0x0C, 0x00);
        success &= WriteReg(gOut32, gInp32, 0xA0, 0x00);
        shouldTurnOff = true;
    }

    if (!success) {
        LogError("EC communication failed (timeout or error)");
        std::cerr << "Warning: Failed to communicate with Embedded Controller.\n";
    }

    std::ofstream out(STATE_FILE.c_str(), std::ios::binary | std::ios::trunc);
    if (out.is_open()) {
        out << shouldTurnOff;
        out.close();
    } else {
        LogError("Failed to write state file: " + STATE_FILE);
        std::cerr << "Cannot save LED state.\n";
    }

    FreeLibrary(hDll);
    ReleaseMutex(mutex);
    CloseHandle(mutex);

    return success ? 0 : 2;
}
