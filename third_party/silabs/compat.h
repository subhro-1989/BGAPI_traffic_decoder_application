
/* app.h
*
* C-side entry point for the ble_link feature test, callable from a C++
* main(). See app.c for the step-by-step routine and ble_link.h for the
* MAC byte-order note, expected output, and worst-case notes.
*/
#define _CRT_SECURE_NO_WARNINGS

#if defined(_MSC_VER) && (_MSC_VER < 1900)
#define snprintf _snprintf
#endif

#ifdef MSVC
#define inline   __inline  
#endif