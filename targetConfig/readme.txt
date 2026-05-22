Resources > Linked Resources
	- Make sure CC3200_SDK_ROOT is correctly referencing the SDK location

Build > Arm Compiler > Include Options
	- Add dir to:
		${CG_TOOL_ROOT}/include
		${CC3200_SDK_ROOT}/driverlib
		${CC3200_SDK_ROOT}/inc
		${CC3200_SDK_ROOT}/example/common
		${CC3200_SDK_ROOT}/simplelink/
		${CC3200_SDK_ROOT}/simplelink/source
		${CC3200_SDK_ROOT}/simplelink/include
		${CC3200_SDK_ROOT}/simplelink_extlib/provisioninglib

Build > Arm Linker > > Basic Options
	- Link information (map) listed into <file> (--map_file, -m) 
		- Set to "${ProjName}.map"
	Set Heap size to 0x00008000
	Set C system stack size 0x00001000


Build > Arm Linker > File Search Path
	Set include library file to:
		libc.a
		${CC3200_SDK_ROOT}/driverlib/ccs/Release/driverlib.a
		${CC3200_SDK_ROOT}/simplelink/ccs/NON_OS/simplelink.a
	Set add <dir> to:
		${CG_TOOL_ROOT}/lib
		${CG_TOOL_ROOT}/include

