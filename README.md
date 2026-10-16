# FPGA Programmer for GW1NR / Tang Nano 9k Embedded Flash or SRAM

Use at your own risk! This programmer is not well-tested; it has been developed and verified specifically for Gowin GW1NR-9C (e.g., Tang Nano 9K) with **embedded Flash (`embFlash`)** and **SRAM**. This programmer does **not** support external Flash chips today (which require SPI flash controller commands); therefore, do not use it on boards with external Flash.

This programmer works with the onboard BL702 USB-JTAG bridge (which emulates an FTDI FT2232D) as well as standard FTDI MPSSE JTAG adapters.

It has the advantage that no custom drivers or zadig libusb replacements are required, as it communicates directly via the vendor `ftd2xx.dll` driver that is often already present on Windows PCs.

For a mature, general-purpose FPGA programmer supporting many FPGA families and cables, see [openFPGALoader](https://github.com/trabucayre/openFPGALoader).

---

## Command Line Usage

### Writing to Embedded Flash (e.g. Tang Nano 9K)

You can program embedded flash directly from the Gowin `.fs` bitstream file:

```powershell
.\build\Release\tang_programmer.exe flash .\mybitfile.fs --confirm-flash-write
```

To validate the file without touching hardware, use `--audit`:

```powershell
.\build\Release\tang_programmer.exe flash .\mybitfile.fs --audit
```

### Writing to SRAM

Direct SRAM writing from `.fs` is not currently supported; convert the bitstream to `.svf` first using Gowin Programmer or the provided `svf_gen.ps1` script which calls the Gowin Programmer for you (note: edit the variable `GowinBin` in the `svf_gen.ps1` file to suit your installation folder first).

```powershell
.\svf_gen.ps1 mybitfile.fs
.\build\Release\tang_programmer.exe sram .\svf_generated\sram.svf
```

---

## C / C++ Library API

`tang_programmer` is built both as a standalone CLI executable (`tang_programmer.exe`) and as a dynamic link library (`tang_programmer.dll` / `tang_programmer.lib`). The library exposes a pure C ABI defined in [`include/tang_programmer.h`](include/tang_programmer.h).

### Header & Linkage

```c
#include "tang_programmer.h"
```

Link against `tang_programmer.lib` or load dynamically at runtime using `LoadLibrary("tang_programmer.dll")` / `GetProcAddress`.

### Types & Enums

#### `tang_operation`
```c
typedef enum {
    TANG_SRAM  = 1,   /* Program FPGA SRAM (volatile, requires .svf) */
    TANG_FLASH = 2    /* Program embedded Flash (non-volatile, supports .fs and .svf) */
} tang_operation;
```

#### `tang_progress_fn`
```c
typedef void (*tang_progress_fn)(const char *json_line, void *user);
```
Callback function invoked during programming to report real-time progress. Each invocation passes a NUL-terminated JSON string (see [Progress Events](#progress-events-json-format)) and the caller-provided `user` context pointer.

---

### Functions

#### `tang_program`
Programs the target FPGA device.

```c
TANG_API int tang_program(const char *path,
                          tang_operation operation,
                          int device_index,
                          tang_progress_fn callback,
                          void *user,
                          char *error,
                          uint32_t error_capacity);
```

- **`path`**: Path to the input file (`.fs` bitstream or `.svf` file).
- **`operation`**: `TANG_FLASH` or `TANG_SRAM`.
- **`device_index`**: FTDI / BL702 device index (typically `0` when only one device is connected).
- **`callback`**: Optional progress callback (`NULL` if progress updates are not needed).
- **`user`**: User-defined context pointer passed through to `callback`.
- **`error`**: Caller-allocated character buffer to receive error descriptions on failure. Guaranteed to be NUL-terminated if `error_capacity > 0`.
- **`error_capacity`**: Size in bytes of the `error` buffer (recommended: at least 512 bytes).
- **Return Value**: `0` on verified success; nonzero on failure.

#### `tang_audit`
Performs offline syntax and structure validation of a bitstream or SVF file without connecting to or accessing any hardware.

```c
TANG_API int tang_audit(const char *path,
                        tang_operation operation,
                        char *error,
                        uint32_t error_capacity);
```

- **`path`**: Path to the file to inspect.
- **`operation`**: Target operation (`TANG_FLASH` or `TANG_SRAM`).
- **`error`**: Buffer to receive failure description if validation fails.
- **`error_capacity`**: Capacity of `error` buffer.
- **Return Value**: `0` if file is valid and recognized; nonzero if invalid.

---

### Progress Events (JSON Format)

The callback receives progress lines formatted as JSON objects:

```json
{"event":"progress","phase":"flash_program","completed":512,"total":1011,"percent":50.64,"message":""}
```

Common phases:
- `"validate"`: Validating bitstream syntax and file integrity.
- `"connect"`: Opening FTDI device and resetting JTAG TAP state.
- `"idcode"`: Querying and verifying device IDCODE (`0x1100481B` for GW1NR-9C).
- `"flash_erase"`: Performing bulk erase of embedded flash block.
- `"flash_setup"`: Pre-program STATUS check (`0x00039020`).
- `"flash_program"`: Streaming 256-byte flash pages (1,011 pages for GW1NR-9C).
- `"flash_status"`: Post-program reload and STATUS verification (`0x0003F020`).
- `"sram_transfer"`: Transferring configuration rows to SRAM.
- `"sram_verify"`: Verifying SRAM operational status.
- `"complete"`: Operation finished and verified.

---

### C++ Example

```cpp
#include "tang_programmer.h"
#include <iostream>

void on_progress(const char* json_line, void* user_data) {
    std::cout << json_line << std::endl;
}

int main() {
    char error_buf[1024] = {0};

    // 1. Audit bitstream file before accessing hardware
    int audit_res = tang_audit("double_blinky.fs", TANG_FLASH, error_buf, sizeof(error_buf));
    if (audit_res != 0) {
        std::cerr << "Audit failed: " << error_buf << std::endl;
        return 1;
    }

    // 2. Program embedded flash
    int prog_res = tang_program(
        "double_blinky.fs",   // bitstream path
        TANG_FLASH,           // operation
        0,                    // device index
        on_progress,          // progress callback
        nullptr,              // user context
        error_buf,            // error buffer
        sizeof(error_buf)     // error buffer size
    );

    if (prog_res != 0) {
        std::cerr << "Programming failed: " << error_buf << std::endl;
        return 1;
    }

    std::cout << "Programming succeeded!" << std::endl;
    return 0;
}
```

### Python Example (`ctypes`)

```python
import ctypes

# Load DLL
dll = ctypes.CDLL("./build/Release/tang_programmer.dll")

# Define callback type
PROGRESS_CALLBACK = ctypes.CFUNCTYPE(None, ctypes.c_char_p, ctypes.c_void_p)

@PROGRESS_CALLBACK
def progress_callback(json_line, user_data):
    print(json_line.decode("utf-8"))

dll.tang_program.argtypes = [
    ctypes.c_char_p,          # path
    ctypes.c_int,             # operation (1=SRAM, 2=FLASH)
    ctypes.c_int,             # device_index
    PROGRESS_CALLBACK,        # callback
    ctypes.c_void_p,          # user
    ctypes.c_char_p,          # error
    ctypes.c_uint32           # error_capacity
]
dll.tang_program.restype = ctypes.c_int

error = ctypes.create_string_buffer(1024)
res = dll.tang_program(b"double_blinky.fs", 2, 0, progress_callback, None, error, 1024)
if res == 0:
    print("Programming succeeded!")
else:
    print("Failed:", error.value.decode("utf-8"))
```

---

## Requirements & Prerequisites

### Build Requirements
- **Operating System:** Windows 10 or 11 (64-bit x64).
- **C++ Compiler:** A C++17 compliant compiler. Microsoft Visual C++ (MSVC) from Visual Studio 2019, 2022, or newer (with the *"Desktop development with C++"* workload) is recommended and automatically detected.
- **CMake:** Version 3.18 or higher.
- **PowerShell:** PowerShell 5.1 (bundled with Windows) or PowerShell Core 7+.

### Environment Variables
- **`PATH`**:
  - Must include `cmake.exe` (e.g., `C:\Program Files\CMake\bin` or Visual Studio's CMake component).
  - Must include `ftd2xx.dll` at runtime (typically automatically present in `C:\Windows\System32\` via Windows Update or the FTDI driver installer).
- **No manual SDK / compiler environment variables required**: When using `build.ps1` or CMake with the default Visual Studio generator, CMake automatically locates the MSVC compiler, linker, and Windows SDK without requiring you to run `vcvarsall.bat` or manually set `INCLUDE`, `LIB`, or `VCINSTALLDIR`.
  > [!NOTE]
  > If using a command-line generator like Ninja or NMake directly without Visual Studio solution files, run from the **x64 Native Tools Command Prompt for VS** so standard MSVC environment variables are exported.

### Runtime Driver Requirements
- **FTDI D2XX Library (`ftd2xx.dll`)**:
  - `tang_programmer` loads `ftd2xx.dll` dynamically at runtime using `LoadLibraryA()`.
  - You do **not** need `ftd2xx.h` or `ftd2xx.lib` at compile time.
  - The driver is installed automatically by Windows Update when plugging in a Tang Nano or can be downloaded from the official [FTDI D2XX Drivers](https://ftdichip.com/drivers/d2xx-drivers/) page.

---

## Building

Use the included PowerShell script [`build.ps1`](build.ps1) to configure and compile using CMake and MSVC:

First, unblock the script if downloaded from the web:
```powershell
Unblock-File build.ps1
```

### Standard Build (Release):
```powershell
.\build.ps1
```
Automatically configures CMake if needed and compiles `tang_programmer.dll` and `tang_programmer.exe` into `build\Release\`.

### Clean:
```powershell
.\build.ps1 -Clean
```
Deletes the `build\` directory.

### Clean & Rebuild:
```powershell
.\build.ps1 -Rebuild
```
Cleans the build directory, reconfigures CMake, and performs a full rebuild.

### Specify Configuration (Debug / Release):
```powershell
.\build.ps1 -Config Debug
```
