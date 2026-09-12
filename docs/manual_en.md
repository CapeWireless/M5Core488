# M5Core488 User Manual
---
## 1. Introduction

M5Core488 is a standalone GPIB macro controller that operates AR488 from an M5Stack CoreS3 without requiring a PC.

AR488 is an open-source GPIB (IEEE-488) controller based on Arduino-class microcontrollers. It can send commands to GPIB instruments over USB serial.

M5Core488 keeps AR488's GPIB control functions as they are, while the M5Stack CoreS3 handles the following tasks:

- Load macros from a microSD card
- Select and run macros from the touch screen
- Run macros from a Web UI over Wi-Fi
- Record measurement results and communication data in CSV format
- Synchronize time using NTP
- Expose the microSD card to a PC as USB storage for editing

The basic system configuration is as follows.

```text
GPIB Instrument
    |
IEEE-488 / GPIB
    |
  AR488
    |
USB Serial
    |
M5Stack CoreS3
    |
    +-- LCD / Touch Operation
    +-- microSD / Macros / Logs
    +-- Wi-Fi / Web UI
    +-- USB SD Mode
```

![M5Core488 system diagram](images/config.jpg)
*M5Core488 basic system configuration*

M5Core488 itself is not tied to any specific instrument.

Instrument-specific commands are written in macro files stored on the microSD card.

This means the M5Core488 firmware does not need to be rewritten each time support for another instrument is added.

---

## 2. Requirements

### 2.1 Hardware

A basic setup requires the following:

- M5Stack CoreS3
- AR488
- GPIB-capable instrument
- microSD card
- USB cable for connecting AR488 and CoreS3
- External power supply for CoreS3 if required (9 V to 24 V)

### 2.2 AR488

M5Core488 treats AR488 as a USB serial device.

In the current implementation, the CoreS3 operates as a USB Host and communicates with AR488.

The serial communication speed is 115200 bps.

For AR488 firmware, GPIB wiring, and related settings, refer to the AR488 documentation.

![ar488-interface](images/ar488-interface.jpg)

*AR488*

![ar488-connected](images/ar488-connected.jpg)

*Example AR488 GPIB connection*

---

## 3. microSD Card Structure

M5Core488 uses the following directories on the microSD card.

```text
/
├─ macros/
│    ├─ SAMPLE.mac
│    ├─ HP*****.mac
│    └─ ...
│
├─ config/
│    ├─ wifi.ini
│    └─ macros.ini
│
└─ logs/
     ├─ 20260906.csv
     └─ ...
```

### `/macros`

Stores GPIB macro files.

The file extension is `.mac`.

### `/config/wifi.ini`

Stores Wi-Fi, NTP, Web UI, boot behavior, and related settings.

### `/config/macros.ini`

Stores the ON / OFF state of each macro.

This is an automatically generated file used to store the ON / OFF state of each macro.
It may not exist initially. If the file does not exist, all macros are treated as ON.
When a macro is switched ON or OFF from the Web UI, this file is created or updated automatically. Manual editing is normally unnecessary.

### `/logs`

Stores CSV logs created during macro execution.

If required directories do not exist, some of them are created automatically by M5Core488.

---

## 4. Startup

When the CoreS3 starts, the mode selection screen is shown first.

The two main choices are:

```text
AR488 MODE

USB SD MODE
```

![M5Core488 mode selection](images/mode-select.jpg)

*Mode selection screen shown when the CoreS3 starts*

If `auto_ar488_seconds` in `wifi.ini` is set to a value from 1 to 60, M5Core488 automatically enters AR488 MODE after the specified number of seconds.

Example:

```ini
auto_ar488_seconds=5
```

With this setting, the mode selection screen is displayed for about 5 seconds after startup. If USB SD MODE is not selected during that time, the unit automatically enters AR488 MODE.

To disable automatic mode switching, use:

```ini
auto_ar488_seconds=0
```

---

## 5. AR488 MODE

AR488 MODE is the normal mode for measurement and macro execution.

In this mode, the CoreS3 acts as a USB Host and communicates with AR488 over USB serial.

When AR488 MODE starts, USB power output from the CoreS3 is enabled and the unit waits for AR488 to connect.

AR488 does not need to be connected at boot. M5Core488 can remain in AR488 MODE and wait for it.

### 5.1 Macro List

