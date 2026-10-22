/* CyOS functions the game calls: export numbers of the CyOS kernel, from
 * the Cybiko SDK's headers (__cyos N). A call to 0xf00000 + N becomes an
 * import of export N when tools/cybiko_mkapp.py builds the module. */
_cy_malloc = 0xf00000 + 876;
_cy_free = 0xf00000 + 610;
_cy_FileInput_ctor = 0xf00000 + 250;    /* FileInput_ctor_STUB(fi, 1) */
_cy_FileInput_open = 0xf00000 + 914;
/* Input_seek, Input_read and Input_dtor are not in the SDK's headers (its
 * bytecode runtime provides them); these numbers are the ones CyOS's own
 * applications import for them */
_cy_Input_seek = 0xf00000 + 1064;
_cy_Input_read = 0xf00000 + 988;
_cy_Input_dtor = 0xf00000 + 119;
