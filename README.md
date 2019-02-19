The following variuables muse be set at the workspace level before
building:
To set them, go to
 Windows -> Preferences -> C/C++ -> Build -> Build Variables
and click "Add..."

XtensaGccPath: The path to your xtensa toolchain bin directory.  For example...
	<sourcedir>/xtensa-lx106-elf-gcc/1.20.0-26-gb404fb9-2/bin
EspBootFile: The esp8266 boot file.  For example...
	<sourcedir>/esp8266/hardware/esp8266/2.4.1/bootloaders/eboot/eboot.elf
EspToolPath: The path to esptool.  For example...
	<sourcedir>/esp8266/tools/esptool/0.4.13/esptool.exe