`.mac` files in the `/macros` directory are displayed in the macro list.

The macro filename is used as the display name.

Example:

```text
HP*****.mac
RS*****.mac
COUNTER_TEST.mac
```

Names are arbitrary.

### 5.2 Macro ON / OFF

Each macro can be enabled or disabled.

A macro that is OFF remains stored on the microSD card, but it cannot be run with RUN or LOOP.

The state is saved in `/config/macros.ini`.

![ar488-mode](images/ar488-mode.jpg)

*AR488 MODE screen*

---

## 6. Macro Files

Macros are ordinary text files.

The basic rules are:

- Blank lines are ignored
- Lines beginning with `#` are comments
- Lines beginning with `@` are M5Core488 local commands
- All other executable lines are sent to AR488 as written

Example:

```text
# GPIB address
++addr 22

# Instrument command
MEAS:VOLT:DC?

# Read response
++read eoi
```

`MEAS:VOLT:DC?` is an example intended for an SCPI-compatible digital multimeter.

Actual commands, GPIB addresses, termination conditions, and other details should be adjusted according to the programming manual for the instrument being used.

### 6.1 About the Macro Language

The M5Core488 macro format is a simple way to send commands to an instrument in sequence.

At present, it is not intended to be a general-purpose scripting language such as Python.

For example, the following constructs are not implemented:

- Conditional branches such as `if` / `else`
- Loop syntax such as `for` / `while`
- Variables
- Functions
- `goto`

Repeated execution is handled by M5Core488 using `[setup]` / `[loop]` sections and the LOOP execution mode rather than by writing `for` or `while` inside a macro.

---

## 7. `@wait` Command

The currently implemented M5Core488 local command is `@wait`.

Syntax:

```text
@wait <milliseconds>
```

Example:

```text
@wait 500
```

This waits for 500 ms before proceeding to the next line.

The maximum value is 600000 ms.

```text
@wait 600000
```

This means a wait of 10 minutes.

An error occurs if the value contains non-numeric characters or exceeds the maximum value.

---

## 8. `[setup]` and `[loop]`

For repeated measurements, instrument initialization can be separated from the section that is executed repeatedly.

Example:

```text
[setup]

++addr 3
++auto 0

# Add instrument initialization here if required

[loop]

MEAS:FREQ?
++read eoi

@wait 1000
```

The example above shows a simple macro intended for an SCPI-compatible frequency counter.

The actual command set varies by instrument.

### `[setup]`

Executed only once when the macro starts.

This section is suitable for settings that do not need to be sent on every cycle, such as measurement mode, range, trigger settings, and input conditions.

### `[loop]`

With RUN, this section is executed once.

With LOOP, this section is repeated until stopped.

### Execution Flow

RUN:

```text
[setup]
   |
   v
[loop] x 1
   |
   v
End
```

LOOP:

```text
[setup] x 1
   |
   v
[loop] cycle 1
   |
[loop] cycle 2
   |
[loop] cycle 3
   |
   ...
```

### Notes

When using `[setup]` and `[loop]`, executable commands must be written inside one of those sections.

If executable commands are written outside a section while using section-based format, a macro format error occurs.

`[setup]` must appear before `[loop]`.

An empty `[loop]` section is not allowed.

---

## 9. Legacy Macro Format

Macros without `[setup]` / `[loop]` sections are also supported.

Example:

```text
++addr 3
++auto 0
MEAS:FREQ?
++read eoi
```

With this format, pressing RUN executes the entire file once.

Legacy-format macros cannot be executed with LOOP.

This prevents initialization or configuration commands from being repeated unintentionally.

---

## 10. RUN

RUN executes a macro once.

For a section-based macro, execution order is:

```text
[setup] x 1
[loop]  x 1
```

For a legacy-format macro, the entire file is executed once.

After execution, the CoreS3 display shows the completion status and log save status.

When the macro is launched from the local screen, tap the screen to return to the macro list after execution.

---

## 11. LOOP

LOOP repeatedly executes the `[loop]` section.

At the start of LOOP, `[setup]` is executed once.

After that, `[loop]` runs as cycle 1, cycle 2, cycle 3, and so on.

### 11.1 STOP

LOOP can be stopped while it is running.

STOP does not forcibly interrupt the current cycle.

The current cycle is allowed to finish, and the next cycle is not started.

