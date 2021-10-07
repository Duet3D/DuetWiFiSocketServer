# DuetWifiSocketServer

This is a brief description howto setup the build environment for DuetWifiSocketServer

# Setup

## Workspace

- create a folder for the eclipse workspace
- checkout the following repositories into workspace
  - git clone git@github.com:Duet3D/DuetWifiSocketServer.git
  - git clone git@github.com:Duet3D/LwipESP8266.git
  - git clone git@github.com:Duet3D/CoreESP8266.git

## Configuration

- launch Eclipse
- open created workspace
- import projects
- setup build environment variabels
  - Windows -> Preferences -> C/C++ -> Build -> Build Variables
  - add XtensaGccPath, i.e. /opt/xtensa-lx106-elf-gcc/bin or C:\toolchains\xtensa-lx106-elf-gcc\bin
  - add EspToolPath, i.e. /usr/bin/esptool or C:\toolchains\esptool\esptool.exe
  - add EspBootFile, i.e. /path/to/eboot.elf or C:\path\to\esptool.elf
- set all projects to Release build configuration

## Build

- run build

# Downloads

## Xtensa Toolchain

- https://github.com/espressif/ESP8266_RTOS_SDK#get-toolchain

## Esptool

- https://github.com/igrr/esptool-ck/

## Sources

- https://github.com/Duet3D/DuetWifiSocketServer
- https://github.com/Duet3D/LwipESP8266
- https://github.com/Duet3D/CoreESP8266

## Links

- forum - https://forum.duet3d.com/
- wiki - https://duet3d.dozuki.com/