```text
cycle 12 running
       |
     STOP
       |
cycle 12 completed
       |
do not start next cycle
       |
      STOP
```

This behavior avoids cutting off communication in the middle of instrument control.

### 11.2 If AR488 Is Disconnected

If the AR488 connection is lost during LOOP, LOOP is aborted.

Even if AR488 reconnects, measurement does not resume automatically.

This avoids resuming operation while the state of the instrument is unknown.

To resume, start LOOP again.

`[setup]` is executed again when LOOP is restarted.

---

## 12. Web UI

When the Web UI is enabled in `wifi.ini`, M5Core488 can be operated from a PC or smartphone on the same LAN.

Example settings:

```ini
web_enable=1
web_port=80
hostname=m5core488
```

It can normally be accessed using an address such as:

```text
http://m5core488.local/
```

If mDNS is unavailable, use the IP address instead.

![M5Core488 Web UI](images/web-ui.jpg)

*M5Core488 Web UI*

### 12.1 Status

The Web UI can display the following status information:

- AR488 connection status
- Current mode
- Auto AR488 status
- Macro execution status
- LOOP status and cycle number
- Local display time
- Clock source
- UTC time
- microSD status
- Wi-Fi connection status
- IP address
- RSSI

The Web status display is updated approximately once per second.

### 12.2 Macro Operations

The following operations are available from the Web UI:

- Macro Download
- Macro ON / OFF
- RUN
- LOOP
- STOP AFTER CYCLE
- Macro Upload

Some operations are locked while a macro is running to avoid conflicts.

### 12.3 Macro Upload

`.mac` files can be uploaded from the Web UI.

Macro syntax is checked during upload.

If a file with the same name already exists, it is replaced and one generation of `.bak` backup is retained.

---

## 13. Intended Use of the Web UI

The current Web UI does not include user authentication.

M5Core488 is therefore intended for use on a trusted LAN.

Direct exposure to the Internet, including router port forwarding, is not recommended.

For remote operation, use a trusted network environment such as a VPN.

---

## 14. Wi-Fi / NTP Settings

Wi-Fi and time-related settings are stored in `/config/wifi.ini`.

Example:

```ini
# M5Core488 Network / Time / Web configuration

ssid=YOUR_WIFI_SSID
password=YOUR_WIFI_PASSWORD

timezone=UTC+09:00

ntp1=pool.ntp.org
ntp2=time.google.com
ntp3=time.cloudflare.com

ntp_resync_hours=24

web_enable=1
web_port=80
hostname=m5core488

auto_ar488_seconds=5
```

---

## 15. Time Handling

M5Core488 treats internal time and display time separately.

The basic approach is:

```text
NTP
 |
 v
UTC
 |
 +-- CoreS3 system clock
 |
 +-- CoreS3 RTC
 |
 +-- CSV log
 |
 +-- Apply timezone offset for LCD / Web display
```

The RTC is kept in UTC.

CSV logs are also recorded in UTC.

The LCD and Web UI apply the fixed `timezone` offset for display.

Example:

```ini
timezone=UTC+09:00
```

With this setting, displayed time corresponds to UTC+09:00, such as Japan Standard Time.

The current `timezone` setting uses a fixed UTC offset.

Automatic daylight saving time (DST) switching is not supported.

---

## 16. NTP Resynchronization

Use `ntp_resync_hours` to configure the NTP resynchronization interval.

Example:

```ini
ntp_resync_hours=24
```

This attempts resynchronization every 24 hours.

```ini
ntp_resync_hours=0
```

With this setting, NTP synchronization is performed only at boot.

The maximum supported value is 720 hours.

When the Web UI is enabled, M5Core488 attempts to reconnect if the Wi-Fi connection is temporarily lost.

To avoid disturbing GPIB communication timing, Wi-Fi reconnection is not performed while a macro is running. It is deferred until a safer time.

---

## 17. CSV Logger

Communication during macro execution is saved to the microSD card in CSV format.

The filename is based on the UTC date.

Example:

```text
/logs/20260906.csv
```

The CSV columns are:

```csv
timestamp_utc,clock_source,macro,line,event,command,response
```

### 17.1 `timestamp_utc`

UTC timestamp.

Milliseconds are included.

Example:

```text
2026-09-06T18:26:46.990Z
```

### 17.2 `clock_source`

The source of the time value.

Typical states are:

```text
NTP
RTC
UNSYNCED
```

### 17.3 `macro`

The name of the macro that was executed.

### 17.4 `line`

The line number in the macro file.

### 17.5 `event`

Describes the type of event.

Typical events:

```text
START
SETUP_START
SETUP_END
CYCLE_START
CYCLE_END
AR488
LOCAL
ERROR
LOOP_STOP
END
ABORT
```

### 17.6 `command`

A command sent to AR488, or an M5Core488 local command.

### 17.7 `response`

A response returned by AR488 or the connected instrument.

Responses containing line breaks are converted into a form that is easier to handle inside CSV files.

---

## 18. Logger Errors

If a log file cannot be opened or a write error occurs, the GPIB macro normally continues running.

```text
Log save failed
     |
Measurement continues
```

This prevents the measurement itself from being stopped by a logging failure.

However, a log error is shown on the display after the measurement finishes.

For important measurements, verify that the CSV file was saved correctly after execution.

---

## 19. USB SD MODE

In USB SD MODE, the microSD card inside the CoreS3 can be accessed from a PC as USB storage.

Typical uses include:

- Editing macro files
- Editing `wifi.ini`
- Copying CSV logs
- Managing files on the microSD card

When USB SD MODE is entered, USB Host output from the CoreS3 is stopped.

The microSD card is then exposed to the PC as USB Mass Storage.

The display shows approximately the following status:

```text
USB SD MODE

SD  : OK
MSC : READY

Connect PC

PC owns SD
Reboot when done
```

![usb-sd-mode](images/usb-sd-mode.jpg)

*USB SD MODE screen*

### 19.1 Important: microSD Ownership

While USB SD MODE is active, the PC directly controls the microSD card.

During this time, M5Core488 is designed not to access the microSD filesystem normally.

If both the PC and CoreS3 modify the filesystem at the same time, the microSD card may be corrupted.

### 19.2 Exiting USB SD MODE

The following procedure is recommended when leaving USB SD MODE:

1. Confirm that all PC-side writes to the drive have completed
2. Safely eject the drive in Windows or another operating system
3. Disconnect the USB cable
4. Reboot the CoreS3

The mode is not switched dynamically. Reboot the unit to change modes.

---

## 20. USB Connection Notes

The CoreS3 USB port has a different role in AR488 MODE and USB SD MODE.

### AR488 MODE

```text
CoreS3 = USB Host
AR488  = USB Device
```

### USB SD MODE

```text
CoreS3 = USB Device
PC     = USB Host
```

The same USB port cannot perform both roles at the same time.

In AR488 MODE, the CoreS3 also outputs USB 5 V.

Depending on the USB wiring and external power arrangement, power sources may conflict. Pay attention to the power path when connecting a PC, CoreS3, and AR488 at the same time.

When flashing firmware, it is recommended to disconnect AR488 first.

---

## 21. When the Web UI Is Not Used

If the Web UI is not required, set:

```ini
web_enable=0
```

With this setting, Wi-Fi is used for NTP synchronization at startup and then turned off.

The Web Server is not started.

---

## 22. Sample Macros

### 22.1 AR488 Standalone LOOP Test

This example checks LOOP operation using AR488 alone, without a GPIB instrument connected.

```text
# M5Core488 sectioned macro test
# No GPIB instrument is required.

[setup]

++mode 1
++auto 0

@wait 500

[loop]

++ver

@wait 1000
```

When LOOP is started, `++ver` is executed repeatedly.

This is useful as an initial operation test.

### 22.2 Example for an SCPI-Compatible DMM

The following is a simple example for repeatedly reading DC voltage from an SCPI-compatible digital multimeter.

```text
[setup]

++addr 22
++auto 0

[loop]

MEAS:VOLT:DC?
++read eoi

@wait 1000
```

### 22.3 Example for an SCPI-Compatible Frequency Counter

The following is a simple example for repeatedly reading frequency from an SCPI-compatible frequency counter.

```text
[setup]

++addr 3
++auto 0

[loop]

MEAS:FREQ?
++read eoi

@wait 1000
```

These are generic examples intended to illustrate the macro format.

Actual SCPI commands, GPIB addresses, input channel selections, trigger conditions, and other settings should be adjusted according to the programming manual for the instrument being used.

---

## 23. Troubleshooting

### AR488 Does Not Become CONNECTED

Check the following:

- AR488 has the correct firmware installed
- The USB cable supports data communication
- AR488 MODE is selected
- AR488 USB serial speed matches the expected value
- CoreS3 USB Host has started correctly

AR488 can also be detected if it is connected after startup.

### Macro Does Not Appear in the List

Check the following:

- The file is stored in the `/macros` directory
- The extension is `.mac`
- The filename is valid
- The microSD card is detected correctly

### LOOP Button Cannot Be Used

LOOP is available only for a valid macro that contains a `[loop]` section.

Legacy-format macros are RUN-only.

Example:

```text
[setup]
...

[loop]
...
```

Confirm that the macro uses this format.

### Macro Format Error

Typical causes include:

- More than one `[setup]` section
- More than one `[loop]` section
- `[loop]` appears before `[setup]`
- Executable commands are written outside sections in a section-based macro
- The `[loop]` section contains no executable lines

### Web UI Does Not Open

Check the following:

- `web_enable=1`
- SSID / Password are correct
- CoreS3 is connected to Wi-Fi
- PC / Smartphone is on the same network
- `http://m5core488.local/` can be resolved
- If name resolution fails, try accessing the unit by IP address

### Time Does Not Show NTP

Check the following:

- Wi-Fi can connect
- The network can reach the NTP servers
- NTP server names in `wifi.ini` are correct

If NTP synchronization fails but the RTC value is valid, the RTC is used instead.

### Logs Are Not Saved

Check the following:

- The microSD card is detected correctly
- `/logs` can be created
- The microSD card has available free space
- The microSD card is not write-protected and the filesystem is not damaged

A macro may continue running even if logging fails.

Also check for messages such as `LOG : WRITE ERROR` on the display.

---

## 24. Current Limitations

As of Rev.1D.1, the current limitations include:

- The macro language is intentionally simple
- Conditional branches such as `if` / `else` are not implemented
- Loop syntax such as `for` / `while` is not implemented
- Variables are not implemented
- Functions are not implemented
- `goto` is not implemented
- There is no custom loop syntax inside macro files
- The primary local command is `@wait`
- The Web UI has no user authentication
- `timezone` uses a fixed UTC offset
- USB Host and USB SD MODE cannot be used simultaneously
- LOOP does not resume automatically

Some of these limitations are intentional in order to keep the design simple.

If complex conditional logic, repetition, or calculations based on measurement results are required, a PC-side solution such as Python may be more suitable.

M5Core488 is mainly intended for sending commands to instruments in sequence, repeating them at a fixed interval when needed, and recording the results.

---

## 25. Tested Development Environment

The main environment used for development and testing of Rev.1D.1 was:

```text
Board:
  M5Stack CoreS3

Arduino FQBN:
  m5stack:esp32:m5stack_cores3

M5Stack ESP32 platform:
  3.3.9

M5Unified:
  0.2.21

M5GFX:
  0.2.28

EspUsbHost:
  2.7.9
```

AR488:

```text
AR488 GPIB controller
ver. 0.53.46
22/05/2026
```

---

## 26. Design and Operating Philosophy

M5Core488 is not intended to automatically recognize and configure every possible GPIB instrument.

M5Core488 is responsible for:

```text
Read macro
    |
Send command
    |
Receive response
    |
Write log
```

The macro determines which commands are sent, in what order, and which values are read back.

This allows the same M5Core488 unit to be used with different instruments simply by preparing instrument-specific `.mac` files.

---

## 27. Modification and Extension

As long as an instrument can be handled by macros, adding another instrument does not require changes to the M5Core488 firmware.

Example:

```text
/macros/
  HP*****.mac
  RS*****.mac
  COUNTER.mac
  DMM.mac
```

The basic design policy is to keep instrument-specific processing out of the M5Core488 firmware as much as possible and place those details on the microSD card instead.

---

## 28. Closing Notes

M5Core488 is a small controller intended to make older GPIB instruments easier to use without a PC.

It does not make the instrument itself new.

Instead, AR488 and CoreS3 share the work so that operation can be as simple as:

```text
Power on
   |
Select macro
   |
RUN
```

That level of convenience is the goal.

For complex automated measurement, a PC and Python are usually the better choice.

M5Core488 is intended to cover the space just before that level of automation.

---
